// SPDX-License-Identifier: MIT
#include "CnaService/Service.hpp"
#include <array>
#include <filesystem>
#include <iostream>
using namespace CnaService;
namespace {
int checks=0;
void check(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}
Json call(Service& service,const std::string& op,const Json& args,const std::string& token,const std::string& title="one") {
    static int sequence=0;
    return parse(service.handle(Json{{"v",1},{"id","social-"+std::to_string(++sequence)},{"op",op},{"game",title},{"token",token},{"args",args}}.dump(),"social-test"));
}
}
int main() {
    const auto path=std::filesystem::current_path()/"social-unit.sqlite3";
    auto clean=[&]{for(const auto* suffix:{"","-wal","-shm"})std::filesystem::remove(path.string()+suffix);};
    try {
        clean();
        {Store store(path.string());store.title("one","One");store.title("two","Two");
         for(const auto& [name,tag]:std::array<std::pair<const char*,const char*>,3>{{{"alice","Alice"},{"bob","Bob"},{"charlie","Charlie"}}})
             store.user(name,std::string(name)+"-password",tag);}
        Service service(path.string());std::array<std::string,3> tokens;
        int index=0;for(const auto* name:{"alice","bob","charlie"}) {
            auto login=call(service,"auth.login",{{"username",name},{"password",std::string(name)+"-password"}},{});
            check(login["error"]=="OK","fixture authentication");tokens[index++]=login["result"]["token"].get<std::string>();
        }
        const auto capabilities=call(service,"hello",Json::object(),{})["result"]["capabilities"].dump();
        check(capabilities.find("\"messages\"")!=std::string::npos&&capabilities.find("player-reviews")!=std::string::npos,"capabilities");
        // Messages: account-global, bounded, recipient-owned.
        check(call(service,"messages.send",{{"gamertags",Json::array({"Bob","Charlie"})},{"text","good game"}},tokens[0])["result"]["sent"]==2,"two recipients");
        check(call(service,"messages.send",{{"gamertags",Json::array({"Alice"})},{"text","self"}},tokens[0])["error"]=="INVALID_ARGUMENT","no self message");
        check(call(service,"messages.send",{{"gamertags",Json::array({"Nobody"})},{"text","x"}},tokens[0])["error"]=="NOT_FOUND","unknown recipient");
        check(call(service,"messages.send",{{"gamertags",Json::array({"Bob","Bob"})},{"text","x"}},tokens[0])["error"]=="INVALID_ARGUMENT","duplicate recipient");
        check(call(service,"messages.send",{{"gamertags",Json::array({"Bob"})},{"text",std::string(257,'a')}},tokens[0])["error"]!="OK","text bound");
        const auto other=call(service,"auth.login",{{"username","charlie"},{"password","charlie-password"}},{},"two")["result"]["token"].get<std::string>();
        check(call(service,"messages.send",{{"gamertags",Json::array({"Bob"})},{"text","rematch?"}},other,"two")["error"]=="OK","another title and sender");
        auto inbox=call(service,"messages.list",{{"start",0},{"limit",10}},tokens[1])["result"];
        check(inbox["total"]==2&&inbox["unread"]==2&&inbox["messages"].size()==2,"inbox across titles");
        check(inbox["messages"][0]["read"]==false,"unread flag");
        const auto first=inbox["messages"][0]["message"].get<std::string>();
        check(call(service,"messages.read",{{"message",first}},tokens[2])["error"]=="NOT_FOUND","another account cannot read");
        check(call(service,"messages.read",{{"message",first}},tokens[1])["error"]=="OK","mark read");
        check(call(service,"messages.list",{{"start",0},{"limit",10}},tokens[1])["result"]["unread"]==1,"unread count");
        check(call(service,"messages.delete",{{"message",first}},tokens[1])["error"]=="OK","delete");
        check(call(service,"messages.list",{{"start",0},{"limit",10}},tokens[1])["result"]["total"]==1,"deleted");
        check(call(service,"messages.list",{{"start",0},{"limit",10}},tokens[2])["result"]["messages"][0]["sender"]=="Alice","sender gamertag");
        // Player reviews: avoided hosts are not offered to the reviewer.
        const auto wildcard=Json::array({nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr});
        check(call(service,"sessions.create",{{"kind","player"},{"maxGamers",4},{"privateSlots",0},{"properties",wildcard},{"allowJoinInProgress",false},{"participants",Json::array({tokens[0]})}},tokens[0])["error"]=="OK","host session");
        Json find{{"kind","player"},{"localCount",1},{"properties",wildcard},{"start",0},{"limit",32}};
        check(call(service,"sessions.find",find,tokens[1])["result"]["sessions"].size()==1,"visible before review");
        check(call(service,"reviews.submit",{{"gamertag","Alice"},{"rating","avoid"}},tokens[1])["error"]=="OK","avoid");
        check(call(service,"sessions.find",find,tokens[1])["result"]["sessions"].empty(),"avoided host hidden from reviewer");
        check(call(service,"sessions.find",find,tokens[2])["result"]["sessions"].size()==1,"other searchers unaffected");
        check(call(service,"reviews.submit",{{"gamertag","Alice"},{"rating","prefer"}},tokens[1])["error"]=="OK","prefer replaces avoid");
        check(call(service,"sessions.find",find,tokens[1])["result"]["sessions"].size()==1,"preferred host visible");
        check(call(service,"reviews.submit",{{"gamertag","Alice"},{"rating","avoid"}},tokens[1])["error"]=="OK","avoid again");
        check(call(service,"reviews.submit",{{"gamertag","Alice"},{"rating","clear"}},tokens[1])["error"]=="OK","clear");
        check(call(service,"sessions.find",find,tokens[1])["result"]["sessions"].size()==1,"cleared review");
        check(call(service,"reviews.submit",{{"gamertag","Bob"},{"rating","avoid"}},tokens[1])["error"]=="INVALID_ARGUMENT","no self review");
        check(call(service,"reviews.submit",{{"gamertag","Nobody"},{"rating","avoid"}},tokens[1])["error"]=="NOT_FOUND","unknown subject");
        check(call(service,"reviews.submit",{{"gamertag","Alice"},{"rating","meh"}},tokens[1])["error"]=="INVALID_ARGUMENT","rating vocabulary");
        // XNA GamerProfile.Reputation: stars from other players' reviews, none without reviews.
        auto profile=[&](const char* tag){return call(service,"profile.get",{{"gamertag",tag}},tokens[2])["result"];};
        check(!profile("Alice").contains("reputation"),"no reviews, no reputation");
        check(call(service,"reviews.submit",{{"gamertag","Alice"},{"rating","prefer"}},tokens[1])["error"]=="OK","one prefer");
        check(profile("Alice")["reputation"]==5.0,"all prefer: five stars");
        check(call(service,"reviews.submit",{{"gamertag","Alice"},{"rating","avoid"}},tokens[2])["error"]=="OK","one avoid");
        check(profile("Alice")["reputation"]==2.5,"half prefer: two and a half stars");
        // GamerZone: the member's own choice.
        check(profile("Alice")["gamerZone"]=="unknown","no zone chosen");
        check(call(service,"profile.setGamerZone",{{"gamerZone","family"}},tokens[0])["result"]["gamerZone"]=="family","zone chosen");
        check(profile("Alice")["gamerZone"]=="family","others see the zone");
        check(call(service,"profile.setGamerZone",{{"gamerZone","hardcore"}},tokens[0])["error"]=="INVALID_ARGUMENT","zone vocabulary");
        clean();std::cout<<"social "<<checks<<" assertions passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<"social test failed: "<<error.what()<<"\n";clean();return 1;}
}
