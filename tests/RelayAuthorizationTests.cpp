// SPDX-License-Identifier: MIT
#include "CnaService/Service.hpp"
#include "CnaService/RelayProtocol.hpp"
#include <array>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
using namespace CnaService;
namespace {
int checks=0;
void check(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}
Json call(Service& service,const std::string& op,const Json& args,const std::string& token={},const std::string& game="one") {
    static int sequence=0;
    return parse(service.handle(Json{{"v",1},{"id","relay-auth-"+std::to_string(++sequence)},{"op",op},{"args",args},{"token",token},{"game",game}}.dump(),"relay-authority-test"));
}
template<class Work> void unauthorized(Work work) {
    try{work();throw std::runtime_error("expected redemption refusal");}catch(const Error& error){check(error.code()=="UNAUTHENTICATED","redemption refusal code");}
}
}
int main() {
    const auto path=std::filesystem::current_path()/"relay-authority-unit.sqlite3";
    auto clean=[&]{for(const auto* suffix:{"","-wal","-shm"})std::filesystem::remove(path.string()+suffix);};
    try {
        clean();
        {Store store(path.string());store.title("one","One");store.title("two","Two");for(const auto* name:{"alice","bob","charlie","dana","eve"})store.user(name,std::string(name)+"-password",name);}
        auto service=std::make_unique<Service>(path.string());std::array<Json,5> credentials;int index=0;
        for(const auto* name:{"alice","bob","charlie","dana","eve"}) {
            const auto value=call(*service,"auth.login",{{"username",name},{"password",std::string(name)+"-password"}});check(value["error"]=="OK","fixture login");credentials[index++]=value["result"];
        }
        auto token=[&](int i){return credentials[i]["token"].get<std::string>();};
        const auto other=call(*service,"auth.login",{{"username","alice"},{"password","alice-password"}},{},"two")["result"]["token"].get<std::string>();
        Json create{{"kind","player"},{"maxGamers",8},{"privateSlots",0},{"properties",Json::array({nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr})},
            {"allowJoinInProgress",false},{"participants",Json::array({token(0),token(2)})}};
        const auto host=call(*service,"sessions.create",create,token(0))["result"];const auto session=host["session"].get<std::string>();
        const auto remote=call(*service,"sessions.join",{{"session",session},{"participants",Json::array({token(1),token(3)})}},token(1))["result"];
        auto mint=[&](int actor,const std::string& id,const Json& participants,const std::string& game="one") {
            return call(*service,"sessions.relayTicket",{{"session",id},{"participants",participants}},token(actor),game);
        };
        auto hostTicket=[&]{return mint(0,session,Json::array({token(0),token(2)}));};
        auto remoteTicket=[&]{return mint(1,session,Json::array({token(1),token(3)}));};
        const auto hello=call(*service,"hello",Json::object())["result"]["capabilities"];
        check(std::find(hello.begin(),hello.end(),Json("relay-tickets"))!=hello.end(),"ticket capability advertised");
        check(std::find(hello.begin(),hello.end(),Json("relay"))!=hello.end(),"forwarding capability advertised");
        check(mint(0,session,Json::array({token(0)}))["error"]=="NOT_AUTHORIZED","partial local group refused");
        check(mint(2,session,Json::array({token(0),token(2)}))["error"]=="NOT_AUTHORIZED","secondary cannot impersonate machine owner");
        check(mint(0,session,Json::array({token(0),token(4)}))["error"]=="NOT_AUTHORIZED","foreign participant refused");
        check(mint(0,session,Json::array({token(0),token(0)}))["error"]=="INVALID_ARGUMENT","duplicate participant refused");
        check(mint(4,session,Json::array({token(4)}))["error"]=="NOT_AUTHORIZED","nonmember refused");
        check(call(*service,"sessions.relayTicket",{{"session",session},{"participants",Json::array({other})}},other,"two")["error"]=="NOT_FOUND","cross-title session refused");
        check(call(*service,"sessions.relayTicket",{{"session",session},{"participants",Json::array({token(0),token(2)})},{"path","../secret"}},token(0))["error"]=="INVALID_ARGUMENT","unknown fields refused");
        const auto issued=hostTicket();check(issued["error"]=="OK","complete group gets ticket");const auto result=issued["result"];
        const auto secret=result["ticket"].get<std::string>();check(secret.size()==64,"random ticket size");
        check(result["machine"]==host["machine"]&&result["session"]==session,"authority matches directory");
        check(result["expires"].get<long long>()-result["serverTime"].get<long long>()==RelayTicketLifetimeSeconds,"short ticket lifetime");
        check(result["relayVersion"]==RelayVersion&&result["maxDatagramBytes"]==MaxRelayDatagramBytes,"framing limits negotiated");
        {Store store(path.string());Statement row(store.db(),"SELECT hash,used,participants_count FROM relay_tickets WHERE hash=?");row.bind(1,sha256(secret));check(row.row()&&row.text(0)!=secret&&row.number(1)==0&&row.number(2)==2,"only ticket hash persisted");}
        unauthorized([&]{(void)service->redeemRelayTicket("two",secret);});
        for(const auto* bad:{"","../ticket","not-a-credential"})unauthorized([&]{(void)service->redeemRelayTicket("one",bad);});
        service.reset();service=std::make_unique<Service>(path.string());
        const auto grant=service->redeemRelayTicket("one",secret);check(service->validateRelayGrant(grant),"unused ticket survives restart and grants full group");
        unauthorized([&]{(void)service->redeemRelayTicket("one",secret);});
        auto forged=grant;forged.game="two";check(!service->validateRelayGrant(forged),"grant title mismatch");
        forged=grant;forged.machine=remote["machine"].get<std::string>();check(!service->validateRelayGrant(forged),"grant machine mismatch");
        forged=grant;forged.owner=credentials[1]["identity"]["userId"].get<std::string>();service->releaseRelayGrant(forged);check(service->validateRelayGrant(grant),"foreign release cannot delete grant");
        {Store store(path.string());Statement expire(store.db(),"UPDATE sessions SET expires=0 WHERE hash=?");expire.bind(1,sha256(token(2)));(void)expire.row();}
        check(hostTicket()["error"]=="NOT_AUTHORIZED","expired secondary cannot mint");
        check(service->validateRelayGrant(grant),"grant binds family not expired access token");
        auto refreshed=call(*service,"auth.refresh",{{"refreshToken",credentials[2]["refreshToken"]}});check(refreshed["error"]=="OK","secondary refresh");credentials[2]=refreshed["result"];
        check(service->validateRelayGrant(grant),"secondary access rotation preserves grant");
        refreshed=call(*service,"auth.refresh",{{"refreshToken",credentials[0]["refreshToken"]}});check(refreshed["error"]=="OK","owner refresh");credentials[0]=refreshed["result"];
        check(service->validateRelayGrant(grant),"owner access rotation preserves grant");
        {Store store(path.string());Statement revoke(store.db(),"UPDATE refresh_families SET revoked=1 WHERE user_id=?");revoke.bind(1,credentials[2]["identity"]["userId"].get<std::string>());(void)revoke.row();}
        check(!service->validateRelayGrant(grant),"secondary family revocation invalidates full machine grant");
        check(hostTicket()["error"]=="NOT_AUTHORIZED","revoked family cannot mint despite old access token");
        credentials[2]=call(*service,"auth.login",{{"username","charlie"},{"password","charlie-password"}})["result"];
        check(hostTicket()["error"]=="OK","fresh confirmed sign-in can mint");
        auto lease=[&]{Store store(path.string());Statement read(store.db(),"SELECT expires FROM directory_machines WHERE id=?");
            read.bind(1,grant.machine);(void)read.row();return read.number(0);};
        service->releaseRelayGrant(grant);check(!service->validateRelayGrant(grant),"released grant gone");
        check(lease()<=now()+20,"a machine off the relay keeps its lease only for the reconnect grace");
        auto second=hostTicket()["result"]["ticket"].get<std::string>();auto secondGrant=service->redeemRelayTicket("one",second);
        check(service->validateRelayGrant(secondGrant),"fresh full-group grant");
        check(lease()>=now()+85,"back on the relay, the machine holds its lease again");
        {Store store(path.string());Statement deny(store.db(),"UPDATE users SET online_allowed=0 WHERE username='charlie'");(void)deny.row();}
        check(!service->validateRelayGrant(secondGrant),"online privilege loss invalidates grant");
        {Store store(path.string());store.exec("UPDATE users SET online_allowed=1 WHERE username='charlie'");}
        service->releaseRelayGrant(secondGrant);
        second=hostTicket()["result"]["ticket"].get<std::string>();
        {Store store(path.string());Statement expire(store.db(),"UPDATE relay_tickets SET expires=0 WHERE hash=?");expire.bind(1,sha256(second));(void)expire.row();}
        unauthorized([&]{(void)service->redeemRelayTicket("one",second);});
        second=hostTicket()["result"]["ticket"].get<std::string>();secondGrant=service->redeemRelayTicket("one",second);
        {Store store(path.string());Statement expire(store.db(),"UPDATE relay_tickets SET grant_expires=0 WHERE hash=?");expire.bind(1,sha256(second));(void)expire.row();}
        check(!service->validateRelayGrant(secondGrant),"established grant has bounded lifetime");
        const auto remoteSecret=remoteTicket()["result"]["ticket"].get<std::string>();const auto remoteGrant=service->redeemRelayTicket("one",remoteSecret);
        check(service->validateRelayGrant(remoteGrant),"remote group authority");
        check(call(*service,"sessions.leave",{{"session",session}},token(1))["error"]=="OK","remote leaves");
        check(!service->validateRelayGrant(remoteGrant),"leave cascades grant");
        {Store store(path.string());store.exec("DELETE FROM relay_tickets");}
        for(int i=0;i<MaxMachineRelayTickets;++i)check(hostTicket()["error"]=="OK","bounded machine ticket fixture");
        check(hostTicket()["error"]=="LIMIT_EXCEEDED","machine quota before mutation");
        {Store store(path.string());store.exec("UPDATE relay_tickets SET expires=0 WHERE used=0");}
        check(hostTicket()["error"]=="OK","expired unused ticket quota reclaimed");
        {Store store(path.string());store.exec("DELETE FROM relay_tickets");
         Statement fill(store.db(),"WITH RECURSIVE n(x) AS(SELECT 1 UNION ALL SELECT x+1 FROM n WHERE x<?) INSERT INTO relay_tickets(hash,game_id,session_id,machine_id,owner_id,participants_count,created,expires,grant_expires) SELECT printf('%064x',x),'one',?,?,?,2,?,?,? FROM n");
         fill.bind(1,MaxTitleRelayTickets);fill.bind(2,session);fill.bind(3,host["machine"].get<std::string>());fill.bind(4,credentials[0]["identity"]["userId"].get<std::string>());fill.bind(5,now());fill.bind(6,now()+60);fill.bind(7,now()+3600);(void)fill.row();}
        check(call(*service,"sessions.join",{{"session",session},{"participants",Json::array({token(1),token(3)})}},token(1))["error"]=="OK","remote rejoin");
        check(remoteTicket()["error"]=="LIMIT_EXCEEDED","title ticket cap before mutation");
        auto otherCreate=create;otherCreate["participants"]=Json::array({other});const auto otherSession=call(*service,"sessions.create",otherCreate,other,"two")["result"]["session"].get<std::string>();
        check(call(*service,"sessions.relayTicket",{{"session",otherSession},{"participants",Json::array({other})}},other,"two")["error"]=="OK","title quotas isolated");
        {Store store(path.string());store.exec("DELETE FROM relay_tickets");}
        second=hostTicket()["result"]["ticket"].get<std::string>();secondGrant=service->redeemRelayTicket("one",second);
        {Store store(path.string());Statement expire(store.db(),"UPDATE directory_machines SET expires=0 WHERE id=?");expire.bind(1,host["machine"].get<std::string>());(void)expire.row();}
        check(!service->validateRelayGrant(secondGrant),"host expiry cascades all session grants");
        service.reset();clean();std::cout<<checks<<" relay authority assertions passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
