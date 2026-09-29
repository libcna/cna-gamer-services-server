// SPDX-License-Identifier: MIT
#include "CnaService/Service.hpp"
#include <algorithm>
#include <array>
#include <tuple>
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
        // Push hints: after a request succeeds, each other account it changed, with the topic.
        std::vector<std::pair<std::string,std::string>> hints;
        service.setHintSink([&](const std::string& user,const std::string& topic){hints.emplace_back(user,topic);});
        auto idOf=[&](const char* tag){return call(service,"gamer.lookup",{{"gamertag",tag}},tokens[0])["result"]["userId"].get<std::string>();};
        const auto aliceId=idOf("Alice"),bobId=idOf("Bob"),charlieId=idOf("Charlie");
        auto hinted=[&](std::vector<std::pair<std::string,std::string>> expected){
            auto seen=hints;hints.clear();std::sort(seen.begin(),seen.end());std::sort(expected.begin(),expected.end());return seen==expected;
        };
        const auto capabilities=call(service,"hello",Json::object(),{})["result"]["capabilities"].dump();
        check(capabilities.find("\"events\"")!=std::string::npos,"event channel capability");
        check(capabilities.find("\"messages\"")!=std::string::npos&&capabilities.find("player-reviews")!=std::string::npos,"capabilities");
        // Messages: account-global, bounded, recipient-owned.
        check(call(service,"messages.send",{{"gamertags",Json::array({"Bob","Charlie"})},{"text","good game"}},tokens[0])["result"]["sent"]==2,"two recipients");
        check(hinted({{bobId,"messages"},{charlieId,"messages"}}),"each recipient hears of a message");
        check(call(service,"messages.send",{{"gamertags",Json::array({"Alice"})},{"text","self"}},tokens[0])["error"]=="INVALID_ARGUMENT","no self message");
        check(hinted({}),"a refused request tells nobody");
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
        // Parties: friends only, one party per account, the leader passes on when leaving.
        auto party=[&](const std::string& token){return call(service,"parties.get",Json::object(),token)["result"];};
        check(party(tokens[0])["party"].is_null()&&party(tokens[0])["invitations"].empty(),"no party yet");
        check(call(service,"parties.invite",{{"gamertag","Bob"}},tokens[0])["error"]=="NOT_AUTHORIZED","friends only");
        hints.clear();
        for(const auto& [a,b,tag,back]:std::array<std::tuple<int,int,const char*,const char*>,2>{{{0,1,"Bob","Alice"},{0,2,"Charlie","Alice"}}}) {
            check(call(service,"friends.add",{{"gamertag",tag}},tokens[a])["error"]=="OK","friend request");
            check(hinted({{b==1?bobId:charlieId,"friends"}}),"the other side hears of a friend request");
            check(call(service,"friends.accept",{{"gamertag",back}},tokens[b])["error"]=="OK","friend accepted");
            check(hinted({{aliceId,"friends"}}),"the requester hears of the answer");
        }
        auto invited=call(service,"parties.invite",{{"gamertag","bob"}},tokens[0]);
        check(hinted({{bobId,"party"}}),"a party invitation");
        check(invited["error"]=="OK"&&invited["result"]["party"]["members"].size()==1&&invited["result"]["party"]["leaderId"]==invited["result"]["party"]["members"][0]["userId"],"inviting starts a party");
        const auto id=invited["result"]["party"]["id"].get<std::string>();
        auto bobView=party(tokens[1]);
        check(bobView["party"].is_null()&&bobView["invitations"].size()==1&&bobView["invitations"][0]["senderGamertag"]=="Alice"&&bobView["invitations"][0]["members"]==1,"invitation in the inbox");
        check(call(service,"parties.accept",{{"party",std::string(32,'0')}},tokens[1])["error"]=="NOT_FOUND","only an invited party");
        hints.clear();
        auto joined=call(service,"parties.accept",{{"party",id}},tokens[1])["result"];
        check(hinted({{aliceId,"party"}}),"the party hears who joined");
        check(joined["party"]["members"].size()==2&&joined["invitations"].empty(),"joined");
        Json alice;for(const auto& member:joined["party"]["members"])if(member["gamertag"]=="Alice")alice=member;
        check(alice.is_object()&&alice["online"]==true,"members with their state");
        // Alice's player-match lobby is joinable; a party member sees it.
        check(alice["joinable"]==true,"a member's joinable game");
        check(call(service,"parties.invite",{{"gamertag","Bob"}},tokens[0])["error"]=="CONFLICT","already a member");
        check(call(service,"parties.invite",{{"gamertag","Charlie"}},tokens[0])["error"]=="OK","second invitation");
        check(call(service,"parties.decline",{{"party",id}},tokens[2])["result"]["invitations"].empty(),"declined");
        hints.clear();
        check(call(service,"parties.leave",Json::object(),tokens[0])["result"]["party"].is_null(),"leader leaves");
        check(hinted({{bobId,"party"}}),"the rest of the party hears who left");
        auto rest=party(tokens[1]);
        check(rest["party"]["members"].size()==1&&rest["party"]["leaderId"]==rest["party"]["members"][0]["userId"],"leadership passes on");
        check(call(service,"parties.leave",Json::object(),tokens[1])["result"]["party"].is_null()&&party(tokens[1])["party"].is_null(),"the last member ends the party");
        // Joining a friend's game from the Guide: a join request the friend's session grants, never listed.
        auto request=call(service,"invites.joinFriend",{{"gamertag","Alice"}},tokens[2]);
        check(request["error"]=="OK"&&request["result"]["senderGamertag"]=="Alice"&&request["result"]["status"]=="pending","join request granted");
        check(call(service,"invites.list",{{"start",0},{"limit",8}},tokens[2])["result"]["invites"].empty(),"join requests stay out of the inbox");
        check(call(service,"invites.joinFriend",{{"gamertag","Alice"}},tokens[2])["result"]["invite"]==request["result"]["invite"],"one request per game");
        check(call(service,"invites.accept",{{"invite",request["result"]["invite"]}},tokens[2])["error"]=="OK","accepted like an invitation");
        check(call(service,"invites.joinFriend",{{"gamertag","Bob"}},tokens[2])["error"]=="NOT_AUTHORIZED","friends or party members only");
        check(call(service,"invites.joinFriend",{{"gamertag","Charlie"}},tokens[0])["error"]=="NOT_FOUND","nothing to join");
        clean();std::cout<<"social "<<checks<<" assertions passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<"social test failed: "<<error.what()<<"\n";clean();return 1;}
}
