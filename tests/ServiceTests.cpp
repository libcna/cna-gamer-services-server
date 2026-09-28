// SPDX-License-Identifier: MS-PL
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
            check(call(s,"one","gamer.lookup",{{"gamertag","absent"}},alice)["error"]=="NOT_FOUND","lookup absent");
        }
        {
            Service s(path.string());
            check(call(s,"one","achievements.list",Json::object(),alice)["result"]["achievements"][0]["earnedTicks"]>0,"restart/auth persistence");
            check(call(s,"one","auth.logout",Json::object(),alice)["error"]=="OK","logout");
            check(call(s,"one","achievements.list",Json::object(),alice)["error"]=="UNAUTHENTICATED","revoked token");
            for(int i=0;i<10;++i)check(call(s,"one","auth.login",{{"username","absent"},{"password","wrong-password"}})["error"]=="AUTHENTICATION_FAILED","throttle threshold");
            check(call(s,"one","auth.login",{{"username","absent"},{"password","wrong-password"}})["error"]=="RATE_LIMITED","throttle");
        }
        {Store db(path.string());db.exec("DROP TABLE title_assets; DROP TABLE assets; ALTER TABLE users DROP COLUMN picture; PRAGMA user_version=1;");}
        {Store upgraded(path.string());Statement version(upgraded.db(),"PRAGMA user_version");(void)version.row();check(version.number(0)==2,"v1 database migration");Statement users(upgraded.db(),"SELECT COUNT(*) FROM users");(void)users.row();check(users.number(0)==2,"migration preserves identities");}
        {Store db(path.string());db.exec("PRAGMA user_version=3");}
        bool refused=false;try{Store future(path.string());}catch(const Error& e){refused=e.code()=="UNSUPPORTED_DATABASE_VERSION";}
        check(refused,"future schema");clean();std::cout<<assertions<<" assertions passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
