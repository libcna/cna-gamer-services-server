// SPDX-License-Identifier: MIT
#include "CnaService/Service.hpp"
#include <array>
#include <filesystem>
#include <iostream>
#include <optional>
using namespace CnaService;
namespace {
int checks=0;
void check(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}
Json call(Service& service,const std::string& op,const Json& args,const std::string& token) {
    static int sequence=0;
    return parse(service.handle(Json{{"v",1},{"id","arbitration-"+std::to_string(++sequence)},{"op",op},{"game","one"},{"token",token},{"args",args}}.dump(),"arbitration-test"));
}
Json row(const std::string& user,const std::string& key,long long rating) {
    return Json{{"userId",user},{"key",key},{"mode",0},{"rating",rating},{"columns",Json::object()}};
}
}
int main() {
    const auto path=std::filesystem::current_path()/"arbitration-unit.sqlite3";
    auto clean=[&]{for(const auto* suffix:{"","-wal","-shm"})std::filesystem::remove(path.string()+suffix);};
    try {
        clean();
        {Store store(path.string());store.title("one","One");
         for(const auto& [name,tag]:std::array<std::pair<const char*,const char*>,3>{{{"alice","Alice"},{"bob","Bob"},{"charlie","Charlie"}}})
             store.user(name,std::string(name)+"-password",tag);
         store.leaderboard("one",Json{{"key","Kills"},{"mode",0},{"ascending",false},{"aggregation","latest"},{"arbitrated",true},{"columns",Json::object()}});
         store.leaderboard("one",Json{{"key","Score"},{"mode",0},{"ascending",false},{"aggregation","latest"},{"arbitrated",false},{"columns",Json::object()}});}
        Service service(path.string());std::array<std::string,3> tokens,users;
        int index=0;for(const auto* name:{"alice","bob","charlie"}) {
            auto login=call(service,"auth.login",{{"username",name},{"password",std::string(name)+"-password"}},{});
            check(login["error"]=="OK","fixture authentication");
            tokens[index]=login["result"]["token"].get<std::string>();users[index]=login["result"]["identity"]["userId"].get<std::string>();++index;
        }
        check(call(service,"hello",Json::object(),{})["result"]["capabilities"].dump().find("ranked-arbitration")!=std::string::npos,"capability advertised");
        const auto wildcard=Json::array({nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr});
        auto created=call(service,"sessions.create",{{"kind","ranked"},{"maxGamers",4},{"privateSlots",0},{"properties",wildcard},{"allowJoinInProgress",false},{"participants",Json::array({tokens[0]})}},tokens[0]);
        check(created["error"]=="OK","ranked create");const auto session=created["result"]["session"].get<std::string>();
        check(call(service,"sessions.join",{{"session",session},{"participants",Json::array({tokens[1]})}},tokens[1])["error"]=="OK","second machine");
        check(call(service,"sessions.join",{{"session",session},{"participants",Json::array({tokens[2]})}},tokens[2])["error"]=="OK","third machine");
        auto snapshot=call(service,"sessions.get",{{"session",session}},tokens[0])["result"];
        auto settings=[&](const std::string& state){return Json{{"session",session},{"revision",snapshot["revision"]},{"maxGamers",4},{"privateSlots",0},{"properties",wildcard},{"allowJoinInProgress",false},{"state",state}};};
        snapshot=call(service,"sessions.update",settings("playing"),tokens[0])["result"];
        const auto playing=snapshot["revision"].get<long long>();
        // Each machine commits its own epoch; arbitrated rows are this machine's report for the round.
        auto commit=[&](int machine,const Json& entries,std::optional<Json> context) {
            auto begun=call(service,"leaderboards.game.begin",{{"kind","local"},{"participants",Json::array({tokens[machine]})}},tokens[machine]);
            check(begun["error"]=="OK","epoch begin");
            Json args{{"gameplay",begun["result"]["gameplay"]},{"entries",entries}};if(context)args["arbitration"]=*context;
            return call(service,"leaderboards.game.commit",args,tokens[machine])["error"].get<std::string>();
        };
        auto read=[&](const std::string& key,const std::string& gamertag) {
            auto page=call(service,"leaderboards.read",{{"key",key},{"mode",0},{"start",0},{"size",10},{"gamers",Json::array({gamertag})}},tokens[0])["result"]["entries"];
            return page.empty()?-1LL:page[0]["rating"].get<long long>();
        };
        const Json context{{"session",session},{"revision",playing}};
        check(commit(0,Json::array({row(users[1],"Kills",3)}),std::nullopt)=="NOT_AUTHORIZED","arbitrated row requires the Ranked round");
        check(commit(0,Json::array({row(users[1],"Score",3)}),context)=="NOT_AUTHORIZED","nonarbitrated rows only for the machine's own gamers");
        check(commit(0,Json::array({row(users[0],"Kills",3)}),Json{{"session",std::string(32,'0')},{"revision",playing}})=="NOT_FOUND","unknown round");
        // Round one: all three machines report; alice's row is unanimous, bob's is a 2:1 majority,
        // charlie's has no majority and is discarded.
        check(commit(0,Json::array({row(users[0],"Kills",10),row(users[1],"Kills",5),row(users[2],"Kills",1),row(users[0],"Score",70)}),context)=="OK","host report");
        check(read("Kills","Alice")==-1&&read("Score","Alice")==70,"arbitrated rows wait for the round; own nonarbitrated rows commit");
        check(commit(1,Json::array({row(users[0],"Kills",10),row(users[1],"Kills",5),row(users[2],"Kills",2)}),context)=="OK","second report");
        check(read("Kills","Alice")==-1,"two of three machines do not complete the round");
        check(commit(2,Json::array({row(users[0],"Kills",10),row(users[1],"Kills",99),row(users[2],"Kills",3)}),context)=="OK","third report");
        check(read("Kills","Alice")==10&&read("Kills","Bob")==5&&read("Kills","Charlie")==-1,"strict majority arbitration");
        check(commit(2,Json::array({row(users[2],"Kills",42)}),context)=="INVALID_STATE","a machine cannot change its report");
        check(commit(2,Json::array({row(users[0],"Kills",10),row(users[1],"Kills",99),row(users[2],"Kills",3)}),context)=="OK","identical report replay is idempotent");
        // Round two: after EndGame, a missing report is resolved from those present once stale.
        snapshot=call(service,"sessions.update",settings("lobby"),tokens[0])["result"];
        snapshot=call(service,"sessions.update",settings("playing"),tokens[0])["result"];
        const Json second{{"session",session},{"revision",snapshot["revision"]}};
        check(commit(0,Json::array({row(users[0],"Kills",20),row(users[1],"Kills",6)}),second)=="OK","second round host");
        check(commit(1,Json::array({row(users[0],"Kills",20),row(users[1],"Kills",6)}),second)=="OK","second round client");
        snapshot=call(service,"sessions.update",settings("lobby"),tokens[0])["result"];
        check(read("Kills","Alice")==10,"third machine still missing");
        {Store store(path.string());store.exec("UPDATE arbitration_rounds SET updated=updated-61 WHERE end_revision IS NOT NULL AND resolved=0");}
        check(commit(0,Json::array({row(users[0],"Score",80)}),std::nullopt)=="OK","a later commit sweeps stale rounds");
        check(read("Kills","Alice")==20&&read("Kills","Bob")==6,"stale round resolves among reporting machines");
        check(read("Kills","Charlie")==-1&&read("Score","Alice")==80,"no report means no arbitrated row");
        // Round three: a machine leaving early reports only its own gamer; the finishers' rows about
        // themselves stand, while the leaver's self-report needs a finisher's agreement.
        snapshot=call(service,"sessions.update",settings("playing"),tokens[0])["result"];
        const Json third{{"session",session},{"revision",snapshot["revision"]}};
        check(commit(2,Json::array({row(users[2],"Kills",500)}),third)=="OK","early leaver self-report");
        check(commit(0,Json::array({row(users[0],"Kills",30),row(users[1],"Kills",7),row(users[2],"Kills",0)}),third)=="OK","finisher one");
        check(commit(1,Json::array({row(users[0],"Kills",30),row(users[1],"Kills",7),row(users[2],"Kills",0)}),third)=="OK","finisher two");
        check(read("Kills","Alice")==30&&read("Kills","Bob")==7,"finishers agree about themselves");
        check(read("Kills","Charlie")==0,"two finishers outvote the leaver's self-report");
        clean();
        std::cout<<"arbitration "<<checks<<" assertions passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<"arbitration test failed: "<<error.what()<<"\n";clean();return 1;}
}
