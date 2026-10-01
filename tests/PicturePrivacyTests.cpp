// SPDX-License-Identifier: MIT
#include "CnaService/Service.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace CnaService;
namespace {
int checks=0;
void check(bool ok,const char* reason){++checks;if(!ok)throw std::runtime_error(reason);}
Json call(Service& s,const std::string& op,Json args,const std::string& token) {
    static int id=0;
    Json r{{"v",1},{"id","picture-"+std::to_string(++id)},{"game","one"},{"op",op},{"args",std::move(args)}};
    if(!token.empty())r["token"]=token;
    return parse(s.handle(r.dump(),"picture-test"));
}
}
int main() {
    try {
        const auto path=(std::filesystem::current_path()/"picture-privacy.sqlite3").string();
        for(const auto* suffix:{"","-wal","-shm"})std::filesystem::remove(path+suffix);
        std::string hash;
        {
            Store db(path);db.title("one","One");db.title("pictures","Profile assets");
            (void)db.user("alice","alice-password","Alice");
            (void)db.user("bob","bob-password","Bob");
            (void)db.user("carol","carol-password","Carol");
            std::string png(24,'\0');png.replace(0,8,"\x89PNG\r\n\x1a\n",8);png[11]=13;png.replace(12,4,"IHDR");png[19]=1;png[23]=1;
            hash=db.asset("pictures","image/png",png);db.picture("alice",hash);
        }
        {
            Service s(path);
            auto login=[&](const std::string& name){auto r=call(s,"auth.login",{{"username",name},{"password",name+"-password"}},"");check(r["error"]=="OK","login");return r["result"]["token"].get<std::string>();};
            const auto alice=login("alice"),bob=login("bob"),carol=login("carol");
            auto picture=[&](const std::string& token,const std::string& expected) {
                const auto binary=s.file("one",token,hash);
                check(binary.code==expected,"binary picture follows profile policy");
                if(expected!="OK")check(binary.bytes.empty()&&binary.mime.empty(),"denied binary has no content");
                const auto json=call(s,"assets.read",{{"hash",hash},{"offset",0},{"length",24}},token);
                check(json["error"]==expected,"JSON picture follows profile policy");
            };
            check(s.file("one","",hash).code!="OK","anonymous binary refused");
            check(call(s,"assets.read",{{"hash",hash},{"offset",0},{"length",24}},"")["error"]!="OK","anonymous JSON refused");
            picture(alice,"OK");picture(bob,"OK");
            {Store db(path);db.privilege("bob","profileViewing","friends");}
            check(call(s,"profile.get",{{"gamertag","Alice"}},bob)["error"]=="NOT_AUTHORIZED","profile refused to stranger");
            picture(bob,"NOT_AUTHORIZED");
            check(call(s,"friends.add",{{"gamertag","Alice"}},bob)["error"]=="OK","friend request");
            picture(bob,"NOT_AUTHORIZED");
            check(call(s,"friends.accept",{{"gamertag","Bob"}},alice)["error"]=="OK","friend acceptance");
            picture(bob,"OK");
            check(call(s,"privacy.block",{{"gamertag","Bob"}},alice)["error"]=="OK","owner blocks viewer");
            picture(bob,"NOT_AUTHORIZED");
            picture(alice,"OK");
            check(call(s,"privacy.unblock",{{"gamertag","Bob"}},alice)["error"]=="OK","unblock");
            {Store db(path);db.privilege("bob","profileViewing","everyone");}
            picture(bob,"OK");
            check(call(s,"privacy.block",{{"gamertag","Alice"}},bob)["error"]=="OK","viewer blocks owner");
            picture(bob,"NOT_AUTHORIZED");
            {Store db(path);db.privilege("alice","profileViewing","blocked");db.privilege("carol","profileViewing","blocked");}
            picture(alice,"OK");picture(carol,"NOT_AUTHORIZED");
            // Content hashes can have several legitimate owners. A viewable owner grants access.
            {Store db(path);db.picture("carol",hash);}
            picture(carol,"OK");
            // An explicit title asset remains public to that title, even if also a private picture.
            {Store db(path);Statement grant(db.db(),"INSERT INTO title_assets(game_id,hash) VALUES('one',?)");grant.bind(1,hash);(void)grant.row();}
            picture(bob,"OK");
        }
        for(const auto* suffix:{"","-wal","-shm"})std::filesystem::remove(path+suffix);
        std::cout<<checks<<" picture privacy checks passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
