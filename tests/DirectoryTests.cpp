// SPDX-License-Identifier: MIT
#include "CnaService/Service.hpp"
#include <filesystem>
#include <iostream>
#include <array>
using namespace CnaService;
namespace {
int checks=0;
void check(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}
Json call(Service& service,const std::string& op,const Json& args,const std::string& token,const std::string& title="one") {
    static int sequence=0;
    return parse(service.handle(Json{{"v",1},{"id","directory-"+std::to_string(++sequence)},{"op",op},{"game",title},{"token",token},{"args",args}}.dump(),"directory-test"));
}
}
int main() {
    const auto path=std::filesystem::current_path()/"directory-unit.sqlite3";
    auto clean=[&]{for(const auto* suffix:{"","-wal","-shm"})std::filesystem::remove(path.string()+suffix);};
    try {
        clean();
        {Store store(path.string());store.title("one","One");store.title("two","Two");
         for(const auto* name:{"alice","bob","charlie","dana","eve","fred"})store.user(name,std::string(name)+"-password",name);
         store.exec("UPDATE users SET online_allowed=0 WHERE username='fred'");}
        Service service(path.string());std::array<std::string,6> tokens;
        int index=0;for(const auto* name:{"alice","bob","charlie","dana","eve","fred"}) {
            auto login=call(service,"auth.login",{{"username",name},{"password",std::string(name)+"-password"}},{});
            check(login["error"]=="OK","fixture authentication");tokens[index++]=login["result"]["token"].get<std::string>();
        }
        const auto other=call(service,"auth.login",{{"username","alice"},{"password","alice-password"}},{},"two")["result"]["token"].get<std::string>();
        auto properties=Json::array({42,nullptr,-2147483648LL,nullptr,nullptr,nullptr,nullptr,2147483647LL});
        auto wildcard=Json::array({nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr});
        for(std::size_t slot=0;slot<SessionPropertyCount;++slot)for(const auto& value:{Json(-2147483648LL),Json(2147483647LL),Json(nullptr)}) {
            auto candidate=wildcard;candidate[slot]=value;validateSessionProperties(candidate);check(true,"all property slots and bounds");
        }
        for(const auto& candidate:{Json::array(),Json::array({nullptr}),Json::array({1,2,3,4,5,6,7,8,9}),Json::object(),Json::array({true,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr}),Json::array({2147483648LL,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr}),Json::array({18446744073709551615ULL,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr}),Json::array({1.5,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr})}) {
            bool rejected=false;try{validateSessionProperties(candidate);}catch(const Error& error){rejected=error.code()=="INVALID_ARGUMENT";}
            check(rejected,"invalid property schema");
        }
        Json create{{"kind","player"},{"maxGamers",8},{"privateSlots",1},{"properties",properties},{"allowJoinInProgress",false},{"participants",Json::array({tokens[0],tokens[1],tokens[2],tokens[3]})}};
        Json find{{"kind","player"},{"localCount",1},{"properties",wildcard},{"start",0},{"limit",32}};
        auto response=call(service,"sessions.create",create,tokens[0]);check(response["error"]=="OK","four local create");
        auto snapshot=response["result"];const auto session=snapshot["session"].get<std::string>();
        check(snapshot["members"].size()==4&&snapshot["currentGamers"]==4&&snapshot["openPublicSlots"]==3&&snapshot["openPrivateSlots"]==1,"separate local membership and capacity");
        check(snapshot["properties"]==properties&&snapshot["state"]=="lobby"&&snapshot["kind"]=="player","directory metadata");
        check(snapshot["machine"]==snapshot["hostMachine"]&&snapshot["members"][0]["ordinal"]==0,"host machine and order");
        check(call(service,"sessions.create",create,tokens[0])["error"]=="INVALID_STATE","account cannot duplicate title membership");
        check(call(service,"sessions.find",find,tokens[4])["result"]["sessions"].size()==1,"remote discovery");
        check(call(service,"sessions.find",find,tokens[0])["result"]["sessions"].empty(),"own membership not rediscovered");
        check(call(service,"sessions.find",find,tokens[5])["error"]=="NOT_AUTHORIZED","online privilege required");
        auto filtered=find;filtered["properties"][0]=43;check(call(service,"sessions.find",filtered,tokens[4])["result"]["sessions"].empty(),"property mismatch");
        filtered["properties"][0]=42;filtered["properties"][2]=-2147483648LL;check(call(service,"sessions.find",filtered,tokens[4])["result"]["sessions"].size()==1,"matching sparse properties");
        filtered["properties"][1]=0;check(call(service,"sessions.find",filtered,tokens[4])["result"]["sessions"].empty(),"null advertised does not match value");
        filtered=find;filtered["localCount"]=4;check(call(service,"sessions.find",filtered,tokens[4])["result"]["sessions"].empty(),"available public capacity filters all locals");
        filtered=find;filtered["kind"]="ranked";check(call(service,"sessions.find",filtered,tokens[4])["result"]["sessions"].empty(),"type isolation");
        check(call(service,"sessions.find",find,other,"two")["result"]["sessions"].empty(),"title isolation");
        check(call(service,"sessions.get",{{"session",session}},other,"two")["error"]=="NOT_FOUND","session cross-title lookup refused");
        check(call(service,"sessions.get",{{"session",session}},tokens[4])["error"]=="NOT_AUTHORIZED","nonmember snapshot refused");
        check(call(service,"sessions.join",{{"session",session},{"participants",Json::array({tokens[4],tokens[4]})}},tokens[4])["error"]=="INVALID_ARGUMENT","duplicate local participants");
        check(call(service,"sessions.join",{{"session",session},{"participants",Json::array({tokens[4],other})}},tokens[4])["error"]=="NOT_AUTHORIZED","cross-title participant credentials");
        check(call(service,"sessions.join",{{"session",session},{"participants",Json::array({tokens[4],tokens[5]})}},tokens[4])["error"]=="NOT_AUTHORIZED","unauthorized local participant");
        check(call(service,"sessions.get",{{"session",session}},tokens[0])["result"]["currentGamers"]==4,"failed multi-local join is atomic");
        auto joined=call(service,"sessions.join",{{"session",session},{"participants",Json::array({tokens[4]})}},tokens[4]);check(joined["error"]=="OK","remote join");
        check(joined["result"]["members"].size()==5&&joined["result"]["openPublicSlots"]==2&&joined["result"]["members"][4]["ordinal"]==4,"membership capacity/order updated");
        check(joined["result"]["machine"]!=snapshot["machine"],"distinct client machine");
        check(call(service,"sessions.join",{{"session",session},{"participants",Json::array({tokens[4]})}},tokens[4])["result"]["revision"]==joined["result"]["revision"],"identical joined group retry is idempotent");
        check(call(service,"sessions.touch",{{"session",session}},tokens[1])["error"]=="NOT_AUTHORIZED","other local member cannot impersonate machine owner");
        check(call(service,"sessions.touch",{{"session",session}},tokens[0])["error"]=="OK","host heartbeat");
        snapshot=call(service,"sessions.get",{{"session",session}},tokens[0])["result"];
        Json update{{"session",session},{"revision",snapshot["revision"]},{"maxGamers",8},{"privateSlots",1},{"properties",properties},{"allowJoinInProgress",false},{"state","playing"}};
        check(call(service,"sessions.update",update,tokens[4])["error"]=="NOT_AUTHORIZED","host update authorization");
        auto playing=call(service,"sessions.update",update,tokens[0]);check(playing["error"]=="OK"&&playing["result"]["state"]=="playing","host starts game");
        check(call(service,"sessions.update",update,tokens[0])["error"]=="CONFLICT","stale host revision refused");
        check(call(service,"sessions.leave",{{"session",session}},tokens[4])["result"]["ended"]==false,"remote leave releases machine");
        check(call(service,"sessions.find",find,tokens[4])["result"]["sessions"].empty(),"playing without join-in-progress hidden");
        check(call(service,"sessions.join",{{"session",session},{"participants",Json::array({tokens[4]})}},tokens[4])["error"]=="INVALID_STATE","playing joins blocked");
        update["revision"]=call(service,"sessions.get",{{"session",session}},tokens[0])["result"]["revision"];update["allowJoinInProgress"]=true;update["properties"][0]=99;
        check(call(service,"sessions.update",update,tokens[0])["error"]=="OK","host enables join and changes properties");
        check(call(service,"sessions.find",find,tokens[4])["result"]["sessions"][0]["properties"][0]==99,"host properties synchronized in directory");
        check(call(service,"sessions.join",{{"session",session},{"participants",Json::array({tokens[4]})}},tokens[4])["error"]=="OK","join in progress");
        {Service restarted(path.string());check(call(restarted,"sessions.get",{{"session",session}},tokens[4])["result"]["members"].size()==5,"restart preserves live membership");}
        update["revision"]=call(service,"sessions.get",{{"session",session}},tokens[0])["result"]["revision"];update["maxGamers"]=4;
        check(call(service,"sessions.update",update,tokens[0])["error"]=="INVALID_ARGUMENT","cannot shrink below allocated capacity");
        {Store admin(path.string());Statement expire(admin.db(),"UPDATE directory_machines SET expires=0 WHERE owner_id=(SELECT id FROM users WHERE username='eve')");(void)expire.row();}
        auto pruned=call(service,"sessions.get",{{"session",session}},tokens[0])["result"];check(pruned["currentGamers"]==4&&pruned["revision"]>update["revision"],"stale remote membership released with revision");
        check(call(service,"sessions.get",{{"session",session}},tokens[4])["error"]=="NOT_AUTHORIZED","expired member loses authority");
        check(call(service,"sessions.leave",{{"session",session}},tokens[0])["result"]["ended"]==true,"host leave closes session");
        check(call(service,"sessions.get",{{"session",session}},tokens[1])["error"]=="NOT_FOUND","host close releases every local member");
        create["kind"]="ranked";create["maxGamers"]=4;create["privateSlots"]=0;create["participants"]=Json::array({tokens[0]});
        auto invalidRanked=create;invalidRanked["allowJoinInProgress"]=true;
        check(call(service,"sessions.create",invalidRanked,tokens[0])["error"]=="INVALID_ARGUMENT","ranked join-in-progress create refused");
        auto ranked=call(service,"sessions.create",create,tokens[0]);check(ranked["error"]=="OK"&&ranked["result"]["kind"]=="ranked","ranked directory create");
        find["kind"]="ranked";check(call(service,"sessions.find",find,tokens[4])["result"]["sessions"].size()==1,"ranked directory discovery");
        const auto rankedId=ranked["result"]["session"].get<std::string>();
        check(call(service,"sessions.join",{{"session",rankedId},{"participants",Json::array({tokens[4]})}},tokens[4])["error"]=="OK","ranked directory join");
        auto rankSnapshot=call(service,"sessions.get",{{"session",rankedId}},tokens[0])["result"];
        Json rankUpdate{{"session",rankedId},{"revision",rankSnapshot["revision"]},{"maxGamers",4},{"privateSlots",0},
            {"properties",properties},{"allowJoinInProgress",true},{"state","lobby"}};
        check(call(service,"sessions.update",rankUpdate,tokens[0])["error"]=="INVALID_ARGUMENT","ranked join-in-progress update refused");
        check(call(service,"sessions.get",{{"session",rankedId}},tokens[0])["result"]==rankSnapshot,"refused ranked update is atomic");
        auto rankInvite=call(service,"invites.send",{{"session",rankedId},{"gamertag","bob"}},tokens[0])["result"];
        const auto rankInviteId=rankInvite["invite"].get<std::string>();
        check(call(service,"invites.accept",{{"invite",rankInviteId}},tokens[1])["error"]=="OK","ranked lobby invitation accepted");
        rankUpdate["allowJoinInProgress"]=false;rankUpdate["state"]="playing";
        check(call(service,"sessions.update",rankUpdate,tokens[0])["error"]=="OK","ranked lobby transitions to gameplay");
        check(call(service,"sessions.find",find,tokens[1])["result"]["sessions"].empty(),"playing ranked sessions hidden");
        check(call(service,"sessions.join",{{"session",rankedId},{"participants",Json::array({tokens[1]})}},tokens[1])["error"]=="INVALID_STATE","ranked ordinary gameplay join refused");
        check(call(service,"sessions.joinInvited",{{"session",rankedId},{"invite",rankInviteId},{"participants",Json::array({tokens[1]})}},tokens[1])["error"]=="INVALID_STATE","ranked invited gameplay join refused");
        check(call(service,"invites.get",{{"invite",rankInviteId}},tokens[1])["result"]["status"]=="accepted","refused ranked join does not consume invitation");
        check(call(service,"sessions.get",{{"session",rankedId}},tokens[0])["result"]["currentGamers"]==2,"refused ranked joins do not add members");
        check(call(service,"sessions.join",{{"session",rankedId},{"participants",Json::array({tokens[4]})}},tokens[4])["error"]=="OK","existing ranked membership replay remains idempotent");
        const auto priorRevision=call(service,"sessions.get",{{"session",rankedId}},tokens[0])["result"]["revision"].get<long long>();
        {Store admin(path.string());admin.exec("DROP TABLE directory_removals; DROP TABLE avatar_catalog_assets; DROP TABLE avatar_catalog_items; DROP TABLE avatar_catalogs; DROP TABLE avatars; DROP TABLE player_reviews; DROP TABLE messages; DROP TABLE arbitration_submissions; DROP TABLE arbitration_rounds; UPDATE directory_sessions SET allow_join=1 WHERE kind='ranked'; PRAGMA user_version=8;");}
        check(call(service,"sessions.find",find,tokens[1])["result"]["sessions"].empty(),"legacy ranked flag cannot expose gameplay");
        check(call(service,"sessions.join",{{"session",rankedId},{"participants",Json::array({tokens[1]})}},tokens[1])["error"]=="INVALID_STATE","legacy ranked flag cannot allow gameplay join");
        {Store upgraded(path.string());Statement version(upgraded.db(),"PRAGMA user_version");check(version.row()&&version.number(0)==SchemaVersion,"ranked policy schema upgrade");}
        auto migrated=call(service,"sessions.get",{{"session",rankedId}},tokens[0])["result"];
        check(migrated["allowJoinInProgress"]==false&&migrated["revision"]==priorRevision+1,"migration repairs flag and revision");
        check(migrated["currentGamers"]==2,"migration preserves authenticated membership");
        {Service restarted(path.string());check(call(restarted,"sessions.get",{{"session",rankedId}},tokens[4])["result"]["allowJoinInProgress"]==false,"ranked policy persists across restart");}
        {Store admin(path.string());admin.exec("UPDATE directory_sessions SET expires=0");}
        check(call(service,"sessions.find",find,tokens[1])["result"]["sessions"].empty(),"host expiry removes stale directory");
        create["kind"]="player";create["maxGamers"]=2;create["privateSlots"]=1;
        auto full=call(service,"sessions.create",create,tokens[0]);check(full["error"]=="OK","one public slot consumed by host");
        auto fullId=full["result"]["session"].get<std::string>();
        check(call(service,"sessions.join",{{"session",fullId},{"participants",Json::array({tokens[4]})}},tokens[4])["error"]=="SESSION_FULL","public join cannot consume private reservation");
        check(call(service,"sessions.get",{{"session","../database"}},tokens[0])["error"]=="INVALID_ARGUMENT","no caller paths");
        check(call(service,"sessions.leave",{{"session",fullId}},tokens[0])["error"]=="OK","cleanup full session");
        // XNA NetworkMachine.RemoveFromSession: only the host, never its own machine; the removed
        // users hear REMOVED_BY_HOST, relay tickets included, and everyone else keeps playing.
        {
            create["kind"]="player";create["maxGamers"]=8;create["privateSlots"]=0;create["participants"]=Json::array({tokens[0]});
            auto hosted=call(service,"sessions.create",create,tokens[0]);check(hosted["error"]=="OK","removal fixture host");
            const auto hostedId=hosted["result"]["session"].get<std::string>();const auto hostMachine=hosted["result"]["machine"].get<std::string>();
            auto guest=call(service,"sessions.join",{{"session",hostedId},{"participants",Json::array({tokens[1],tokens[2]})}},tokens[1]);
            auto other=call(service,"sessions.join",{{"session",hostedId},{"participants",Json::array({tokens[3]})}},tokens[3]);
            check(guest["error"]=="OK"&&other["error"]=="OK","removal fixture joins");
            const auto guestMachine=guest["result"]["machine"].get<std::string>();
            const auto before=call(service,"sessions.get",{{"session",hostedId}},tokens[0])["result"]["revision"].get<long long>();
            check(call(service,"sessions.remove",{{"session",hostedId},{"machine",guestMachine}},tokens[3])["error"]=="NOT_AUTHORIZED","only the host removes");
            check(call(service,"sessions.remove",{{"session",hostedId},{"machine",hostMachine}},tokens[0])["error"]=="INVALID_ARGUMENT","host cannot remove itself");
            check(call(service,"sessions.remove",{{"session",hostedId},{"machine","0123456789abcdef0123456789abcdef"}},tokens[0])["error"]=="NOT_FOUND","unknown machine");
            check(call(service,"sessions.remove",{{"session",hostedId},{"machine","../x"}},tokens[0])["error"]=="INVALID_ARGUMENT","machine id format");
            auto removed=call(service,"sessions.remove",{{"session",hostedId},{"machine",guestMachine}},tokens[0]);
            check(removed["error"]=="OK"&&removed["result"]["members"].size()==2&&removed["result"]["revision"].get<long long>()==before+1,"machine removed, revision advanced");
            for(const auto& user:{tokens[1],tokens[2]})
                check(call(service,"sessions.get",{{"session",hostedId}},user)["error"]=="REMOVED_BY_HOST","removed user is told why");
            check(call(service,"sessions.touch",{{"session",hostedId}},tokens[1])["error"]=="REMOVED_BY_HOST","removed machine heartbeat");
            check(call(service,"sessions.relayTicket",{{"session",hostedId},{"participants",Json::array({tokens[1],tokens[2]})}},tokens[1])["error"]=="REMOVED_BY_HOST","removed machine gets no relay ticket");
            check(call(service,"sessions.get",{{"session",hostedId}},tokens[3])["error"]=="OK","other machine unaffected");
            check(call(service,"sessions.get",{{"session",hostedId}},tokens[4])["error"]=="NOT_AUTHORIZED","never-member still NOT_AUTHORIZED");
            check(call(service,"sessions.leave",{{"session",hostedId}},tokens[0])["result"]["ended"]==true,"cleanup removal fixture");
        }
        for(const auto& bad:{Json(0),Json(5),Json(-1),Json(18446744073709551615ULL),Json(1.5),Json(true)}) {
            auto request=find;request["localCount"]=bad;check(call(service,"sessions.find",request,tokens[1])["error"]=="INVALID_ARGUMENT","bounded search locals");
        }
        for(const auto& bad:{Json(0),Json(33),Json(18446744073709551615ULL)}) {
            auto request=find;request["limit"]=bad;check(call(service,"sessions.find",request,tokens[1])["error"]=="INVALID_ARGUMENT","bounded listing limit");
        }
        auto unknown=find;unknown["path"]="/tmp/data";check(call(service,"sessions.find",unknown,tokens[1])["error"]=="INVALID_ARGUMENT","unknown arguments refused");
        unknown=find;unknown.erase("start");check(call(service,"sessions.find",unknown,tokens[1])["error"]=="INVALID_ARGUMENT","missing arguments refused");
        create["kind"]="systemlink";check(call(service,"sessions.create",create,tokens[0])["error"]=="INVALID_ARGUMENT","SystemLink stays outside central directory");
        clean();std::cout<<checks<<" directory assertions passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
