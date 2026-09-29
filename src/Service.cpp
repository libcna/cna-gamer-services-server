// SPDX-License-Identifier: MIT
#include "CnaService/Service.hpp"
#include <openssl/crypto.h>
#include <set>

namespace CnaService {
Service::Service(const std::string& database):store_(database) {}
std::string Service::handle(std::string_view bytes,std::string_view peer) {
    std::string id;
    try {
        const auto request=parse(bytes);validateRequest(request);id=stringField(request,"id",64);
        std::lock_guard lock(mutex_);
        auto result=response(id,"OK",dispatch(request,std::string(peer))).dump();
        if (result.size()>MaxMessageBytes) throw Error("LIMIT_EXCEEDED");
        return result;
    } catch (const Error& e) { return response(id,e.code()).dump(); }
      catch (...) { return response(id,"INTERNAL_ERROR").dump(); }
}
Json Service::identity(const std::string& id) {
    Statement s(store_.db(),"SELECT id,gamertag,motto,region,online_allowed,picture FROM users WHERE id=?");s.bind(1,id);
    if (!s.row()) throw Error("NOT_FOUND");
    Statement score(store_.db(),"SELECT COALESCE(SUM(a.score),0),COUNT(*) FROM earned e JOIN achievements a ON a.game_id=e.game_id AND a.key=e.key WHERE e.user_id=?");
    score.bind(1,id);(void)score.row();
    // A title counts as played once the account has presence, an earned achievement or a leaderboard row in it.
    Statement titles(store_.db(),"SELECT COUNT(*) FROM (SELECT game_id FROM presence WHERE user_id=?1 UNION "
        "SELECT game_id FROM earned WHERE user_id=?1 UNION SELECT game_id FROM leaderboard_entries WHERE user_id=?1)");
    titles.bind(1,id);(void)titles.row();
    return Json{{"userId",s.text(0)},{"gamertag",s.text(1)},{"displayName",s.text(1)},{"motto",s.text(2)},
        {"region",s.text(3)},{"allowOnlineSessions",s.number(4)!=0},{"gamerScore",score.number(0)},{"totalAchievements",score.number(1)},
        {"titlesPlayed",titles.number(0)},{"picture",s.text(5)}};
}
Json Service::dispatch(const Json& r,const std::string& peer) {
    const auto op=stringField(r,"op",64), game=stringField(r,"game",64), id=stringField(r,"id",64);
    const auto& a=r["args"];
    static const std::set<std::string> operations{"hello","auth.login","auth.logout","auth.refresh","auth.ping","gamer.lookup","profile.get","profile.gameDefaults","profile.setGameDefaults","friends.list","friends.add","friends.remove","friends.accept","presence.set","presence.status","achievements.list","achievements.award","assets.read","leaderboards.read","leaderboards.definition","leaderboards.game.begin","leaderboards.game.commit","leaderboards.game.abort","sessions.relayTicket","sessions.create","sessions.find","sessions.get","sessions.touch","sessions.update","sessions.join","sessions.joinInvited","sessions.leave","sessions.remove","sessions.addMembers","invites.send","invites.list","invites.get","invites.accept","invites.dismiss","messages.send","messages.list","messages.read","messages.delete","reviews.submit","avatars.get","avatars.set","avatars.catalog"};
    if (!operations.contains(op)) throw Error("UNKNOWN_OPERATION");
    if (op=="hello") return Json{{"version",1},{"capabilities",Json::array({"identity","authentication","session-refresh","heartbeat","friends","friend-requests","presence","presence-status","game-defaults","achievements","assets","leaderboard-reads","local-leaderboard-commit","leaderboard-epoch-abort","ranked-arbitration","messages","player-reviews","avatars","session-directory","session-removal","host-migration","session-add-members","session-invitations","relay-tickets","relay"})},{"maxMessageBytes",MaxMessageBytes}};
    Statement title(store_.db(),"SELECT id FROM titles WHERE id=?");title.bind(1,game);
    if (!title.row()) throw Error("UNKNOWN_TITLE");
    std::string user;
    const auto timestamp=now();
    if (op!="auth.login"&&op!="auth.refresh") {
        const auto token=stringField(r,"token",128);
        if (token.size()!=64) throw Error("UNAUTHENTICATED");
        Statement session(store_.db(),"SELECT user_id FROM sessions WHERE hash=? AND game_id=? AND expires>?");
        session.bind(1,sha256(token));session.bind(2,game);session.bind(3,timestamp);
        if (!session.row()) throw Error("UNAUTHENTICATED");
        user=session.text(0);
        Statement seen(store_.db(),"UPDATE sessions SET last_seen=? WHERE hash=?");seen.bind(1,timestamp);seen.bind(2,sha256(token));(void)seen.row();
    }
    if (op=="auth.login"||op=="auth.refresh") {
        for (auto it=loginRates_.begin();it!=loginRates_.end();) {
            if (timestamp-it->second.start>=60) it=loginRates_.erase(it);else ++it;
        }
        if (loginRates_.size()>=4096 && !loginRates_.contains(peer)) throw Error("RATE_LIMITED");
        auto& rate=loginRates_[peer];if (rate.count++>=10) throw Error("RATE_LIMITED");if (!rate.start) rate.start=timestamp;
    }
    Statement trim(store_.db(),"DELETE FROM request_ids WHERE created<?");trim.bind(1,timestamp-86400);(void)trim.row();
    Statement count(store_.db(),"SELECT COUNT(*) FROM request_ids WHERE game_id=?");count.bind(1,game);(void)count.row();
    if (count.number(0)>=100000) throw Error("LIMIT_EXCEEDED");
    try { Statement nonce(store_.db(),"INSERT INTO request_ids(game_id,id,created) VALUES(?,?,?)");nonce.bind(1,game);nonce.bind(2,id);nonce.bind(3,timestamp);(void)nonce.row(); }
    catch (const Error& e) { if (e.code()=="CONFLICT") throw Error("DUPLICATE_REQUEST");throw; }
    if (op=="auth.login") {
        const auto username=stringField(a,"username",64),password=stringField(a,"password",256);
        if (!identifier(username)||password.size()<8) throw Error("AUTHENTICATION_FAILED");
        Statement s(store_.db(),"SELECT id,salt,verifier FROM users WHERE username=?");s.bind(1,username);
        const bool found=s.row();
        const auto computed=passwordHash(password,found?s.text(1):"00000000000000000000000000000000");
        const auto expected=found?s.text(2):std::string(64,'0');
        if (!found || expected.size()!=computed.size() || CRYPTO_memcmp(expected.data(),computed.data(),computed.size())!=0)
            throw Error("AUTHENTICATION_FAILED");
        user=s.text(0);
        return issueCredentials(user,game);
    }
    if(op=="auth.refresh")return refreshCredentials(game,a);
    if(op=="auth.ping")return Json{{"serverTime",timestamp}};
    if (op=="auth.logout") {
        const auto hash=sha256(stringField(r,"token",128));Statement scope(store_.db(),"SELECT refresh_family FROM sessions WHERE hash=?");scope.bind(1,hash);(void)scope.row();
        const auto family=scope.text(0);if(!family.empty())revokeFamily(family);
        else {Statement remove(store_.db(),"DELETE FROM sessions WHERE hash=?");remove.bind(1,hash);(void)remove.row();}
        return Json::object();
    }
    if (op=="gamer.lookup" || op=="profile.get") {
        const auto gamertag=stringField(a,"gamertag",32);
        Statement s(store_.db(),"SELECT id FROM users WHERE gamertag=?");s.bind(1,gamertag);
        if (!s.row()) throw Error("NOT_FOUND");
        return identity(s.text(0));
    }
    if (op=="presence.set") {
        if (!a.contains("mode") || !a["mode"].is_number_integer() || a["mode"]<0 || a["mode"]>255) throw Error("INVALID_ARGUMENT");
        Statement s(store_.db(),"INSERT INTO presence(user_id,game_id,mode,text) VALUES(?,?,?,?) ON CONFLICT(user_id,game_id) DO UPDATE SET mode=excluded.mode,text=excluded.text");
        s.bind(1,user);s.bind(2,game);s.bind(3,a["mode"].get<long long>());s.bind(4,stringField(a,"text",256));(void)s.row();return Json::object();
    }
    if (op=="profile.gameDefaults"||op=="profile.setGameDefaults") {
        // XNA SignedInGamer.GameDefaults: the caller's own preferred game settings, account-wide.
        if(op=="profile.setGameDefaults") {
            if(a.size()!=1||!a.contains("gameDefaults"))throw Error("INVALID_ARGUMENT");
            validateGameDefaults(a["gameDefaults"]);
            Statement update(store_.db(),"UPDATE users SET game_defaults=? WHERE id=?");update.bind(1,a["gameDefaults"].dump());update.bind(2,user);(void)update.row();
        } else if(!a.empty())throw Error("INVALID_ARGUMENT");
        Statement read(store_.db(),"SELECT game_defaults FROM users WHERE id=?");read.bind(1,user);(void)read.row();
        return Json{{"gameDefaults",parse(read.text(0))}};
    }
    if (op=="presence.status") {
        // Account-wide, like the console's online status; friends see it only while online.
        const auto status=stringField(a,"status",8);
        if(status!="online"&&status!="away"&&status!="busy") throw Error("INVALID_ARGUMENT");
        Statement s(store_.db(),"UPDATE users SET status=? WHERE id=?");s.bind(1,status);s.bind(2,user);(void)s.row();
        return Json{{"status",status}};
    }
    if (op=="friends.add" || op=="friends.remove" || op=="friends.accept") {
        Statement target(store_.db(),"SELECT id FROM users WHERE gamertag=?");target.bind(1,stringField(a,"gamertag",32));
        if (!target.row()) throw Error("NOT_FOUND");
        if (target.text(0)==user) throw Error("INVALID_ARGUMENT");
        const auto friendId=target.text(0);
        if(op=="friends.accept") {
            Statement incoming(store_.db(),"SELECT 1 FROM friends WHERE user_id=? AND friend_id=?");
            incoming.bind(1,friendId);incoming.bind(2,user);if(!incoming.row())throw Error("INVALID_STATE");
        }
        if(op!="friends.remove") {
            Statement existing(store_.db(),"SELECT 1 FROM friends WHERE user_id=? AND friend_id=?");
            existing.bind(1,user);existing.bind(2,friendId);
            if(!existing.row())for(const auto& account:{user,friendId}) {
                Statement cap(store_.db(),"SELECT COUNT(*) FROM friends WHERE user_id=? OR friend_id=?");
                cap.bind(1,account);cap.bind(2,account);(void)cap.row();if(cap.number(0)>=256)throw Error("LIMIT_EXCEEDED");
            }
        }
        Statement s(store_.db(),op=="friends.remove"?
            "DELETE FROM friends WHERE (user_id=? AND friend_id=?) OR (user_id=? AND friend_id=?)":
            "INSERT OR IGNORE INTO friends(user_id,friend_id) VALUES(?,?)");
        s.bind(1,user);s.bind(2,friendId);
        if(op=="friends.remove"){s.bind(3,friendId);s.bind(4,user);}
        (void)s.row();return Json::object();
    }
    if (op=="friends.list") {
        Statement s(store_.db(),"SELECT u.id,u.gamertag,EXISTS(SELECT 1 FROM sessions z WHERE z.user_id=u.id AND z.expires>? AND z.last_seen>?),COALESCE(p.mode,0),COALESCE(p.text,''),EXISTS(SELECT 1 FROM friends f WHERE f.user_id=? AND f.friend_id=u.id),EXISTS(SELECT 1 FROM friends f WHERE f.friend_id=? AND f.user_id=u.id),u.status FROM users u LEFT JOIN presence p ON p.user_id=u.id AND p.game_id=? WHERE u.id IN (SELECT friend_id FROM friends WHERE user_id=? UNION SELECT user_id FROM friends WHERE friend_id=?) ORDER BY u.gamertag LIMIT 257");
        s.bind(1,timestamp);s.bind(2,timestamp-90);s.bind(3,user);s.bind(4,user);s.bind(5,game);s.bind(6,user);s.bind(7,user);
        Json friends=Json::array();
        while(s.row()) {
            if(friends.size()>=256)throw Error("LIMIT_EXCEEDED");
            const bool accepted=s.number(5)&&s.number(6), online=accepted&&s.number(2);
            Json row{{"userId",s.text(0)},{"gamertag",s.text(1)},{"online",online},
                {"presenceMode",online?s.number(3):0},{"presenceText",online?s.text(4):""},
                {"away",online&&s.text(7)=="away"},{"busy",online&&s.text(7)=="busy"},
                {"accepted",accepted},{"requestSent",!accepted&&s.number(5)!=0},{"requestReceived",!accepted&&s.number(6)!=0},
                {"joinable",false},{"inviteReceivedFrom",false},{"inviteSentTo",false},{"inviteAccepted",false},{"inviteRejected",false}};
            if(accepted) {
                // Joinable: in this title's player-match session that is live, admits joiners now and has a public slot.
                Statement joinable(store_.db(),"SELECT EXISTS(SELECT 1 FROM directory_members m JOIN directory_sessions d ON d.id=m.session_id "
                    "WHERE m.game_id=? AND m.user_id=? AND d.kind='player' AND d.expires>? AND (d.state='lobby' OR d.allow_join=1) "
                    "AND (SELECT COUNT(*) FROM directory_members o WHERE o.session_id=d.id AND o.private_slot=0)<d.max_gamers-d.private_slots)");
                joinable.bind(1,game);joinable.bind(2,s.text(0));joinable.bind(3,timestamp);(void)joinable.row();
                // This title's unexpired invitations between the two, as XNA's friend state reports them.
                Statement invites(store_.db(),"SELECT "
                    "EXISTS(SELECT 1 FROM session_invitations WHERE game_id=?1 AND sender_id=?2 AND recipient_id=?3 AND status='pending' AND expires>?4),"
                    "EXISTS(SELECT 1 FROM session_invitations WHERE game_id=?1 AND sender_id=?3 AND recipient_id=?2 AND status='pending' AND expires>?4),"
                    "EXISTS(SELECT 1 FROM session_invitations WHERE game_id=?1 AND sender_id=?3 AND recipient_id=?2 AND status IN ('accepted','used') AND expires>?4),"
                    "EXISTS(SELECT 1 FROM session_invitations WHERE game_id=?1 AND sender_id=?3 AND recipient_id=?2 AND status='dismissed' AND expires>?4)");
                invites.bind(1,game);invites.bind(2,s.text(0));invites.bind(3,user);invites.bind(4,timestamp);(void)invites.row();
                row["joinable"]=online&&joinable.number(0)!=0;
                row["inviteReceivedFrom"]=invites.number(0)!=0;row["inviteSentTo"]=invites.number(1)!=0;
                row["inviteAccepted"]=invites.number(2)!=0;row["inviteRejected"]=invites.number(3)!=0;
            }
            friends.push_back(std::move(row));
        }
        return Json{{"friends",friends}};
    }
    if(op=="assets.read") {
        const auto hash=stringField(a,"hash",64);
        if(hash.size()!=64||hash.find_first_not_of("0123456789abcdef")!=std::string::npos)throw Error("INVALID_ARGUMENT");
        for(const auto* key:{"offset","length"})if(!a.contains(key)||!a[key].is_number_integer()||a[key]<0)throw Error("INVALID_ARGUMENT");
        const auto offset=a["offset"].get<long long>(),length=a["length"].get<long long>();
        if(offset>16777216||length<1||length>12288)throw Error("LIMIT_EXCEEDED");
        // Title assets, account pictures and every imported avatar catalog file are readable.
        Statement authorized(store_.db(),"SELECT 1 FROM title_assets WHERE game_id=? AND hash=? UNION SELECT 1 FROM users WHERE picture=? "
            "UNION SELECT 1 FROM avatar_catalog_assets WHERE hash=? LIMIT 1");
        authorized.bind(1,game);authorized.bind(2,hash);authorized.bind(3,hash);authorized.bind(4,hash);
        if(!authorized.row())throw Error("NOT_FOUND");
        Statement asset(store_.db(),"SELECT size,mime,substr(bytes,?,?) FROM assets WHERE hash=?");
        asset.bind(1,offset+1);asset.bind(2,length);asset.bind(3,hash);if(!asset.row())throw Error("NOT_FOUND");
        if(offset>=asset.number(0))throw Error("INVALID_ARGUMENT");
        const auto bytes=asset.blob(2);constexpr char digits[]="0123456789abcdef";std::string encoded;encoded.reserve(bytes.size()*2);
        for(unsigned char byte:bytes){encoded+=digits[byte>>4];encoded+=digits[byte&15];}
        return Json{{"hash",hash},{"size",asset.number(0)},{"mime",asset.text(1)},{"offset",offset},{"hex",encoded}};
    }
    if(op=="messages.send") {
        // Guide Compose: 1..100 recipients, text <=256 UTF-8 bytes; bounded inbox and sender rate.
        const auto text=stringField(a,"text",256);
        if(!a.contains("gamertags")||!a["gamertags"].is_array()||a["gamertags"].empty()||a["gamertags"].size()>100)throw Error("INVALID_ARGUMENT");
        Statement rate(store_.db(),"SELECT COUNT(*) FROM messages WHERE sender_id=? AND created>?");rate.bind(1,user);rate.bind(2,timestamp-3600);(void)rate.row();
        if(rate.number(0)+static_cast<long long>(a["gamertags"].size())>200)throw Error("RATE_LIMITED");
        std::vector<std::string> recipients;std::set<std::string> seen;
        for(const auto& tag:a["gamertags"]) {
            if(!tag.is_string())throw Error("INVALID_ARGUMENT");
            Statement target(store_.db(),"SELECT id FROM users WHERE gamertag=?");target.bind(1,tag.get<std::string>());
            if(!target.row())throw Error("NOT_FOUND");
            if(target.text(0)==user||!seen.insert(target.text(0)).second)throw Error("INVALID_ARGUMENT");
            Statement inbox(store_.db(),"SELECT COUNT(*) FROM messages WHERE recipient_id=?");inbox.bind(1,target.text(0));(void)inbox.row();
            if(inbox.number(0)>=100)throw Error("LIMIT_EXCEEDED");
            recipients.push_back(target.text(0));
        }
        store_.exec("BEGIN IMMEDIATE");
        try {
            for(const auto& recipient:recipients) {
                Statement insert(store_.db(),"INSERT INTO messages(id,sender_id,recipient_id,text,created) VALUES(?,?,?,?,?)");
                insert.bind(1,randomHex(16));insert.bind(2,user);insert.bind(3,recipient);insert.bind(4,text);insert.bind(5,timestamp);(void)insert.row();
            }
            store_.exec("COMMIT");
        }catch(...){store_.exec("ROLLBACK");throw;}
        return Json{{"sent",recipients.size()}};
    }
    if(op=="messages.list") {
        const auto start=integerField(a,"start",0,100),limit=integerField(a,"limit",1,32);
        Statement total(store_.db(),"SELECT COUNT(*),SUM(read=0) FROM messages WHERE recipient_id=?");total.bind(1,user);(void)total.row();
        Statement page(store_.db(),"SELECT m.id,u.gamertag,m.text,m.created,m.read FROM messages m JOIN users u ON u.id=m.sender_id WHERE m.recipient_id=? ORDER BY m.created DESC,m.id LIMIT ? OFFSET ?");
        page.bind(1,user);page.bind(2,limit);page.bind(3,start);
        Json rows=Json::array();
        while(page.row())rows.push_back(Json{{"message",page.text(0)},{"sender",page.text(1)},{"text",page.text(2)},{"created",page.number(3)},{"read",page.number(4)!=0}});
        return Json{{"start",start},{"total",total.number(0)},{"unread",total.number(1)},{"messages",rows}};
    }
    if(op=="messages.read"||op=="messages.delete") {
        const auto id=stringField(a,"message",32);
        if(id.size()!=32||id.find_first_not_of("0123456789abcdef")!=std::string::npos)throw Error("INVALID_ARGUMENT");
        Statement owned(store_.db(),"SELECT 1 FROM messages WHERE id=? AND recipient_id=?");owned.bind(1,id);owned.bind(2,user);
        if(!owned.row())throw Error("NOT_FOUND");
        Statement change(store_.db(),op=="messages.read"?"UPDATE messages SET read=1 WHERE id=?":"DELETE FROM messages WHERE id=?");change.bind(1,id);(void)change.row();
        return Json::object();
    }
    if(op=="reviews.submit") {
        // Guide Player Review: prefer/avoid feedback; "clear" withdraws it. Avoided hosts are not
        // offered by this reviewer's matchmaking searches.
        const auto rating=stringField(a,"rating",16);
        if(rating!="prefer"&&rating!="avoid"&&rating!="clear")throw Error("INVALID_ARGUMENT");
        Statement target(store_.db(),"SELECT id FROM users WHERE gamertag=?");target.bind(1,stringField(a,"gamertag",32));
        if(!target.row())throw Error("NOT_FOUND");
        if(target.text(0)==user)throw Error("INVALID_ARGUMENT");
        Statement cap(store_.db(),"SELECT COUNT(*) FROM player_reviews WHERE reviewer_id=?");cap.bind(1,user);(void)cap.row();
        if(rating!="clear"&&cap.number(0)>=1024)throw Error("LIMIT_EXCEEDED");
        Statement change(store_.db(),rating=="clear"?"DELETE FROM player_reviews WHERE reviewer_id=? AND subject_id=?":
            "INSERT INTO player_reviews(reviewer_id,subject_id,rating,updated) VALUES(?,?,?,?) ON CONFLICT(reviewer_id,subject_id) DO UPDATE SET rating=excluded.rating,updated=excluded.updated");
        change.bind(1,user);change.bind(2,target.text(0));if(rating!="clear"){change.bind(3,rating);change.bind(4,timestamp);}(void)change.row();
        return Json::object();
    }
    if(op.starts_with("avatars."))return avatars(user,op,a,timestamp);
    if(op.starts_with("invites."))return invitations(user,game,op,a);
    if(op=="sessions.relayTicket")return issueRelayTicket(user,game,a);
    if(op.starts_with("sessions."))return directory(user,game,op,a);
    if(op=="leaderboards.game.begin")return beginLeaderboardGame(user,game,a);
    if(op=="leaderboards.game.abort")return abortLeaderboardGame(user,game,a);
    if(op=="leaderboards.game.commit")return commitLeaderboardGame(user,game,a);
    if(op=="leaderboards.read")return readLeaderboard(game,a);
    if(op=="leaderboards.definition") {
        Statement board(store_.db(),"SELECT ascending,aggregation,arbitrated,columns FROM leaderboards WHERE game_id=? AND key=? AND mode=?");
        board.bind(1,game);board.bind(2,stringField(a,"key",64));board.bind(3,integerField(a,"mode",-2147483648LL,2147483647LL));
        if(!board.row())throw Error("NOT_FOUND");
        return Json{{"ascending",board.number(0)!=0},{"aggregation",board.text(1)},{"arbitrated",board.number(2)!=0},{"columns",parse(board.text(3))}};
    }
    if (op=="achievements.award") {
        const auto key=stringField(a,"key",64);if(!identifier(key))throw Error("INVALID_ARGUMENT");
        Statement definition(store_.db(),"SELECT key,name FROM achievements WHERE game_id=? AND key=?");definition.bind(1,game);definition.bind(2,key);
        if(!definition.row())throw Error("NOT_FOUND");
        Statement s(store_.db(),"INSERT OR IGNORE INTO earned(user_id,game_id,key,ticks) VALUES(?,?,?,?)");
        s.bind(1,user);s.bind(2,game);s.bind(3,key);s.bind(4,(timestamp+62135596800LL)*10000000LL);(void)s.row();
        // Whether this call earned it (a client shows "achievement unlocked" once), and its name.
        return Json{{"awarded",sqlite3_changes(store_.db())>0},{"name",definition.text(1)}};
    }
    Statement s(store_.db(),"SELECT a.key,a.name,a.description,a.how_to_earn,a.score,a.display,a.picture,COALESCE(e.ticks,0) FROM achievements a LEFT JOIN earned e ON e.game_id=a.game_id AND e.key=a.key AND e.user_id=? WHERE a.game_id=? ORDER BY a.key LIMIT 129");
    s.bind(1,user);s.bind(2,game);Json achievements=Json::array();
    while(s.row()) { if(achievements.size()>=128)throw Error("LIMIT_EXCEEDED");achievements.push_back(Json{{"key",s.text(0)},{"name",s.text(1)},{"description",s.text(2)},{"howToEarn",s.text(3)},{"score",s.number(4)},{"display",s.number(5)!=0},{"picture",s.text(6)},{"earnedTicks",s.number(7)}}); }
    return Json{{"achievements",achievements}};
}
}
