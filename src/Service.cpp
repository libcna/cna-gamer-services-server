// SPDX-License-Identifier: MS-PL
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
    return Json{{"userId",s.text(0)},{"gamertag",s.text(1)},{"displayName",s.text(1)},{"motto",s.text(2)},
        {"region",s.text(3)},{"allowOnlineSessions",s.number(4)!=0},{"gamerScore",score.number(0)},{"totalAchievements",score.number(1)},{"picture",s.text(5)}};
}
Json Service::dispatch(const Json& r,const std::string& peer) {
    const auto op=stringField(r,"op",64), game=stringField(r,"game",64), id=stringField(r,"id",64);
    const auto& a=r["args"];
    static const std::set<std::string> operations{"hello","auth.login","auth.logout","gamer.lookup","profile.get","friends.list","friends.add","friends.remove","friends.accept","presence.set","achievements.list","achievements.award","assets.read","leaderboards.read","leaderboards.definition","leaderboards.game.begin","leaderboards.game.commit"};
    if (!operations.contains(op)) throw Error("UNKNOWN_OPERATION");
    if (op=="hello") return Json{{"version",1},{"capabilities",Json::array({"identity","authentication","friends","friend-requests","presence","achievements","assets","leaderboard-reads","local-leaderboard-commit"})},{"maxMessageBytes",MaxMessageBytes}};
    Statement title(store_.db(),"SELECT id FROM titles WHERE id=?");title.bind(1,game);
    if (!title.row()) throw Error("UNKNOWN_TITLE");
    std::string user;
    const auto timestamp=now();
    if (op!="auth.login") {
        const auto token=stringField(r,"token",128);
        if (token.size()!=64) throw Error("UNAUTHENTICATED");
        Statement session(store_.db(),"SELECT user_id FROM sessions WHERE hash=? AND game_id=? AND expires>?");
        session.bind(1,sha256(token));session.bind(2,game);session.bind(3,timestamp);
        if (!session.row()) throw Error("UNAUTHENTICATED");
        user=session.text(0);
        Statement seen(store_.db(),"UPDATE sessions SET last_seen=? WHERE hash=?");seen.bind(1,timestamp);seen.bind(2,sha256(token));(void)seen.row();
    }
    if (op=="auth.login") {
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
        Statement cap(store_.db(),"SELECT COUNT(*) FROM sessions WHERE user_id=? AND expires>?");cap.bind(1,user);cap.bind(2,timestamp);(void)cap.row();
        if (cap.number(0)>=32) throw Error("LIMIT_EXCEEDED");
        const auto token=randomHex(32);
        Statement insert(store_.db(),"INSERT INTO sessions(hash,user_id,game_id,expires,last_seen) VALUES(?,?,?,?,?)");
        insert.bind(1,sha256(token));insert.bind(2,user);insert.bind(3,game);insert.bind(4,timestamp+3600);insert.bind(5,timestamp);(void)insert.row();
        return Json{{"identity",identity(user)},{"token",token},{"expires",timestamp+3600}};
    }
    if (op=="auth.logout") {
        Statement s(store_.db(),"DELETE FROM sessions WHERE hash=?");s.bind(1,sha256(stringField(r,"token",128)));(void)s.row();return Json::object();
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
        Statement s(store_.db(),"SELECT u.id,u.gamertag,EXISTS(SELECT 1 FROM sessions z WHERE z.user_id=u.id AND z.expires>? AND z.last_seen>?),COALESCE(p.mode,0),COALESCE(p.text,''),EXISTS(SELECT 1 FROM friends f WHERE f.user_id=? AND f.friend_id=u.id),EXISTS(SELECT 1 FROM friends f WHERE f.friend_id=? AND f.user_id=u.id) FROM users u LEFT JOIN presence p ON p.user_id=u.id AND p.game_id=? WHERE u.id IN (SELECT friend_id FROM friends WHERE user_id=? UNION SELECT user_id FROM friends WHERE friend_id=?) ORDER BY u.gamertag LIMIT 257");
        s.bind(1,timestamp);s.bind(2,timestamp-90);s.bind(3,user);s.bind(4,user);s.bind(5,game);s.bind(6,user);s.bind(7,user);
        Json friends=Json::array();
        while(s.row()) {
            if(friends.size()>=256)throw Error("LIMIT_EXCEEDED");
            const bool accepted=s.number(5)&&s.number(6), online=accepted&&s.number(2);
            friends.push_back(Json{{"userId",s.text(0)},{"gamertag",s.text(1)},{"online",online},
                {"presenceMode",online?s.number(3):0},{"presenceText",online?s.text(4):""},
                {"accepted",accepted},{"requestSent",!accepted&&s.number(5)!=0},{"requestReceived",!accepted&&s.number(6)!=0}});
        }
        return Json{{"friends",friends}};
    }
    if(op=="assets.read") {
        const auto hash=stringField(a,"hash",64);
        if(hash.size()!=64||hash.find_first_not_of("0123456789abcdef")!=std::string::npos)throw Error("INVALID_ARGUMENT");
        for(const auto* key:{"offset","length"})if(!a.contains(key)||!a[key].is_number_integer()||a[key]<0)throw Error("INVALID_ARGUMENT");
        const auto offset=a["offset"].get<long long>(),length=a["length"].get<long long>();
        if(offset>16777216||length<1||length>12288)throw Error("LIMIT_EXCEEDED");
        Statement authorized(store_.db(),"SELECT 1 FROM title_assets WHERE game_id=? AND hash=? UNION SELECT 1 FROM users WHERE picture=? LIMIT 1");
        authorized.bind(1,game);authorized.bind(2,hash);authorized.bind(3,hash);if(!authorized.row())throw Error("NOT_FOUND");
        Statement asset(store_.db(),"SELECT size,mime,substr(bytes,?,?) FROM assets WHERE hash=?");
        asset.bind(1,offset+1);asset.bind(2,length);asset.bind(3,hash);if(!asset.row())throw Error("NOT_FOUND");
        if(offset>=asset.number(0))throw Error("INVALID_ARGUMENT");
        const auto bytes=asset.blob(2);constexpr char digits[]="0123456789abcdef";std::string encoded;encoded.reserve(bytes.size()*2);
        for(unsigned char byte:bytes){encoded+=digits[byte>>4];encoded+=digits[byte&15];}
        return Json{{"hash",hash},{"size",asset.number(0)},{"mime",asset.text(1)},{"offset",offset},{"hex",encoded}};
    }
    if(op=="leaderboards.game.begin")return beginLeaderboardGame(user,game,a);
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
        Statement definition(store_.db(),"SELECT key FROM achievements WHERE game_id=? AND key=?");definition.bind(1,game);definition.bind(2,key);
        if(!definition.row())throw Error("NOT_FOUND");
        Statement s(store_.db(),"INSERT OR IGNORE INTO earned(user_id,game_id,key,ticks) VALUES(?,?,?,?)");
        s.bind(1,user);s.bind(2,game);s.bind(3,key);s.bind(4,(timestamp+62135596800LL)*10000000LL);(void)s.row();return Json::object();
    }
    Statement s(store_.db(),"SELECT a.key,a.name,a.description,a.how_to_earn,a.score,a.display,a.picture,COALESCE(e.ticks,0) FROM achievements a LEFT JOIN earned e ON e.game_id=a.game_id AND e.key=a.key AND e.user_id=? WHERE a.game_id=? ORDER BY a.key LIMIT 129");
    s.bind(1,user);s.bind(2,game);Json achievements=Json::array();
    while(s.row()) { if(achievements.size()>=128)throw Error("LIMIT_EXCEEDED");achievements.push_back(Json{{"key",s.text(0)},{"name",s.text(1)},{"description",s.text(2)},{"howToEarn",s.text(3)},{"score",s.number(4)},{"display",s.number(5)!=0},{"picture",s.text(6)},{"earnedTicks",s.number(7)}}); }
    return Json{{"achievements",achievements}};
}
}
