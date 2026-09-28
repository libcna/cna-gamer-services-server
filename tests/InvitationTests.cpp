// SPDX-License-Identifier: MIT
#include "CnaService/Service.hpp"
#include <array>
#include <filesystem>
#include <iostream>
#include <memory>
using namespace CnaService;
namespace {
int checks=0;
void check(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}
Json call(Service& service,const std::string& op,const Json& args,const std::string& token,const std::string& title="one") {
    static int sequence=0;
    return parse(service.handle(Json{{"v",1},{"id","invite-test-"+std::to_string(++sequence)},{"op",op},{"game",title},{"token",token},{"args",args}}.dump(),"invite-unit"));
}
}
int main() {
    const auto path=std::filesystem::current_path()/"invitations-unit.sqlite3";
    auto clean=[&]{for(const auto* suffix:{"","-wal","-shm"})std::filesystem::remove(path.string()+suffix);};
    try {
        clean();
        std::array<std::string,6> tokens;
        {Store store(path.string());store.title("one","One");store.title("two","Two");
         for(const auto* name:{"alice","bob","charlie","dana","eve","fred"})store.user(name,std::string(name)+"-password",name);
         store.exec("UPDATE users SET online_allowed=0 WHERE username='fred'");}
        auto service=std::make_unique<Service>(path.string());
        int index=0;for(const auto* name:{"alice","bob","charlie","dana","eve","fred"}) {
            const auto login=call(*service,"auth.login",{{"username",name},{"password",std::string(name)+"-password"}},{});
            check(login["error"]=="OK","fixture authentication");tokens[index++]=login["result"]["token"].get<std::string>();
        }
        const auto other=call(*service,"auth.login",{{"username","bob"},{"password","bob-password"}},{},"two")["result"]["token"].get<std::string>();
        Json create{{"kind","player"},{"maxGamers",4},{"privateSlots",2},{"properties",Json::array({nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr})},{"allowJoinInProgress",false},{"participants",Json::array({tokens[0],tokens[2]})}};
        const auto created=call(*service,"sessions.create",create,tokens[0]);check(created["error"]=="OK","host with two authenticated locals");
        const auto session=created["result"]["session"].get<std::string>();
        auto send=[&](const char* target,const std::string& actor){return call(*service,"invites.send",{{"session",session},{"gamertag",target}},actor);};
        check(send("bob",tokens[4])["error"]=="NOT_AUTHORIZED","nonmember cannot invite");
        check(send("alice",tokens[0])["error"]=="INVALID_ARGUMENT","cannot invite self");
        check(send("charlie",tokens[0])["error"]=="INVALID_STATE","cannot invite existing local member");
        check(send("missing",tokens[0])["error"]=="NOT_FOUND","unknown recipient");
        check(send("fred",tokens[0])["error"]=="NOT_AUTHORIZED","recipient online privilege");
        auto sent=send("bob",tokens[0]);check(sent["error"]=="OK","real recipient invitation");
        const auto invite=sent["result"]["invite"].get<std::string>();
        check(invite.size()==32&&sent["result"]["session"]==session&&sent["result"]["status"]=="pending","opaque invitation identity and pending state");
        check(sent["result"]["expires"].get<long long>()-sent["result"]["created"].get<long long>()==InviteLifetimeSeconds,"bounded invitation TTL");
        check(send("bob",tokens[0])["result"]==sent["result"],"duplicate pending send is idempotent without extending TTL");
        const auto inbox=call(*service,"invites.list",{{"start",0},{"limit",32}},tokens[1]);
        check(inbox["error"]=="OK"&&inbox["result"]["invites"].size()==1&&!inbox["result"]["more"].get<bool>(),"authenticated recipient inbox");
        check(call(*service,"invites.list",{{"start",0},{"limit",32}},tokens[0])["result"]["invites"].empty(),"sender is not recipient");
        check(call(*service,"invites.get",{{"invite",invite}},tokens[2])["error"]=="NOT_AUTHORIZED","other account cannot inspect invitation");
        check(call(*service,"invites.accept",{{"invite",invite}},tokens[0])["error"]=="NOT_AUTHORIZED","sender cannot accept for recipient");
        check(call(*service,"invites.get",{{"invite",invite}},other,"two")["error"]=="NOT_FOUND","title isolation");
        Json join{{"session",session},{"invite",invite},{"participants",Json::array({tokens[1],tokens[3]})}};
        check(call(*service,"sessions.join",{{"session",session},{"participants",Json::array({tokens[1]})}},tokens[1])["error"]=="SESSION_FULL","ordinary join cannot consume private capacity");
        check(call(*service,"sessions.joinInvited",join,tokens[1])["error"]=="INVALID_STATE","receipt is not user acceptance");
        auto accepted=call(*service,"invites.accept",{{"invite",invite}},tokens[1]);
        check(accepted["error"]=="OK"&&accepted["result"]["status"]=="accepted"&&accepted["result"]["acceptedAt"].get<long long>()>0,"explicit acceptance transition");
        check(call(*service,"invites.accept",{{"invite",invite}},tokens[1])["result"]==accepted["result"],"accept retry does not repeat transition");
        service.reset();service=std::make_unique<Service>(path.string());
        check(call(*service,"invites.get",{{"invite",invite}},tokens[1])["result"]==accepted["result"],"accepted invitation persists through restart");
        auto unauthorized=join;unauthorized["participants"]=Json::array({tokens[4]});
        check(call(*service,"sessions.joinInvited",unauthorized,tokens[4])["error"]=="NOT_AUTHORIZED","invite ID alone grants no authority");
        unauthorized=join;unauthorized["participants"]=Json::array({tokens[1],other});
        check(call(*service,"sessions.joinInvited",unauthorized,tokens[1])["error"]=="NOT_AUTHORIZED","all participants must authenticate same title");
        const auto joined=call(*service,"sessions.joinInvited",join,tokens[1]);check(joined["error"]=="OK","recipient plus second local joins private slots");
        check(joined["result"]["currentGamers"]==4&&joined["result"]["openPrivateSlots"]==0&&joined["result"]["members"][2]["privateSlot"]==true&&joined["result"]["members"][3]["privateSlot"]==true,"all local allocations atomic and private first");
        check(call(*service,"invites.get",{{"invite",invite}},tokens[1])["result"]["status"]=="used","invitation consumed with membership commit");
        check(call(*service,"sessions.joinInvited",join,tokens[1])["result"]==joined["result"],"same joined group replay is idempotent");
        unauthorized=join;unauthorized["participants"]=Json::array({tokens[1]});
        check(call(*service,"sessions.joinInvited",unauthorized,tokens[1])["error"]=="INVALID_STATE","used token cannot change local group");
        check(call(*service,"invites.accept",{{"invite",invite}},tokens[1])["error"]=="INVALID_STATE","consumed token cannot be accepted again");
        check(call(*service,"invites.dismiss",{{"invite",invite}},tokens[1])["error"]=="INVALID_STATE","consumed token cannot be dismissed");
        check(call(*service,"sessions.leave",{{"session",session}},tokens[1])["error"]=="OK","grouped leave");
        check(call(*service,"sessions.joinInvited",join,tokens[1])["error"]=="INVALID_STATE","used token cannot resurrect membership after leave");
        auto second=send("bob",tokens[2]);check(second["error"]=="OK","another active local may invite");
        const auto secondId=second["result"]["invite"].get<std::string>();
        check(call(*service,"invites.dismiss",{{"invite",secondId}},tokens[1])["result"]["status"]=="dismissed","recipient dismisses");
        check(call(*service,"invites.dismiss",{{"invite",secondId}},tokens[1])["result"]["status"]=="dismissed","dismiss retry idempotent");
        check(call(*service,"invites.accept",{{"invite",secondId}},tokens[1])["error"]=="INVALID_STATE","dismissed token cannot accept");
        second=send("bob",tokens[0]);const auto expired=second["result"]["invite"].get<std::string>();
        {Store store(path.string());Statement expire(store.db(),"UPDATE session_invitations SET expires=0 WHERE id=?");expire.bind(1,expired);(void)expire.row();}
        check(call(*service,"invites.accept",{{"invite",expired}},tokens[1])["error"]=="INVALID_STATE","expired invite rejected deterministically");
        check(call(*service,"invites.list",{{"start",0},{"limit",32}},tokens[1])["result"]["invites"].empty(),"expired and terminal entries hidden");
        for(const auto& value:{Json(-1),Json(65),Json(18446744073709551615ULL),Json(true),Json(1.5)})
            check(call(*service,"invites.list",{{"start",value},{"limit",32}},tokens[1])["error"]=="INVALID_ARGUMENT","bounded inbox offset");
        for(const auto& value:{Json(0),Json(33),Json(18446744073709551615ULL)})
            check(call(*service,"invites.list",{{"start",0},{"limit",value}},tokens[1])["error"]=="INVALID_ARGUMENT","bounded inbox page");
        for(const char* bad:{"../db","AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","","000000000000000000000000000000000"})
            check(call(*service,"invites.get",{{"invite",bad}},tokens[1])["error"]=="INVALID_ARGUMENT","strict opaque token grammar");
        check(call(*service,"invites.get",{{"invite",std::string(32,'0')}},tokens[1])["error"]=="NOT_FOUND","unknown invite");
        check(call(*service,"invites.list",{{"start",0},{"limit",32},{"path","/tmp/data"}},tokens[1])["error"]=="INVALID_ARGUMENT","unknown fields refused");
        check(call(*service,"invites.list",{{"start",0}},tokens[1])["error"]=="INVALID_ARGUMENT","missing fields refused");
        // Quota accounting retains dismissed invitations; callers cannot clear their rate history.
        for(int n=0;n<MaxHourlyInvites-2;++n) {
            const auto issued=send("eve",tokens[2]);check(issued["error"]=="OK","quota fixture send");
            check(call(*service,"invites.dismiss",{{"invite",issued["result"]["invite"]}},tokens[4])["error"]=="OK","quota fixture dismissed");
        }
        const auto issued=send("eve",tokens[2]);check(issued["error"]=="OK","last quota slot");
        check(send("eve",tokens[2])["result"]["invite"]==issued["result"]["invite"],"duplicate does not consume exhausted quota");
        check(send("dana",tokens[2])["error"]=="RATE_LIMITED","hourly sender quota before mutation");
        const auto closedInvite=issued["result"]["invite"].get<std::string>();
        check(call(*service,"sessions.leave",{{"session",session}},tokens[0])["result"]["ended"]==true,"host close");
        check(call(*service,"invites.get",{{"invite",closedInvite}},tokens[4])["error"]=="NOT_FOUND","closed session cascades invitation authority");
        const auto replacement=call(*service,"sessions.create",create,tokens[0]);check(replacement["error"]=="OK","replacement host after close");
        check(call(*service,"invites.send",{{"session",replacement["result"]["session"]},{"gamertag","eve"}},tokens[2])["error"]=="RATE_LIMITED","sender quota survives host close and fresh session");
        const auto replacementId=replacement["result"]["session"].get<std::string>();
        check(call(*service,"sessions.leave",{{"session",replacementId}},tokens[0])["error"]=="OK","replacement cleanup");
        create["kind"]="ranked";create["maxGamers"]=3;create["privateSlots"]=1;create["participants"]=Json::array({tokens[0]});
        const auto mixed=call(*service,"sessions.create",create,tokens[0]);check(mixed["error"]=="OK","ranked invitation directory fixture");
        const auto mixedId=mixed["result"]["session"].get<std::string>();
        auto mixedInvite=call(*service,"invites.send",{{"session",mixedId},{"gamertag","bob"}},tokens[0]);check(mixedInvite["error"]=="OK","ranked CNA control invitation");
        const auto mixedToken=mixedInvite["result"]["invite"].get<std::string>();
        check(call(*service,"invites.accept",{{"invite",mixedToken}},tokens[1])["error"]=="OK","mixed invitation acceptance");
        Json mixedJoin{{"session",mixedId},{"invite",mixedToken},{"participants",Json::array({tokens[1],tokens[3]})}};
        Json update{{"session",mixedId},{"revision",mixed["result"]["revision"]},{"maxGamers",3},{"privateSlots",1},{"properties",create["properties"]},{"allowJoinInProgress",false},{"state","playing"}};
        const auto playing=call(*service,"sessions.update",update,tokens[0]);check(playing["error"]=="OK","playing without join in progress");
        check(call(*service,"sessions.joinInvited",mixedJoin,tokens[1])["error"]=="INVALID_STATE","accepted invitation respects playing state");
        check(call(*service,"invites.get",{{"invite",mixedToken}},tokens[1])["result"]["status"]=="accepted","rejected join leaves acceptance intact");
        update["revision"]=playing["result"]["revision"];update["state"]="lobby";
        check(call(*service,"sessions.update",update,tokens[0])["error"]=="OK","back to lobby");
        const auto mixedResult=call(*service,"sessions.joinInvited",mixedJoin,tokens[1]);check(mixedResult["error"]=="OK","private then public mixed allocation");
        check(mixedResult["result"]["members"][1]["privateSlot"]==true&&mixedResult["result"]["members"][2]["privateSlot"]==false,"one private then one public slot");
        check(call(*service,"sessions.leave",{{"session",mixedId}},tokens[0])["error"]=="OK","mixed session cleanup");
        create["maxGamers"]=2;
        const auto limited=call(*service,"sessions.create",create,tokens[0]);check(limited["error"]=="OK","insufficient multi-local capacity fixture");
        const auto limitedId=limited["result"]["session"].get<std::string>();
        const auto limitedInvite=call(*service,"invites.send",{{"session",limitedId},{"gamertag","bob"}},tokens[0]);check(limitedInvite["error"]=="OK","small session invitation");
        const auto limitedToken=limitedInvite["result"]["invite"].get<std::string>();
        check(call(*service,"invites.accept",{{"invite",limitedToken}},tokens[1])["error"]=="OK","small session acceptance");
        check(call(*service,"sessions.joinInvited",{{"session",limitedId},{"invite",limitedToken},{"participants",Json::array({tokens[1],tokens[3]})}},tokens[1])["error"]=="SESSION_FULL","insufficient total capacity rejects all locals");
        check(call(*service,"invites.get",{{"invite",limitedToken}},tokens[1])["result"]["status"]=="accepted","full session does not consume invite");
        check(call(*service,"sessions.get",{{"session",limitedId}},tokens[0])["result"]["currentGamers"]==1,"full session failure does not insert any participant");
        check(call(*service,"sessions.leave",{{"session",limitedId}},tokens[0])["error"]=="OK","limited cleanup");
        create["kind"]="player";create["maxGamers"]=4;create["privateSlots"]=2;create["participants"]=Json::array({tokens[0],tokens[2]});
        const auto resourceFixture=call(*service,"sessions.create",create,tokens[0]);check(resourceFixture["error"]=="OK","bounded inbox fixture");
        const auto resourceSession=resourceFixture["result"]["session"].get<std::string>();
        {Store store(path.string());
         store.exec("UPDATE invitation_send_limits SET window_start=0");
         Statement fill(store.db(),"WITH RECURSIVE n(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM n WHERE x<?) INSERT INTO session_invitations(id,game_id,session_id,sender_id,recipient_id,created,expires) SELECT printf('%032x',x),'one',?,(SELECT id FROM users WHERE username='alice'),(SELECT id FROM users WHERE username='bob'),?,? FROM n");
         fill.bind(1,MaxIncomingInvites);fill.bind(2,resourceSession);fill.bind(3,now());fill.bind(4,now()+InviteLifetimeSeconds);(void)fill.row();}
        check(call(*service,"invites.send",{{"session",resourceSession},{"gamertag","bob"}},tokens[2])["error"]=="LIMIT_EXCEEDED","recipient inbox cap before mutation");
        const auto firstPage=call(*service,"invites.list",{{"start",0},{"limit",1}},tokens[1]);
        check(firstPage["result"]["invites"].size()==1&&firstPage["result"]["more"]==true,"bounded inbox first page and more flag");
        const auto lastPage=call(*service,"invites.list",{{"start",63},{"limit",1}},tokens[1]);
        check(lastPage["result"]["invites"].size()==1&&lastPage["result"]["more"]==false,"bounded inbox final page");
        check(call(*service,"invites.list",{{"start",64},{"limit",32}},tokens[1])["result"]["invites"].empty(),"past-end inbox page");
        {Store store(path.string());
         Statement fill(store.db(),"WITH RECURSIVE n(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM n WHERE x<?) INSERT INTO session_invitations(id,game_id,session_id,sender_id,recipient_id,status,created,expires) SELECT printf('%032x',100000+x),'one',?,(SELECT id FROM users WHERE username='alice'),(SELECT id FROM users WHERE username='eve'),'dismissed',?,? FROM n");
         fill.bind(1,MaxTitleInvites-MaxIncomingInvites);fill.bind(2,resourceSession);fill.bind(3,now());fill.bind(4,now()-1);(void)fill.row();}
        check(call(*service,"invites.send",{{"session",resourceSession},{"gamertag","dana"}},tokens[0])["error"]=="LIMIT_EXCEEDED","retained title resource cap before mutation");
        auto otherCreate=create;otherCreate["participants"]=Json::array({other});
        const auto otherSession=call(*service,"sessions.create",otherCreate,other,"two");check(otherSession["error"]=="OK","independent title directory fixture");
        check(call(*service,"invites.send",{{"session",otherSession["result"]["session"]},{"gamertag","dana"}},other,"two")["error"]=="OK","one title cannot exhaust another invitation budget");
        check(call(*service,"sessions.leave",{{"session",resourceSession}},tokens[0])["error"]=="OK","resource fixture cleanup");
        {Store store(path.string());Statement version(store.db(),"PRAGMA user_version");(void)version.row();check(version.number(0)==13,"schema migration version");}
        service.reset();clean();std::cout<<checks<<" invitation assertions passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
