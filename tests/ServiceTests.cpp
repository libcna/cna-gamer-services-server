// SPDX-License-Identifier: MIT
#include "CnaService/Service.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
using namespace CnaService;
int assertions=0;
void check(bool value,const char* reason) {++assertions;if(!value)throw std::runtime_error(reason);}
Json call(Service& s,std::string game,std::string op,Json args=Json::object(),std::string token={},std::string id={}) {
    static int sequence=0;if(id.empty())id="unit-"+std::to_string(++sequence);
    Json r{{"v",1},{"id",id},{"game",game},{"op",op},{"args",args}};if(!token.empty())r["token"]=token;
    return parse(s.handle(r.dump(),"unit-peer"));
}
int main() {
    auto path=std::filesystem::current_path()/"service-unit.sqlite3";
    auto clean=[&]{for(auto suffix:{"","-wal","-shm"})std::filesystem::remove(path.string()+suffix);};
    try {
        clean();std::ifstream input(CNA_PROTOCOL_VECTORS);
        for(const auto& v:Json::parse(input)) {
            std::string error="OK";try{validateRequest(parse(v["input"].get<std::string>()));}catch(const Error& e){error=e.code();}
            check(error==v["error"].get<std::string>(),"golden vector");
        }
        const std::string valid=R"({"v":1,"id":"one","game":"sample","op":"hello","args":{}})";
        for(std::size_t n=0;n<valid.size();++n) {
            bool refused=false;try{validateRequest(parse(valid.substr(0,n)));}catch(const Error&){refused=true;}check(refused,"truncation");
        }
        for(const auto& invalid:{std::string("\xff"),std::string(65537,'a'),std::string(20,'[')+std::string(20,']')}) {
            bool refused=false;try{(void)parse(invalid);}catch(const Error&){refused=true;}check(refused,"resource/UTF8 bounds");
        }
        for(bool composite:{false,true}) {
            auto array=Json::array();for(int i=0;i<257;++i)array.push_back(composite?Json::object():Json(i));
            bool refused=false;try{(void)parse(array.dump());}catch(const Error& e){refused=e.code()=="LIMIT_EXCEEDED";}check(refused,"array early count limit");
        }
        Json object=Json::object();for(int i=0;i<257;++i)object[std::to_string(i)]=0;
        bool oversized=false;try{(void)parse(object.dump());}catch(const Error& e){oversized=e.code()=="LIMIT_EXCEEDED";}check(oversized,"object early field limit");
        std::mt19937 random(0x434e41);
        for(int trial=0;trial<5000;++trial) {
            std::string mutated=valid;
            for(int edits=0;edits<1+trial%4;++edits)mutated[random()%mutated.size()]=static_cast<char>(random()%256);
            auto outcome=[&]{try{validateRequest(parse(mutated));return std::string("OK");}catch(const Error& e){return e.code();}};
            check(outcome()==outcome(),"deterministic malformed-input property");
        }
        {
            Store db(path.string());db.title("one","One");db.title("two","Two");
            (void)db.user("alice","alice-password","Alice");(void)db.user("bob","bob-password","Bob");
            Json achievement{{"key","first"},{"name","First"},{"description","An award"},{"howToEarn","Play"},{"score",10}};
            db.achievement("one",achievement);db.achievement("two",achievement);
            Json board{{"key","BestScoreLifeTime"},{"mode",0},{"ascending",false},{"aggregation","best"},{"arbitrated",false},{"columns",{{"Rounds","int32"},{"Label","string"}}}};
            db.leaderboard("one",board);db.leaderboard("two",board);board["mode"]=1;board["ascending"]=true;db.leaderboard("one",board);board["mode"]=2;board["aggregation"]="latest";db.leaderboard("one",board);board["mode"]=3;board["arbitrated"]=true;db.leaderboard("one",board);
            for(const auto& tag:{"Alice","Bob"}) {
                Json row{{"key","BestScoreLifeTime"},{"mode",0},{"gamertag",tag},{"rating",tag==std::string("Alice")?100LL:200LL},{"columns",{{"Rounds",{{"type","int32"},{"value",3}}},{"Label",{{"type","string"},{"value","Original"}}}}}};
                db.seedLeaderboard("one",row);row["mode"]=1;db.seedLeaderboard("one",row);
            }
            bool refused=false;try{validateColumns({{"Rounds",{{"type","int64"},{"value",3}}}},board["columns"]);}catch(const Error& e){refused=e.code()=="INVALID_ARGUMENT";}check(refused,"board column type");
            refused=false;try{validateColumns({{"Rounds",{{"type","int32"},{"value",2147483648LL}}}},board["columns"]);}catch(const Error& e){refused=e.code()=="INVALID_ARGUMENT";}check(refused,"board column range");
            refused=false;try{validateColumns({{"unknown",{{"type","int32"},{"value",0}}}},board["columns"]);}catch(const Error& e){refused=e.code()=="INVALID_ARGUMENT";}check(refused,"board undefined column");
        }
        std::string alice,bob,other;
        const std::string assetBytes="glTF"+std::string("\x02\0\0\0\x0c\0\0\0",8);
        std::string hash;
        {Store db(path.string());hash=db.asset("one","model/gltf-binary",assetBytes);check(db.asset("one","model/gltf-binary",assetBytes)==hash,"idempotent asset import");}

        {
            Service s(path.string());
            check(call(s,"missing","hello")["error"]=="OK","hello");
            check(call(s,"one","unknown")["error"]=="UNKNOWN_OPERATION","unknown op");
            check(call(s,"missing","auth.login",{{"username","alice"},{"password","alice-password"}})["error"]=="UNKNOWN_TITLE","title");
            check(call(s,"one","auth.login",{{"username","alice"},{"password","wrong-password"}})["error"]=="AUTHENTICATION_FAILED","bad password");
            check(call(s,"one","auth.login",{{"username","absent"},{"password","wrong-password"}})["error"]=="AUTHENTICATION_FAILED","absent user");
            alice=call(s,"one","auth.login",{{"username","alice"},{"password","alice-password"}})["result"]["token"];
            bob=call(s,"one","auth.login",{{"username","bob"},{"password","bob-password"}})["result"]["token"];
            other=call(s,"two","auth.login",{{"username","alice"},{"password","alice-password"}})["result"]["token"];
            Json read{{"key","BestScoreLifeTime"},{"mode",0},{"start",0},{"size",1}};
            auto page=call(s,"one","leaderboards.read",read,alice)["result"];
            check(page["total"]==2&&page["entries"].size()==1&&page["entries"][0]["gamertag"]=="Bob"&&page["entries"][0]["rank"]==1,"descending board");
            read["start"]=1;page=call(s,"one","leaderboards.read",read,alice)["result"];
            check(page["start"]==1&&page["entries"][0]["gamertag"]=="Alice"&&page["entries"][0]["columns"]["Rounds"]["value"]==3,"board second page/columns");
            read["pivot"]="Alice";read["start"]=0;page=call(s,"one","leaderboards.read",read,alice)["result"];check(page["start"]==1,"centered board");
            read["gamers"]=Json::array({"Alice"});page=call(s,"one","leaderboards.read",read,alice)["result"];check(page["total"]==1&&page["start"]==0&&page["entries"][0]["rank"]==2,"restricted board global rank");
            read.erase("pivot");read["gamers"]=Json::array();check(call(s,"one","leaderboards.read",read,alice)["result"]["total"]==0,"empty gamer restriction");
            read.erase("gamers");read["mode"]=1;check(call(s,"one","leaderboards.read",read,alice)["result"]["entries"][0]["gamertag"]=="Alice","ascending mode isolation");
            read["mode"]=0;check(call(s,"two","leaderboards.read",read,other)["result"]["total"]==0,"title board isolation");
            read["size"]=0;check(call(s,"one","leaderboards.read",read,alice)["error"]=="INVALID_ARGUMENT","board zero page");read["size"]=101;check(call(s,"one","leaderboards.read",read,alice)["error"]=="INVALID_ARGUMENT","board page cap");read["size"]=1;read["mode"]=99;check(call(s,"one","leaderboards.read",read,alice)["error"]=="NOT_FOUND","missing board");
            check(call(s,"two","achievements.list",Json::object(),alice)["error"]=="UNAUTHENTICATED","token isolation");
            check(call(s,"one","achievements.award",{{"key","absent"}},alice)["error"]=="NOT_FOUND","invalid award");
            check(call(s,"one","achievements.award",{{"key","first"}},alice,"duplicate")["error"]=="OK","award");
            check(call(s,"one","achievements.award",{{"key","first"}},alice,"duplicate")["error"]=="DUPLICATE_REQUEST","duplicate request");
            auto ticks=call(s,"one","achievements.list",Json::object(),alice)["result"]["achievements"][0]["earnedTicks"];
            check(ticks>0,"timestamp");
            check(call(s,"one","achievements.award",{{"key","first"}},alice)["error"]=="OK","repeat award");
            check(call(s,"one","achievements.list",Json::object(),alice)["result"]["achievements"][0]["earnedTicks"]==ticks,"unchanged timestamp");
            for(auto pair:{std::pair{"one",bob},std::pair{"two",other}})
                check(call(s,pair.first,"achievements.list",Json::object(),pair.second)["result"]["achievements"][0]["earnedTicks"]==0,"user/title earned isolation");
            auto resource=call(s,"one","assets.read",{{"hash",hash},{"offset",0},{"length",12288}},alice);
            check(resource["error"]=="OK"&&resource["result"]["size"]==12&&resource["result"]["hex"]=="676c5446020000000c000000","bounded asset bytes");
            check(call(s,"two","assets.read",{{"hash",hash},{"offset",0},{"length",1}},other)["error"]=="NOT_FOUND","asset title ACL");
            check(call(s,"one","assets.read",{{"hash","../file"},{"offset",0},{"length",1}},alice)["error"]=="INVALID_ARGUMENT","no asset paths");
            check(call(s,"one","assets.read",{{"hash",hash},{"offset",12},{"length",1}},alice)["error"]=="INVALID_ARGUMENT","asset end offset");
            check(call(s,"one","assets.read",{{"hash",hash},{"offset",0},{"length",12289}},alice)["error"]=="LIMIT_EXCEEDED","asset chunk cap");
            check(call(s,"one","assets.read",{{"hash",std::string(64,'0')},{"offset",0},{"length",1}},alice)["error"]=="NOT_FOUND","missing asset");
            check(call(s,"one","friends.add",{{"gamertag","Bob"}},alice)["error"]=="OK","friend add");
            auto pending=call(s,"one","friends.list",Json::object(),alice)["result"]["friends"];
            check(pending.size()==1&&pending[0]["requestSent"]==true&&!pending[0]["accepted"].get<bool>()&&!pending[0]["online"].get<bool>(),"outgoing request privacy");
            auto incoming=call(s,"one","friends.list",Json::object(),bob)["result"]["friends"];
            check(incoming.size()==1&&incoming[0]["requestReceived"]==true,"incoming request");
            check(call(s,"one","friends.accept",{{"gamertag","Alice"}},bob)["error"]=="OK","mutual accept");
            check(call(s,"one","presence.set",{{"mode",1},{"text","Playing"}},bob)["error"]=="OK","presence");
            auto friends=call(s,"one","friends.list",Json::object(),alice)["result"]["friends"];
            check(friends.size()==1&&friends[0]["online"]==true&&friends[0]["presenceText"]=="Playing","friend presence");
            check(friends[0]["accepted"]==true&&!friends[0]["requestSent"].get<bool>(),"accepted flags");
            check(call(s,"two","friends.list",Json::object(),other)["result"]["friends"][0]["presenceText"]=="","title presence isolation");
            check(call(s,"one","friends.remove",{{"gamertag","Alice"}},bob)["error"]=="OK","mutual removal");
            check(call(s,"one","friends.list",Json::object(),alice)["result"]["friends"].empty(),"removed both directions");
            check(call(s,"one","friends.accept",{{"gamertag","Bob"}},alice)["error"]=="INVALID_STATE","accept without request");
            check(call(s,"one","friends.add",{{"gamertag","Alice"}},alice)["error"]=="INVALID_ARGUMENT","self request");
            check(call(s,"one","profile.get",{{"gamertag","Alice"}},bob)["result"]["gamerScore"]==10,"profile aggregate");
            check(call(s,"one","profile.get",{{"gamertag","Alice"}},bob)["result"]["titlesPlayed"]==1,"titles played by an earned achievement");
            check(call(s,"one","profile.get",{{"gamertag","Bob"}},alice)["result"]["titlesPlayed"]==1,"titles played by presence");
            check(call(s,"one","gamer.lookup",{{"gamertag","absent"}},alice)["error"]=="NOT_FOUND","lookup absent");
        }
        {
            Service s(path.string());
            check(call(s,"one","achievements.list",Json::object(),alice)["result"]["achievements"][0]["earnedTicks"]>0,"restart/auth persistence");
            check(call(s,"one","leaderboards.read",{{"key","BestScoreLifeTime"},{"mode",0},{"start",0},{"size",2}},alice)["result"]["total"]==2,"board restart persistence");
            auto scope=call(s,"one","leaderboards.game.begin",{{"kind","local"},{"participants",Json::array({alice,bob})}},alice)["result"]["gameplay"];
            check(scope.is_string(),"authenticated local game scope");
            Json rows=Json::array({{{"userId",call(s,"one","profile.get",{{"gamertag","Alice"}},alice)["result"]["userId"]},{"key","BestScoreLifeTime"},{"mode",0},{"rating",350},{"columns",{{"Rounds",{{"type","int32"},{"value",5}}}}}}});
            check(call(s,"one","leaderboards.game.commit",{{"gameplay",scope},{"entries",rows}},bob)["error"]=="NOT_AUTHORIZED","commit host authority");
            auto bad=rows;bad.push_back(rows[0]);bad[1]["key"]="undefined";
            check(call(s,"one","leaderboards.game.commit",{{"gameplay",scope},{"entries",bad}},alice)["error"]=="NOT_FOUND","invalid commit row");
            auto unchanged=call(s,"one","leaderboards.read",{{"key","BestScoreLifeTime"},{"mode",0},{"start",0},{"size",1},{"gamers",Json::array({"Alice"})}},alice)["result"]["entries"][0]["rating"];
            check(unchanged==100,"invalid commit atomicity");
            check(call(s,"one","leaderboards.game.commit",{{"gameplay",scope},{"entries",rows}},alice)["error"]=="OK","local game commit");
            check(call(s,"one","leaderboards.game.commit",{{"gameplay",scope},{"entries",rows}},alice)["error"]=="OK","idempotent commit retry");
            {Service restarted(path.string());check(call(restarted,"one","leaderboards.game.commit",{{"gameplay",scope},{"entries",rows}},alice)["error"]=="OK","commit retry survives restart");}
            auto submit=[&](Json entries){auto epoch=call(s,"one","leaderboards.game.begin",{{"kind","local"},{"participants",Json::array({alice})}},alice)["result"]["gameplay"];return call(s,"one","leaderboards.game.commit",{{"gameplay",epoch},{"entries",entries}},alice);};
            auto row=rows[0];row["rating"]=50;
            check(submit(Json::array({row}))["error"]=="OK","worse best score accepted without replacement");
            check(call(s,"one","leaderboards.read",{{"key","BestScoreLifeTime"},{"mode",0},{"start",0},{"size",1}},alice)["result"]["entries"][0]["rating"]==350,"best aggregation retained");
            row["mode"]=1;check(submit(Json::array({row}))["error"]=="OK","ascending score accepted");
            check(call(s,"one","leaderboards.read",{{"key","BestScoreLifeTime"},{"mode",1},{"start",0},{"size",1}},alice)["result"]["entries"][0]["rating"]==50,"ascending best aggregation");
            row["mode"]=2;check(submit(Json::array({row}))["error"]=="OK","latest board first write");row["rating"]=90;
            check(submit(Json::array({row}))["error"]=="OK","latest board replacement");
            check(call(s,"one","leaderboards.read",{{"key","BestScoreLifeTime"},{"mode",2},{"start",0},{"size",1}},alice)["result"]["entries"][0]["rating"]==90,"latest aggregation replaces worse score");
            row["mode"]=3;check(submit(Json::array({row}))["error"]=="NOT_AUTHORIZED","local scope cannot write arbitrated board");
            row["mode"]=0;row["userId"]=call(s,"one","profile.get",{{"gamertag","Bob"}},alice)["result"]["userId"];
            check(submit(Json::array({row}))["error"]=="NOT_AUTHORIZED","scope cannot write nonmember row");
            row=rows[0];row["columns"]["Rounds"]["type"]="int64";check(submit(Json::array({row}))["error"]=="INVALID_ARGUMENT","commit column schema enforced");
            check(submit(Json::array({rows[0],rows[0]}))["error"]=="INVALID_ARGUMENT","duplicate commit row rejected");
            rows[0]["rating"]=400;check(call(s,"one","leaderboards.game.commit",{{"gameplay",scope},{"entries",rows}},alice)["error"]=="INVALID_STATE","closed scope changed payload");
            check(call(s,"one","leaderboards.game.begin",{{"kind","ranked"},{"participants",Json::array({alice})}},alice)["error"]=="NOT_SUPPORTED","unadvertised ranked scope");
            check(call(s,"one","leaderboards.game.begin",{{"kind","local"},{"participants",Json::array({other})}},alice)["error"]=="NOT_AUTHORIZED","cross title participants");
            check(call(s,"one","leaderboards.game.begin",{{"kind","local"},{"participants",Json::array({bob})}},alice)["error"]=="NOT_AUTHORIZED","owner missing membership");
            check(call(s,"one","leaderboards.game.abort",{{"gameplay",scope}},alice)["error"]=="INVALID_STATE","cannot abort committed epoch");
            check(call(s,"one","leaderboards.game.abort",{{"gameplay","../file"}},alice)["error"]=="INVALID_ARGUMENT","malformed abort id");
            std::vector<Json> abandoned;
            for(int attempt=0;attempt<17;++attempt) {
                auto response=call(s,"one","leaderboards.game.begin",{{"kind","local"},{"participants",Json::array({alice})}},alice);
                if(response["error"]=="LIMIT_EXCEEDED")break;
                check(response["error"]=="OK","epoch quota setup");abandoned.push_back(response["result"]["gameplay"]);
            }
            check(!abandoned.empty()&&abandoned.size()<17,"active epoch quota enforced");
            check(call(s,"one","leaderboards.game.abort",{{"gameplay",abandoned[0]}},bob)["error"]=="NOT_AUTHORIZED","abort host authority");
            {Service restarted(path.string());check(call(restarted,"one","leaderboards.game.abort",{{"gameplay",abandoned[0]}},alice)["error"]=="OK","abort after restart");}
            for(const auto& epoch:abandoned)check(call(s,"one","leaderboards.game.abort",{{"gameplay",epoch}},alice)["error"]=="OK","abort and repeated abort");
            auto replacement=call(s,"one","leaderboards.game.begin",{{"kind","local"},{"participants",Json::array({alice})}},alice);
            check(replacement["error"]=="OK","abort releases quota");
            check(call(s,"one","leaderboards.game.abort",{{"gameplay",replacement["result"]["gameplay"]}},alice)["error"]=="OK","replacement cleanup");
            check(call(s,"one","auth.logout",Json::object(),alice)["error"]=="OK","logout");
            check(call(s,"one","achievements.list",Json::object(),alice)["error"]=="UNAUTHENTICATED","revoked token");
            for(int i=0;i<10;++i)check(call(s,"one","auth.login",{{"username","absent"},{"password","wrong-password"}})["error"]=="AUTHENTICATION_FAILED","throttle threshold");
            check(call(s,"one","auth.login",{{"username","absent"},{"password","wrong-password"}})["error"]=="RATE_LIMITED","throttle");
        }
        {
            Service s(path.string());
            auto login=call(s,"one","auth.login",{{"username","alice"},{"password","alice-password"}})["result"];
            const auto access=login["token"].get<std::string>(),refresh=login["refreshToken"].get<std::string>();
            check(access.size()==64&&refresh.size()==64&&access!=refresh,"independent access/refresh credentials");
            check(call(s,"one","auth.ping",Json::object(),access)["error"]=="OK","authenticated heartbeat");
            auto rotated=call(s,"one","auth.refresh",{{"refreshToken",refresh}})["result"];
            check(rotated["refreshToken"]!=refresh&&rotated["token"]!=access&&rotated["refreshExpires"]==login["refreshExpires"],"refresh rotates without extending lifetime");
            const auto current=rotated["token"].get<std::string>();
            check(call(s,"one","auth.ping",Json::object(),access)["error"]=="UNAUTHENTICATED","earlier access retired");
            {Service restarted(path.string());check(call(restarted,"one","auth.ping",Json::object(),current)["error"]=="OK","rotated access survives restart");}
            check(call(s,"two","auth.refresh",{{"refreshToken",rotated["refreshToken"]}})["error"]=="UNAUTHENTICATED","refresh title isolation");
            check(call(s,"one","auth.ping",Json::object(),current)["error"]=="OK","wrong-title attempt does not revoke family");
            check(call(s,"one","auth.refresh",{{"refreshToken",refresh}})["error"]=="UNAUTHENTICATED","refresh replay rejected");
            check(call(s,"one","auth.ping",Json::object(),current)["error"]=="UNAUTHENTICATED","replay revokes rotated access");
            check(call(s,"one","auth.refresh",{{"refreshToken",rotated["refreshToken"]}})["error"]=="UNAUTHENTICATED","replay revokes current refresh");
            check(call(s,"one","auth.ping",Json::object(),bob)["error"]=="OK","replay does not revoke other user");
            check(call(s,"two","auth.ping",Json::object(),other)["error"]=="OK","replay does not revoke other title/device");
            auto logout=call(s,"one","auth.login",{{"username","alice"},{"password","alice-password"}})["result"];
            check(call(s,"one","auth.logout",Json::object(),logout["token"])["error"]=="OK","refresh family logout");
            check(call(s,"one","auth.refresh",{{"refreshToken",logout["refreshToken"]}})["error"]=="UNAUTHENTICATED","logout revokes refresh authority");
        }
        {
            Service s(path.string());auto credentials=call(s,"one","auth.login",{{"username","alice"},{"password","alice-password"}})["result"];
            {Store db(path.string());db.exec("UPDATE sessions SET expires=0 WHERE user_id=(SELECT id FROM users WHERE username='alice');");}
            check(call(s,"one","auth.ping",Json::object(),credentials["token"])["error"]=="UNAUTHENTICATED","expired access rejected");
            auto restored=call(s,"one","auth.refresh",{{"refreshToken",credentials["refreshToken"]}});
            check(restored["error"]=="OK","refresh renews expired access");
            {Store db(path.string());db.exec("UPDATE refresh_families SET expires=0 WHERE user_id=(SELECT id FROM users WHERE username='alice');");}
            check(call(s,"one","auth.refresh",{{"refreshToken",restored["result"]["refreshToken"]}})["error"]=="UNAUTHENTICATED","expired refresh rejected");
            check(call(s,"one","auth.refresh",{{"refreshToken","malformed"}})["error"]=="UNAUTHENTICATED","malformed refresh rejected");
            for(int i=0;i<6;++i)check(call(s,"one","auth.refresh",{{"refreshToken",std::string(64,'0')}})["error"]=="UNAUTHENTICATED","refresh throttle threshold");
            check(call(s,"one","auth.refresh",{{"refreshToken",std::string(64,'0')}})["error"]=="RATE_LIMITED","refresh source rate limit");
        }
        {Store db(path.string());db.exec("DROP TABLE directory_removals; DROP TABLE avatar_catalog_assets; DROP TABLE avatar_catalog_items; DROP TABLE avatar_catalogs; DROP TABLE avatars; DROP TABLE player_reviews; DROP TABLE messages; DROP TABLE arbitration_submissions; DROP TABLE arbitration_rounds; DROP TABLE relay_ticket_members; DROP TABLE relay_tickets; DROP TABLE invitation_send_limits; DROP TABLE session_invitations; DROP TABLE directory_members; DROP TABLE directory_machines; DROP TABLE directory_sessions; DROP TABLE refresh_credentials; DROP INDEX sessions_refresh_family; ALTER TABLE sessions DROP COLUMN refresh_family; DROP TABLE refresh_families; DROP TABLE leaderboard_game_members; DROP TABLE leaderboard_games; DROP TABLE leaderboard_entries; DROP TABLE leaderboards; DROP TABLE title_assets; DROP TABLE assets; ALTER TABLE users DROP COLUMN picture; PRAGMA user_version=1;");}
        {Store upgraded(path.string());Statement version(upgraded.db(),"PRAGMA user_version");(void)version.row();check(version.number(0)==SchemaVersion,"v1 database migration");Statement users(upgraded.db(),"SELECT COUNT(*) FROM users");(void)users.row();check(users.number(0)==2,"migration preserves identities");}
        {Store db(path.string());db.exec(("PRAGMA user_version="+std::to_string(SchemaVersion+1)).c_str());}
        bool refused=false;try{Store future(path.string());}catch(const Error& e){refused=e.code()=="UNSUPPORTED_DATABASE_VERSION";}
        check(refused,"future schema");clean();std::cout<<assertions<<" assertions passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
