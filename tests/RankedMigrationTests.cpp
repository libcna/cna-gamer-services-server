// SPDX-License-Identifier: MIT
// GSH-08: a Ranked game whose host crashes mid-game. The service migrates the host, the new host
// ends the game, machines report and leave in the order they happen to, the service restarts and
// requests are retried. Exactly one result must come out, nothing reported may be lost, and the
// machine that lost the host role must not act as host again.
#include "CnaService/Service.hpp"
#include <array>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
using namespace CnaService;
namespace {
int checks=0;
void check(bool value,const std::string& reason){++checks;if(!value)throw std::runtime_error(reason);}
int sequence=0;
Json call(Service& service,const std::string& op,const Json& args,const std::string& token,const std::string& id={}) {
    return parse(service.handle(Json{{"v",1},{"id",id.empty()?"ranked-migration-"+std::to_string(++sequence):id},{"op",op},{"game","one"},
        {"token",token},{"args",args}}.dump(),"ranked-migration-test"));
}
Json row(const std::string& user,long long rating) {
    return Json{{"userId",user},{"key","Kills"},{"mode",0},{"rating",rating},{"columns",Json::object()}};
}
}
int main() {
    const auto path=std::filesystem::current_path()/"ranked-migration-unit.sqlite3";
    auto clean=[&]{for(const auto* suffix:{"","-wal","-shm"})std::filesystem::remove(path.string()+suffix);};
    try {
        clean();
        {Store store(path.string());store.title("one","One");
         for(const auto& [name,tag]:std::array<std::pair<const char*,const char*>,3>{{{"alice","Alice"},{"bob","Bob"},{"charlie","Charlie"}}})
             store.user(name,std::string(name)+"-password",tag);
         store.leaderboard("one",Json{{"key","Kills"},{"mode",0},{"ascending",false},{"aggregation","latest"},{"arbitrated",true},{"columns",Json::object()}});}
        auto service=std::make_unique<Service>(path.string());
        std::array<std::string,3> tokens,users;
        int index=0;for(const auto* name:{"alice","bob","charlie"}) {
            auto login=call(*service,"auth.login",{{"username",name},{"password",std::string(name)+"-password"}},{});
            tokens[index]=login["result"]["token"].get<std::string>();users[index]=login["result"]["identity"]["userId"].get<std::string>();++index;
        }
        const auto wildcard=Json::array({nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr});
        const auto session=call(*service,"sessions.create",{{"kind","ranked"},{"maxGamers",4},{"privateSlots",0},{"properties",wildcard},
            {"allowJoinInProgress",false},{"participants",Json::array({tokens[0]})}},tokens[0])["result"]["session"].get<std::string>();
        for(int machine:{1,2})check(call(*service,"sessions.join",{{"session",session},{"participants",Json::array({tokens[machine]})}},tokens[machine])["error"]=="OK","join");
        auto snapshot=call(*service,"sessions.get",{{"session",session}},tokens[0])["result"];
        std::map<std::string,long long> ordinals;for(const auto& member:snapshot["members"])ordinals[member["userId"]]=member["ordinal"];
        auto settings=[&](const std::string& state,long long revision){return Json{{"session",session},{"revision",revision},{"maxGamers",4},{"privateSlots",0},
            {"properties",wildcard},{"allowJoinInProgress",false},{"allowHostMigration",true},{"state",state}};};
        snapshot=call(*service,"sessions.update",settings("playing",snapshot["revision"]),tokens[0])["result"];
        const auto started=snapshot["revision"].get<long long>();
        // Each machine opened its leaderboard epoch when the game started.
        std::array<std::string,3> gameplay;
        for(int machine=0;machine<3;++machine)
            gameplay[machine]=call(*service,"leaderboards.game.begin",{{"kind","local"},{"participants",Json::array({tokens[machine]})}},tokens[machine])["result"]["gameplay"];

        // Alice's machine (the host) crashes: its lease lapses, and the next request hands the host
        // role to the lowest remaining gamer, Bob.
        {Store store(path.string());store.exec(("UPDATE directory_machines SET expires=0 WHERE owner_id='"+users[0]+"'").c_str());}
        snapshot=call(*service,"sessions.get",{{"session",session}},tokens[1])["result"];
        check(snapshot["hostId"]==users[1],"Bob is the new host");
        check(snapshot["members"].size()==2,"Alice's gamer left with her machine");
        for(const auto& member:snapshot["members"])check(ordinals[member["userId"]]==member["ordinal"],"gamer ids stay what they were");
        check(snapshot["revision"].get<long long>()>started,"the handover is a new revision");
        // Alice restarts with her credential: she is no longer in the session and cannot act as host.
        check(call(*service,"sessions.update",settings("lobby",snapshot["revision"]),tokens[0])["error"]!="OK","the old host cannot end the game");
        check(call(*service,"sessions.touch",{{"session",session}},tokens[0])["error"]!="OK","nor keep its machine alive");

        // Bob, now host, ends the game; Charlie reports his own gamer and leaves.
        snapshot=call(*service,"sessions.update",settings("lobby",snapshot["revision"]),tokens[1])["result"];
        const auto ended=snapshot["revision"].get<long long>();
        check(call(*service,"leaderboards.game.commit",{{"gameplay",gameplay[2]},{"entries",Json::array({row(users[2],4)})},
            {"arbitration",{{"session",session},{"revision",ended}}}},tokens[2])["error"]=="OK","the leaving machine reports");
        check(call(*service,"sessions.leave",{{"session",session}},tokens[2])["error"]=="OK","Charlie leaves");
        snapshot=call(*service,"sessions.get",{{"session",session}},tokens[1])["result"];
        check(snapshot["revision"].get<long long>()>ended,"the departure is a new revision");

        // The service restarts. Bob reports with the revision he now knows -- after the end of the
        // game -- and the report must still count for the game that just ended.
        service=std::make_unique<Service>(path.string());
        const Json report{{"gameplay",gameplay[1]},{"entries",Json::array({row(users[0],9),row(users[1],7),row(users[2],4)})},
            {"arbitration",{{"session",session},{"revision",snapshot["revision"]}}}};
        const auto first=call(*service,"leaderboards.game.commit",report,tokens[1],"bob-report");
        check(first["error"]=="OK","a report made after the game ended and the session moved on is not lost: "+first["error"].get<std::string>());
        check(call(*service,"leaderboards.game.commit",report,tokens[1],"bob-report")==first,"a retried request is answered from the record");
        check(call(*service,"leaderboards.game.commit",report,tokens[1])["error"]=="OK","a repeated identical report changes nothing");

        // Alice never reports. Once the round is stale it resolves among the two reporters.
        {Store store(path.string());store.exec("UPDATE arbitration_rounds SET updated=updated-61 WHERE resolved=0");}
        auto sweep=call(*service,"leaderboards.game.begin",{{"kind","local"},{"participants",Json::array({tokens[1]})}},tokens[1])["result"]["gameplay"];
        check(call(*service,"leaderboards.game.commit",{{"gameplay",sweep},{"entries",Json::array()}},tokens[1])["error"]=="OK","a later commit sweeps");
        auto read=[&](const std::string& tag) {
            auto page=call(*service,"leaderboards.read",{{"key","Kills"},{"mode",0},{"start",0},{"size",10},{"gamers",Json::array({tag})}},tokens[1])["result"]["entries"];
            return page.empty()?-1LL:page[0]["rating"].get<long long>();
        };
        check(read("Alice")==9&&read("Bob")==7&&read("Charlie")==4,"one result: every reported row that the reporters agree on");
        {
            Store store(path.string());
            Statement rounds(store.db(),"SELECT COUNT(*),SUM(resolved) FROM arbitration_rounds");(void)rounds.row();
            check(rounds.number(0)==1&&rounds.number(1)==1,"exactly one round, resolved once");
            Statement reports(store.db(),"SELECT COUNT(*) FROM arbitration_submissions");(void)reports.row();
            check(reports.number(0)==2,"one report per machine that reported");
            Statement entries(store.db(),"SELECT COUNT(*) FROM leaderboard_entries WHERE key='Kills'");(void)entries.row();
            check(entries.number(0)==3,"one row per gamer");
        }

        // A second game on the same session: a report made during it belongs to it, not to the first.
        snapshot=call(*service,"sessions.get",{{"session",session}},tokens[1])["result"];
        snapshot=call(*service,"sessions.update",settings("playing",snapshot["revision"]),tokens[1])["result"];
        const auto again=call(*service,"leaderboards.game.begin",{{"kind","local"},{"participants",Json::array({tokens[1]})}},tokens[1])["result"]["gameplay"];
        check(call(*service,"leaderboards.game.commit",{{"gameplay",again},{"entries",Json::array({row(users[1],11)})},
            {"arbitration",{{"session",session},{"revision",snapshot["revision"]}}}},tokens[1])["error"]=="OK","second game report");
        check(read("Bob")==11,"the only machine of the second game completes it at once");
        clean();
        std::cout<<"ranked migration "<<checks<<" checks passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<"ranked migration test failed: "<<error.what()<<"\n";clean();return 1;}
}
