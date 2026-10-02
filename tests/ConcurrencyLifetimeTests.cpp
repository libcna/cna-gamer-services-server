// SPDX-License-Identifier: MIT
#include "CnaService/Service.hpp"
#include <array>
#include <atomic>
#include <barrier>
#include <filesystem>
#include <iostream>
#include <thread>
using namespace CnaService;
namespace {
int checks=0;
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
Json call(Service& s,const std::string& op,Json args,const std::string& token={}) {
    static std::atomic<int> sequence=0;
    Json r{{"v",1},{"id","lifetime-"+std::to_string(++sequence)},{"game","one"},{"op",op},{"args",std::move(args)}};
    if(!token.empty())r["token"]=token;
    return parse(s.handle(r.dump(),"lifetime-test"));
}
}
int main() {
    const auto path=(std::filesystem::current_path()/"concurrency-lifetime.sqlite3").string();
    auto clean=[&]{for(const auto* suffix:{"","-wal","-shm"})std::filesystem::remove(path+suffix);};
    try {
        clean();
        {Store db(path);db.title("one","One");for(const auto* name:{"alice","bob","carol"})(void)db.user(name,std::string(name)+"-password",std::string(1,name[0]-32)+(name+1));}
        Service service(path);std::array<std::string,3> tokens;
        int index=0;for(const auto* name:{"alice","bob","carol"}){const auto r=call(service,"auth.login",{{"username",name},{"password",std::string(name)+"-password"}});check(r["error"]=="OK","login");tokens[index++]=r["result"]["token"];}
        const auto created=call(service,"sessions.create",{{"kind","player"},{"maxGamers",2},{"privateSlots",0},{"properties",Json::array({nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr})},
            {"allowJoinInProgress",true},{"participants",Json::array({tokens[0]})}},tokens[0]);
        check(created["error"]=="OK","session create");const auto session=created["result"]["session"].get<std::string>();
        const auto invited=call(service,"invites.send",{{"session",session},{"gamertag","Bob"}},tokens[0]);check(invited["error"]=="OK","invite");const auto invitation=invited["result"]["invite"];
        const auto ticket=call(service,"sessions.relayTicket",{{"session",session},{"participants",Json::array({tokens[0]})}},tokens[0]);check(ticket["error"]=="OK","ticket");
        const auto grant=service.redeemRelayTicket("one",ticket["result"]["ticket"]);check(service.validateRelayGrant(grant),"grant alive");
        std::barrier start(2);std::array<Json,2> replies;std::array<std::thread,2> workers;
        for(int i=0;i<2;++i)workers[i]=std::thread([&,i]{start.arrive_and_wait();replies[i]=call(service,"sessions.join",{{"session",session},{"participants",Json::array({tokens[i+1]})}},tokens[i+1]);});
        for(auto& worker:workers)worker.join();
        check((replies[0]["error"]=="OK"&&replies[1]["error"]=="SESSION_FULL")||(replies[1]["error"]=="OK"&&replies[0]["error"]=="SESSION_FULL"),"only one concurrent join wins last slot");
        const auto roster=call(service,"sessions.get",{{"session",session}},tokens[0]);check(roster["error"]=="OK"&&roster["result"]["currentGamers"]==2&&roster["result"]["members"].size()==2,"capacity and roster consistent");
        // Race a final authorized snapshot against host deletion. Either serial order is valid.
        std::array<Json,2> ending;
        std::thread reader([&]{start.arrive_and_wait();ending[0]=call(service,"sessions.get",{{"session",session}},tokens[0]);});
        std::thread destroyer([&]{start.arrive_and_wait();ending[1]=call(service,"sessions.leave",{{"session",session}},tokens[0]);});
        reader.join();destroyer.join();
        check(ending[0]["error"]=="OK"||ending[0]["error"]=="NOT_FOUND","snapshot/deletion has a valid serial order");
        check(ending[1]["error"]=="OK"&&ending[1]["result"]["ended"]==true,"host leave destroys nonmigrating session");
        check(!service.validateRelayGrant(grant),"destroyed session invalidates retained grant");service.releaseRelayGrant(grant);service.releaseRelayGrant(grant);
        check(call(service,"sessions.get",{{"session",session}},tokens[0])["error"]=="NOT_FOUND","dead session lookup");
        check(call(service,"sessions.leave",{{"session",session}},tokens[0])["error"]=="NOT_FOUND","double leave cannot resurrect session");
        check(call(service,"invites.accept",{{"invite",invitation}},tokens[1])["error"]=="NOT_FOUND","dead session invitation gone");
        check(call(service,"sessions.join",{{"session",session},{"participants",Json::array({tokens[1]})}},tokens[1])["error"]=="NOT_FOUND","stale reconnect refused");
        {Store db(path);for(const auto* table:{"directory_sessions","directory_machines","directory_members","relay_tickets","relay_ticket_members","session_invitations"}) {
            const auto sql=std::string("SELECT COUNT(*) FROM ")+table;Statement rows(db.db(),sql.c_str());check(rows.row()&&rows.number(0)==0,"dependent state removed");}}
        clean();std::cout<<checks<<" concurrency/lifetime checks passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
