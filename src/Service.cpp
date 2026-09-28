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
    Statement s(store_.db(),"SELECT id,gamertag,motto,region,online_allowed FROM users WHERE id=?");s.bind(1,id);
    if (!s.row()) throw Error("NOT_FOUND");
    Statement score(store_.db(),"SELECT COALESCE(SUM(a.score),0),COUNT(*) FROM earned e JOIN achievements a ON a.game_id=e.game_id AND a.key=e.key WHERE e.user_id=?");
    score.bind(1,id);(void)score.row();
    return Json{{"userId",s.text(0)},{"gamertag",s.text(1)},{"displayName",s.text(1)},{"motto",s.text(2)},
        {"region",s.text(3)},{"allowOnlineSessions",s.number(4)!=0},{"gamerScore",score.number(0)},{"totalAchievements",score.number(1)}};
}
Json Service::dispatch(const Json& r,const std::string& peer) {
    const auto op=stringField(r,"op",64), game=stringField(r,"game",64), id=stringField(r,"id",64);
    const auto& a=r["args"];
    static const std::set<std::string> operations{"hello","auth.login","auth.logout","gamer.lookup","profile.get","friends.list","friends.add","friends.remove","presence.set","achievements.list","achievements.award"};
    if (!operations.contains(op)) throw Error("UNKNOWN_OPERATION");
    if (op=="hello") return Json{{"version",1},{"capabilities",Json::array({"identity","authentication","friends","presence","achievements"})},{"maxMessageBytes",MaxMessageBytes}};
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
    if (op=="friends.add" || op=="friends.remove") {
        Statement target(store_.db(),"SELECT id FROM users WHERE gamertag=?");target.bind(1,stringField(a,"gamertag",32));
        if (!target.row()) throw Error("NOT_FOUND");
        if (target.text(0)==user) throw Error("INVALID_ARGUMENT");
        Statement s(store_.db(),op=="friends.add"?"INSERT OR IGNORE INTO friends(user_id,friend_id) VALUES(?,?)":"DELETE FROM friends WHERE user_id=? AND friend_id=?");
        s.bind(1,user);s.bind(2,target.text(0));(void)s.row();return Json::object();
    }
    if (op=="friends.list") {
        Statement s(store_.db(),"SELECT u.id,u.gamertag,EXISTS(SELECT 1 FROM sessions z WHERE z.user_id=u.id AND z.expires>? AND z.last_seen>?),COALESCE(p.mode,0),COALESCE(p.text,'') FROM friends f JOIN users u ON u.id=f.friend_id LEFT JOIN presence p ON p.user_id=u.id AND p.game_id=? WHERE f.user_id=? ORDER BY u.gamertag LIMIT 257");
        s.bind(1,timestamp);s.bind(2,timestamp-90);s.bind(3,game);s.bind(4,user);Json friends=Json::array();
        while(s.row()) { if(friends.size()>=256)throw Error("LIMIT_EXCEEDED");friends.push_back(Json{{"userId",s.text(0)},{"gamertag",s.text(1)},{"online",s.number(2)!=0},{"presenceMode",s.number(3)},{"presenceText",s.text(4)}}); }
        return Json{{"friends",friends}};
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
