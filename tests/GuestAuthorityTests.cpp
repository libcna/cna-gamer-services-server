// SPDX-License-Identifier: MIT
// Oracle: protocol/online-guests-v1.md, owner policy 2026-11-06; not native Xbox credentials.
#include "CnaService/Service.hpp"
#include "GuestTestAccess.hpp"
#include "LegacyGuestSchema.hpp"
#include <sqlite3.h>
#include <thread>
#include <barrier>
#include <functional>
#include <array>
#include <filesystem>
#include <iostream>
#include <memory>
using namespace CnaService;
namespace {
void check(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
struct Fixture {
    std::filesystem::path path;
    std::unique_ptr<Service> service;
    std::array<std::string,3> token,user,refresh;
    explicit Fixture(const std::string& name):path(std::filesystem::current_path()/("guest-"+name+".sqlite3")) {
        clean();
        {Store db(path.string());db.title("one","One");db.title("two","Two");
         for(const auto* n:{"alice","bob","charlie"})db.user(n,std::string(n)+"-password",n);
         db.leaderboard("one",{{"key","Score"},{"mode",0},{"ascending",false},{"aggregation","latest"},
             {"arbitrated",false},{"columns",Json::object()}});}
        service=std::make_unique<Service>(path.string());
        int i=0;for(const auto* n:{"alice","bob","charlie"}) {
            auto login=ok("auth.login",{{"username",n},{"password",std::string(n)+"-password"}},"");
            token[i]=login["token"];user[i]=login["identity"]["userId"];refresh[i]=login["refreshToken"];++i;
        }
    }
    ~Fixture(){service.reset();clean();}
    void clean(){for(const auto* s:{"","-wal","-shm"})std::filesystem::remove(path.string()+s);}
    Json call(const std::string& op,const Json& args,const std::string& credential,const std::string& title="one") {
        static int id=0;
        return parse(service->handle(Json{{"v",1},{"id","gap16-"+std::to_string(++id)},{"op",op},
            {"game",title},{"token",credential},{"args",args}}.dump(),"guest-unit"));
    }
    Json ok(const std::string& op,const Json& args,const std::string& credential) {
        auto r=call(op,args,credential);if(r["error"]!="OK")throw std::runtime_error(op+": "+r["error"].get<std::string>());
        return r["result"];
    }
    void error(const std::string& op,const Json& args,const std::string& credential,const char* expected) {
        check(call(op,args,credential)["error"]==expected,op.c_str());
    }
    Json account(int n){return Json{{"access",token[n]}};}
    Json guest(const std::string& id,int parent=0){return Json{{"guest",id},{"parentAccess",token[parent]}};}
    std::string open(int parent=0,int slot=1){return ok("guests.open",{{"slot",slot}},token[parent])["guestId"];}
    Json createArgs(const Json& group,const std::string& kind="player") {
        return Json{{"kind",kind},{"maxGamers",8},{"privateSlots",1},{"properties",wild()},
            {"allowJoinInProgress",false},{"allowHostMigration",true},{"participants",group}};
    }
    static Json wild(){return Json::array({nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr});}
    Json findArgs(const std::string& kind="player",int locals=1) {
        return Json{{"kind",kind},{"localCount",locals},{"properties",wild()},{"start",0},{"limit",32}};
    }
    Json get(const std::string& id,int actor=0){return ok("sessions.getGuests",{{"session",id}},token[actor]);}
    long long scalar(const std::string& sql){Store db(path.string());Statement q(db.db(),sql.c_str());check(q.row(),"scalar row");return q.number(0);}
};
void OpenIdentityHasNoAccountOrCredential() {
    Fixture f("identity");
    auto first=f.ok("guests.open",{{"slot",1}},f.token[0]);
    const std::string id=first["guestId"];
    check(id.size()==34&&id.starts_with("g:")&&id.substr(2).find_first_not_of("0123456789abcdef")==std::string::npos,"typed random identity");
    check(first==Json{{"guestId",id},{"parentId",f.user[0]},{"gamertag","alice (1)"},{"slot",1},{"allowOnlineSessions",true}},"exact public identity shape");
    check(f.open()==id,"same family slot is idempotent");
    check(f.open(0,2)!=id,"different slots have distinct IDs");
    check(f.scalar("SELECT COUNT(*) FROM users")==3,"no guest user account");
    check(f.scalar("SELECT COUNT(*) FROM sessions")==3,"no guest access credential");
    for(const auto& op:{"auth.ping","achievements.list","achievements.award","friends.list","presence.set"})
        f.error(op,Json::object(),id,"UNAUTHENTICATED");
    f.error("guests.open",{{"slot",0}},f.token[0],"INVALID_ARGUMENT");
    f.error("guests.open",{{"slot",4}},f.token[0],"INVALID_ARGUMENT");
    f.error("guests.open",{{"slot",true}},f.token[0],"INVALID_ARGUMENT");
    f.error("guests.open",{{"slot",1},{"parentId",f.user[1]}},f.token[0],"INVALID_ARGUMENT");
}
void BindingUsesFamilyTitleAndCompleteMachine() {
    Fixture f("binding");const auto guest=f.open();
    auto args=f.createArgs(Json::array({f.account(0),f.guest(guest)}));
    auto forged=args;forged["participants"][1]["guest"]="g:"+std::string(32,'0');
    f.error("sessions.createGuests",forged,f.token[0],"NOT_AUTHORIZED");
    auto foreign=args;foreign["participants"]=Json::array({f.account(1),f.guest(guest,1)});
    f.error("sessions.createGuests",foreign,f.token[1],"NOT_AUTHORIZED");
    auto missing=args;missing["participants"]=Json::array({f.account(1),f.guest(guest)});
    f.error("sessions.createGuests",missing,f.token[1],"NOT_AUTHORIZED");
    auto duplicate=args;duplicate["participants"].push_back(f.guest(guest));
    f.error("sessions.createGuests",duplicate,f.token[0],"INVALID_ARGUMENT");
    const auto second=f.ok("auth.login",{{"username","alice"},{"password","alice-password"}},"")["token"].get<std::string>();
    auto newFamily=args;newFamily["participants"][0]["access"]=second;newFamily["participants"][1]["parentAccess"]=second;
    f.error("sessions.createGuests",newFamily,second,"NOT_AUTHORIZED");
    const auto other=f.call("auth.login",{{"username","alice"},{"password","alice-password"}},"","two")["result"]["token"].get<std::string>();
    auto cross=args;cross["participants"][0]["access"]=other;cross["participants"][1]["parentAccess"]=other;
    check(f.call("sessions.createGuests",cross,other,"two")["error"]=="NOT_AUTHORIZED","title-bound identity");
    auto hosted=f.ok("sessions.createGuests",args,f.token[0]);
    check(hosted["currentGamers"]==2&&hosted["openPublicSlots"]==5&&hosted["openPrivateSlots"]==1,"failed attacks consumed no slots");
    check(hosted["members"][1]["userId"]==guest&&hosted["members"][1]["parentId"]==f.user[0]&&hosted["members"][1]["isGuest"]==true,"service-authoritative parent and flag");
    auto incomplete=Json{{"session",hosted["session"]},{"participants",Json::array({f.account(0)})}};
    f.error("sessions.relayTicketGuests",incomplete,f.token[0],"NOT_AUTHORIZED");
    incomplete["participants"]=args["participants"];
    check(f.ok("sessions.relayTicketGuests",incomplete,f.token[0])["ticket"].get<std::string>().size()==64,"full typed group gets relay authority");
}
void FourSlotsAndAdmissionAreAtomic(const std::string& kind) {
    Fixture f("slots-"+kind);const auto a=f.open(),b=f.open(1);
    auto created=f.ok("sessions.createGuests",f.createArgs(Json::array({f.account(0),f.guest(a)}),kind),f.token[0]);
    const std::string id=created["session"];
    auto joined=f.ok("sessions.joinGuests",{{"session",id},{"participants",Json::array({f.account(1),f.guest(b,1)})}},f.token[1]);
    check(joined["currentGamers"]==4&&joined["openPublicSlots"]==3&&joined["openPrivateSlots"]==1,"four participants consume four slots");
    const auto b2=f.open(1,2),b3=f.open(1,3);
    auto added=f.ok("sessions.addMembersGuests",{{"session",id},{"participants",Json::array({f.guest(b2,1),f.guest(b3,1)})}},f.token[1]);
    check(added["currentGamers"]==6&&added["openPublicSlots"]==1&&added["members"][5]["ordinal"]==5,"four local maximum and next ordinals");
    auto before=f.get(id);
    f.error("sessions.addMembersGuests",{{"session",id},{"participants",Json::array({f.account(2)})}},f.token[1],"LIMIT_EXCEEDED");
    check(f.get(id)==before,"overfull local group atomic");
    auto update=f.createArgs(Json::array(),kind);update.erase("participants");update.erase("kind");
    update["session"]=id;update["revision"]=before["revision"];update["state"]="playing";
    auto playing=f.ok("sessions.updateGuests",update,f.token[0]);
    f.error("sessions.joinGuests",{{"session",id},{"participants",Json::array({f.account(2)})}},f.token[2],"INVALID_STATE");
    check(f.get(id)==playing,"forbidden playing join atomic");
    if(kind=="player") {
        update["revision"]=playing["revision"];update["allowJoinInProgress"]=true;
        f.ok("sessions.updateGuests",update,f.token[0]);
        check(f.ok("sessions.joinGuests",{{"session",id},{"participants",Json::array({f.account(2)})}},f.token[2])["currentGamers"]==7,"player playing join permitted");
    }
}
void RankedPlayingAdmissionRejectsBothModes() {
    // Oracle: online-guests-v1 admission policy and immutable mode. Matching-mode
    // ordinary/invited joins must reach INVALID_STATE and preserve accepted consent.
    for(const bool guests:{false,true}) {
        Fixture f(guests?"ranked-playing-guests":"ranked-playing-legacy");
        const std::string suffix=guests?"Guests":"";
        auto host=Json::array({f.token[0]}),joiners=Json::array({f.token[1]});
        if(guests) {
            host=Json::array({f.account(0),f.guest(f.open())});
            joiners=Json::array({f.account(1),f.guest(f.open(1),1)});
        }
        auto args=f.createArgs(host,"ranked");
        const auto created=f.ok("sessions.create"+suffix,args,f.token[0]);
        check(created.value("guestProtocol",0)==(guests?1:0),"created session mode");
        const std::string id=created["session"];
        const auto invite=f.ok("invites.send",{{"session",id},{"gamertag","bob"}},f.token[0])["invite"];
        f.ok("invites.accept",{{"invite",invite}},f.token[1]);
        args.erase("kind");args.erase("participants");args["session"]=id;
        args["revision"]=created["revision"];args["state"]="playing";
        const auto playing=f.ok("sessions.update"+suffix,args,f.token[0]);
        const auto consent=f.ok("invites.get",{{"invite",invite}},f.token[1]);
        check(consent["status"]=="accepted","playing invitation consent retained");
        Json join{{"session",id},{"participants",joiners}};
        f.error("sessions.join"+suffix,join,f.token[1],"INVALID_STATE");
        check(f.ok("sessions.get"+suffix,{{"session",id}},f.token[0])==playing,"ordinary rejection preserves roster and revision");
        join["invite"]=invite;
        f.error("sessions.joinInvited"+suffix,join,f.token[1],"INVALID_STATE");
        check(f.ok("sessions.get"+suffix,{{"session",id}},f.token[0])==playing,"invited rejection preserves roster and revision");
        check(f.ok("invites.get",{{"invite",invite}},f.token[1])==consent,"rejected admission does not consume consent");
    }
}
void RotationRevocationCloseAndMigration() {
    Fixture f("lifetime");const auto a=f.open(),b=f.open(1);
    const auto id=f.ok("sessions.createGuests",f.createArgs(Json::array({f.account(0),f.guest(a)})),f.token[0])["session"].get<std::string>();
    f.ok("sessions.joinGuests",{{"session",id},{"participants",Json::array({f.account(1),f.guest(b,1)})}},f.token[1]);
    auto rotated=f.ok("auth.refresh",{{"refreshToken",f.refresh[0]}},"");f.token[0]=rotated["token"];
    check(f.open()==a,"rotation retains family identity");
    const auto ticket=f.ok("sessions.relayTicketGuests",{{"session",id},{"participants",Json::array({f.account(0),f.guest(a)})}},f.token[0])["ticket"].get<std::string>();
    f.ok("guests.close",{{"guest",a}},f.token[0]);
    auto closed=f.get(id);check(closed["currentGamers"]==3&&closed["hostId"]==f.user[0],"guest close retains parent and owner");
    f.error("guests.close",{{"guest",a}},f.token[1],"NOT_AUTHORIZED");
    bool refused=false;try{(void)f.service->redeemRelayTicket("one",ticket);}catch(const Error& e){refused=e.code()=="UNAUTHENTICATED";}
    check(refused,"closed guest invalidates old relay grant");
    const auto fresh=f.open();check(fresh!=a,"closed slot gets fresh identity");
    auto grown=f.ok("sessions.addMembersGuests",{{"session",id},{"participants",Json::array({f.guest(fresh)})}},f.token[0]);
    check(grown["currentGamers"]==4,"fresh identity can be added");
    f.ok("auth.logout",Json::object(),f.token[0]);
    auto migrated=f.get(id,1);
    check(migrated["currentGamers"]==2&&migrated["hostId"]==f.user[1]&&migrated["members"][1]["userId"]==b
        &&migrated["members"][1]["parentId"]==f.user[1]&&migrated["members"][1]["isGuest"]==true,"migration retains surviving guest's original parent");
    auto again=f.ok("auth.login",{{"username","alice"},{"password","alice-password"}},"")["token"].get<std::string>();
    f.error("guests.close",{{"guest",fresh}},again,"NOT_AUTHORIZED");
    f.service.reset();f.service=std::make_unique<Service>(f.path.string());
    check(f.get(id,1)["members"]==migrated["members"],"restart retains valid guest bindings");
    {Store db(f.path.string());db.exec("UPDATE refresh_families SET expires=0 WHERE user_id=(SELECT id FROM users WHERE username='bob')");}
    check(f.call("sessions.findGuests",f.findArgs(),f.token[2])["result"]["sessions"].empty(),"expired parent removes final host group without waiting");
}
void SecondaryParentRemovalAndPrivacyCannotBeBypassed() {
    Fixture f("secondary");const auto guest=f.open();
    const auto id=f.ok("sessions.createGuests",f.createArgs(Json::array({f.account(2),f.account(0),f.guest(guest)})),f.token[2])["session"].get<std::string>();
    check(f.ok("sessions.findGuests",f.findArgs(),f.token[1])["sessions"].size()==1,"secondary-parent privacy fixture");
    f.ok("privacy.block",{{"gamertag","alice"}},f.token[1]);
    check(f.ok("sessions.findGuests",f.findArgs(),f.token[1])["sessions"].empty(),"blocking secondary parent blocks its guests and machine");
    const auto ticket=f.ok("sessions.relayTicketGuests",{{"session",id},{"participants",Json::array({f.account(2),f.account(0),f.guest(guest)})}},f.token[2])["ticket"].get<std::string>();
    f.ok("auth.logout",Json::object(),f.token[0]);
    auto retained=f.get(id,2);
    check(retained["currentGamers"]==1&&retained["hostId"]==f.user[2]&&retained["members"][0]["userId"]==f.user[2],"secondary parent and its guest leave, owner remains");
    bool refused=false;try{(void)f.service->redeemRelayTicket("one",ticket);}catch(const Error& e){refused=e.code()=="UNAUTHENTICATED";}
    check(refused,"secondary-parent revocation closes old group authority");
}
void OnlineRevocationAndOpenRateAreEnforced() {
    Fixture f("privilege");const auto guest=f.open();
    for(int n=1;n<8;++n)check(f.open()==guest,"eight idempotent opens within window");
    f.error("guests.open",{{"slot",1}},f.token[0],"RATE_LIMITED");
    check(f.scalar("SELECT COUNT(*) FROM guest_identities")==1,"rate refusal allocates no identity");
    const auto id=f.ok("sessions.createGuests",f.createArgs(Json::array({f.account(2),f.account(0),f.guest(guest)})),f.token[2])["session"].get<std::string>();
    {Store db(f.path.string());db.exec("UPDATE users SET online_allowed=0 WHERE username='alice'");}
    auto retained=f.get(id,2);check(retained["currentGamers"]==1&&retained["hostId"]==f.user[2],"online privilege removal cascades secondary parent and guest");
    f.error("guests.open",{{"slot",2}},f.token[0],"NOT_AUTHORIZED");
}

void GuestCannotOwnProgressOrBorrowParentRating() {
    Fixture f("progress");const auto guest=f.open();
    const auto epoch=f.ok("leaderboards.game.begin",{{"kind","local"},{"participants",Json::array({f.token[0]})}},f.token[0])["gameplay"];
    const Json good{{"userId",f.user[0]},{"key","Score"},{"mode",0},{"rating",17},{"columns",Json::object()}};
    auto evil=good;evil["userId"]=guest;evil["rating"]=999;
    f.error("leaderboards.game.commit",{{"gameplay",epoch},{"entries",Json::array({good,evil})}},f.token[0],"NOT_AUTHORIZED");
    check(f.scalar("SELECT COUNT(*) FROM leaderboard_entries")==0,"failed guest target rolls back account row too");
    f.ok("leaderboards.game.commit",{{"gameplay",epoch},{"entries",Json::array({good})}},f.token[0]);
    check(f.scalar("SELECT COUNT(*) FROM leaderboard_entries")==1&&f.scalar("SELECT rating FROM leaderboard_entries")==17,"account-only progress unaffected");
    check(f.scalar("SELECT COUNT(*) FROM users")==3,"no hidden guest account created for progress");
}
void LegacyIsolationAndQosCapacity() {
    Fixture f("compat");const auto a=f.open();
    const auto id=f.ok("sessions.createGuests",f.createArgs(Json::array({f.account(0),f.guest(a)})),f.token[0])["session"].get<std::string>();
    check(f.ok("sessions.find",f.findArgs(),f.token[1])["sessions"].empty(),"legacy find excludes guest mode");
    check(f.ok("sessions.findQos",f.findArgs(),f.token[1])["sessions"].empty(),"legacy probe cannot discover guest mode");
    auto found=f.ok("sessions.findGuests",f.findArgs(),f.token[1]);
    check(found["sessions"].size()==1&&found["sessions"][0]["guestProtocol"]==1&&!found["sessions"][0].contains("members"),"new listing redacts roster and versions mode");
    f.error("sessions.join",{{"session",id},{"participants",Json::array({f.token[1]})}},f.token[1],"NOT_SUPPORTED");
    auto page=f.ok("sessions.findQosGuests",f.findArgs(),f.token[1]);
    check(page["sessions"].size()==1,"new guest-aware QoS listing");
    f.ok("sessions.qosTicket",{{"permit",page["sessions"][0]["permit"]}},f.token[1]);
    check(f.get(id)["currentGamers"]==2,"probe creates no guest or membership");
    auto full=f.findArgs("player",4); // max8-private1-current2 = five public, so four fit.
    check(f.ok("sessions.findGuests",full,f.token[1])["sessions"].size()==1,"guest counts in public arithmetic");
    auto legacyArgs=f.createArgs(Json::array({f.token[2]}));legacyArgs.erase("allowHostMigration");
    auto legacy=f.ok("sessions.create",legacyArgs,f.token[2]);
    check(!legacy.contains("guestProtocol")&&!legacy["members"][0].contains("isGuest"),"old account snapshot schema unchanged");
    auto join=Json{{"session",legacy["session"]},{"participants",Json::array({f.account(1),f.guest(f.open(1),1)})}};
    f.error("sessions.joinGuests",join,f.token[1],"NOT_SUPPORTED");
}
void BudgetBoundariesAndRollingWindow() {
    for(const auto scope:{0,1,2}) {
        Fixture f("budget-"+std::to_string(scope));
        GuestTestAccess::seed(*f.service,f.user[0],"one",scope==0?23:scope==1?1023:8191,scope==0,scope!=2);
        const auto id=f.open();check(!id.empty(),"last global/title/account slot admitted");
        const auto before=f.scalar("SELECT COUNT(*) FROM guest_identities");
        f.error("guests.open",{{"slot",2}},f.token[0],"LIMIT_EXCEEDED");
        check(f.scalar("SELECT COUNT(*) FROM guest_identities")==before,"over-cap admission allocates zero identities");
    }
    Fixture f("rolling");long long clock=1000;GuestTestAccess::clock(*f.service,[&]{return clock;});
    const auto id=f.open();for(int i=1;i<8;++i)check(f.open()==id,"budgeted idempotent open");
    clock=1059;f.error("guests.open",{{"slot",2}},f.token[0],"RATE_LIMITED");
    clock=1060;check(f.open()==id,"at exact sixty seconds oldest calls expire");
}
void ConcurrentLastSlotIsAtomic() {
    Fixture f("concurrent");GuestTestAccess::seed(*f.service,f.user[0],"one",23,true,true);
    std::barrier start(3);std::array<std::string,2> errors;
    auto run=[&](Service& service,int i) {
        start.arrive_and_wait();
        const auto response=parse(service.handle(Json{{"v",1},{"id","last-slot-"+std::to_string(i)},{"op","guests.open"},{"game","one"},{"token",f.token[0]},{"args",{{"slot",i+1}}}}.dump(),"guest-concurrent"));
        errors[i]=response["error"];
    };
    std::thread first([&]{run(*f.service,0);}),other([&]{run(*f.service,1);});start.arrive_and_wait();first.join();other.join();
    check((errors[0]=="OK"&&errors[1]=="LIMIT_EXCEEDED")||(errors[1]=="OK"&&errors[0]=="LIMIT_EXCEEDED"),"one last-slot winner, one bounded refusal");
    check(f.scalar("SELECT COUNT(*) FROM guest_identities")==24,"atomic account cap");
}
const char* faultPoint=nullptr;int faultCountdown=0;
void injectFailure(const char* point){if(faultPoint&&std::string(point)==faultPoint&&--faultCountdown<=0)throw Error("INTERNAL_ERROR");}
void TransactionRollbackAndReplayCannotResurrect() {
    Fixture f("rollback");Service::setFaultHookForTesting(injectFailure);
    struct Reset {~Reset(){Service::setFaultHookForTesting(nullptr);faultPoint=nullptr;faultCountdown=0;}} reset;
    faultPoint="guest-after-open";f.error("guests.open",{{"slot",1}},f.token[0],"INTERNAL_ERROR");
    check(f.scalar("SELECT COUNT(*) FROM guest_identities")==0,"identity insert rollback");faultPoint=nullptr;
    const Json request{{"v",1},{"id","guest-replay"},{"op","guests.open"},{"game","one"},{"token",f.token[0]},{"args",{{"slot",1}}}};
    auto first=parse(f.service->handle(request.dump(),"guest-replay"));check(first["error"]=="OK","first open");
    check(parse(f.service->handle(request.dump(),"guest-replay"))["result"]==first["result"],"live request replay stable");
    const auto guest=first["result"]["guestId"].get<std::string>();
    faultPoint="guest-after-member";faultCountdown=2; // fail after account AND guest insert
    f.error("sessions.createGuests",f.createArgs(Json::array({f.account(0),f.guest(guest)})),f.token[0],"INTERNAL_ERROR");
    check(f.scalar("SELECT COUNT(*) FROM directory_sessions")==0&&f.scalar("SELECT COUNT(*) FROM directory_members")==0,"admission rollback includes session, machine and all members");
    faultPoint=nullptr;
    const auto hosted=f.ok("sessions.createGuests",f.createArgs(Json::array({f.account(0),f.guest(guest)})),f.token[0]);
    faultPoint="guest-after-close";faultCountdown=0;f.error("guests.close",{{"guest",guest}},f.token[0],"INTERNAL_ERROR");
    check(f.get(hosted["session"])==hosted&&f.scalar("SELECT COUNT(*) FROM guest_identities")==1,"failed close rolls back identity, roster and revision");
    faultPoint=nullptr;f.ok("guests.close",{{"guest",guest}},f.token[0]);
    check(parse(f.service->handle(request.dump(),"guest-replay"))["error"]=="NOT_AUTHORIZED","closed identity cannot reappear from cached success");
    check(f.scalar("SELECT COUNT(*) FROM guest_identities")==0,"no replay resurrection");
    {Store db(f.path.string());Statement q(db.db(),"SELECT COUNT(*) FROM request_ids WHERE instr(COALESCE(result,''),?)>0");q.bind(1,f.token[0]);check(q.row()&&q.number(0)==0,"replay never stores parent bearer");}
}
void Schema27MigrationAndFailureAreAtomic() {
    Fixture f("migration");const auto before=f.ok("sessions.create",f.createArgs(Json::array({f.token[0],f.token[1]})),f.token[0]);
    f.service.reset();{Store db(f.path.string());removeGuestSchemaForMigration(db);db.exec("PRAGMA user_version=26; CREATE TABLE guest_open_times(conflict INTEGER)");}
    bool failed=false;try{Store upgrade(f.path.string());}catch(const Error&){failed=true;}check(failed,"schema27 conflict rejects migration");
    sqlite3* raw=nullptr;check(sqlite3_open(f.path.string().c_str(),&raw)==SQLITE_OK,"open migration evidence");
    {Statement v(raw,"PRAGMA user_version");check(v.row()&&v.number(0)==26,"failed migration version remains26");
     Statement c(raw,"SELECT COUNT(*) FROM pragma_table_info('directory_sessions') WHERE name='guest_protocol'");check(c.row()&&c.number(0)==0,"failed migration rolls back first ALTER");
     Statement m(raw,"SELECT COUNT(*) FROM directory_members");check(m.row()&&m.number(0)==2,"failed migration preserves original members");}
    check(sqlite3_exec(raw,"DROP TABLE guest_open_times",nullptr,nullptr,nullptr)==SQLITE_OK,"remove conflict");check(sqlite3_close(raw)==SQLITE_OK,"close migration evidence");
    f.service=std::make_unique<Service>(f.path.string());
    const auto after=f.ok("sessions.get",{{"session",before["session"]}},f.token[0]);check(after==before,"schema27 preserves every legacy snapshot field, ID and ordinal");
    check(f.scalar("PRAGMA user_version")==27,"migration reaches exact approved schema27");
    {Store db(f.path.string());Statement fk(db.db(),"PRAGMA foreign_key_check");check(!fk.row(),"all foreign keys valid after migration");}
}
void GuestInvitationsAndLegacyModeStaySeparate() {
    Fixture f("invited");const auto a=f.open(),b=f.open(1);
    auto args=f.createArgs(Json::array({f.account(0),f.guest(a)}));args["maxGamers"]=4;args["privateSlots"]=2;
    const auto created=f.ok("sessions.createGuests",args,f.token[0]);
    check(created["openPublicSlots"]==0&&created["openPrivateSlots"]==2,"ordinary slots reserved independently of guests");
    const auto invite=f.ok("invites.send",{{"session",created["session"]},{"gamertag","bob"}},f.token[0])["invite"];
    f.ok("invites.accept",{{"invite",invite}},f.token[1]);
    auto joined=f.ok("sessions.joinInvitedGuests",{{"session",created["session"]},{"invite",invite},{"participants",Json::array({f.account(1),f.guest(b,1)})}},f.token[1]);
    check(joined["currentGamers"]==4&&joined["openPrivateSlots"]==0&&joined["members"][2]["privateSlot"]==true&&joined["members"][3]["privateSlot"]==true,"accepted parent invite admits both into two private slots");
    f.error("invites.accept",{{"invite",invite}},b,"UNAUTHENTICATED");
}

void RankedGuestsAreNeitherSubjectsNorVotes() {
    Fixture f("ranked-progress");const auto a=f.open(),b=f.open(1);
    {Store db(f.path.string());db.leaderboard("one",{{"key","Round"},{"mode",0},{"ascending",false},{"aggregation","latest"},{"arbitrated",true},{"columns",Json::object()}});}
    const auto hosted=f.ok("sessions.createGuests",f.createArgs(Json::array({f.account(0),f.guest(a)}),"ranked"),f.token[0]);
    const auto id=hosted["session"].get<std::string>();
    auto joined=f.ok("sessions.joinGuests",{{"session",id},{"participants",Json::array({f.account(1),f.guest(b,1)})}},f.token[1]);
    auto update=f.createArgs(Json::array(),"ranked");update.erase("participants");update.erase("kind");update["session"]=id;update["revision"]=joined["revision"];update["state"]="playing";
    const auto playing=f.ok("sessions.updateGuests",update,f.token[0]);
    {Store db(f.path.string());Statement q(db.db(),"SELECT json_array_length(members),json_array_length(machines) FROM arbitration_rounds WHERE session_id=?");q.bind(1,id);check(q.row()&&q.number(0)==2&&q.number(1)==2,"two account subjects and two machine votes, despite four participants");}
    const Json context{{"session",id},{"revision",playing["revision"]}};
    auto epoch=[&](int actor){return f.ok("leaderboards.game.begin",{{"kind","local"},{"participants",Json::array({f.token[actor]})}},f.token[actor])["gameplay"];};
    const Json good{{"userId",f.user[0]},{"key","Round"},{"mode",0},{"rating",17},{"columns",Json::object()}};
    auto evil=good;evil["userId"]=a;evil["rating"]=999;
    const auto first=epoch(0);f.error("leaderboards.game.commit",{{"gameplay",first},{"entries",Json::array({good,evil})},{"arbitration",context}},f.token[0],"NOT_AUTHORIZED");
    check(f.scalar("SELECT COUNT(*) FROM arbitration_submissions")==0,"guest subject creates no vote or partial report");
    f.ok("leaderboards.game.commit",{{"gameplay",first},{"entries",Json::array({good})},{"arbitration",context}},f.token[0]);
    check(f.scalar("SELECT COUNT(*) FROM leaderboard_entries")==0,"one machine cannot finish two-machine arbitration");
    f.ok("leaderboards.game.commit",{{"gameplay",epoch(1)},{"entries",Json::array({good})},{"arbitration",context}},f.token[1]);
    check(f.scalar("SELECT COUNT(*) FROM arbitration_submissions")==2&&f.scalar("SELECT COUNT(*) FROM leaderboard_entries")==1&&f.scalar("SELECT rating FROM leaderboard_entries")==17,"two agreeing machines produce one account row17");
}
void GuestQosHostAuthorityTracksExactGroupAndMigration() {
    Fixture f("qos");const auto a=f.open(),b=f.open(1);
    const auto id=f.ok("sessions.createGuests",f.createArgs(Json::array({f.account(0),f.guest(a)})),f.token[0])["session"].get<std::string>();
    f.ok("sessions.joinGuests",{{"session",id},{"participants",Json::array({f.account(1),f.guest(b,1)})}},f.token[1]);
    auto group=[&](int actor,const std::string& guest){return Json{{"session",id},{"participants",Json::array({f.account(actor),f.guest(guest,actor)})}};};
    const auto hostGroup=group(0,a);auto relay=f.service->redeemRelayTicket("one",f.ok("sessions.relayTicketGuests",hostGroup,f.token[0])["ticket"]);
    auto host=f.service->redeemQosTicket("one",f.ok("sessions.qosHostTicketGuests",hostGroup,f.token[0])["ticket"]);
    check(f.service->validateQosGrant(host),"complete guest host group has valid responder-only authority");
    const auto page=f.ok("sessions.findQosGuests",f.findArgs(),f.token[2]);const auto permit=page["sessions"][0]["permit"];
    f.ok("auth.logout",Json::object(),f.token[0]);
    check(!f.service->validateRelayGrant(relay)&&!f.service->validateQosGrant(host),"parent sign-out revokes relay and old host responder");
    f.error("sessions.qosTicket",{{"permit",permit}},f.token[2],"NOT_AUTHORIZED");
    const auto survivor=group(1,b);auto nextRelay=f.service->redeemRelayTicket("one",f.ok("sessions.relayTicketGuests",survivor,f.token[1])["ticket"]);
    const auto next=f.service->redeemQosTicket("one",f.ok("sessions.qosHostTicketGuests",survivor,f.token[1])["ticket"]);
    check(f.service->validateQosGrant(next),"new host obtains fresh group-bound responder after handover");
    f.ok("guests.close",{{"guest",b}},f.token[1]);check(!f.service->validateRelayGrant(nextRelay)&&!f.service->validateQosGrant(next),"individual close invalidates both old complete-group grants");
    const Json accountOnly{{"session",id},{"participants",Json::array({f.account(1)})}};
    (void)f.service->redeemRelayTicket("one",f.ok("sessions.relayTicketGuests",accountOnly,f.token[1])["ticket"]);
    const auto renewed=f.scalar("SELECT expires FROM directory_machines WHERE owner_id='"+f.user[1]+"'");
    f.service->releaseRelayGrant(nextRelay);
    check(f.scalar("SELECT expires FROM directory_machines WHERE owner_id='"+f.user[1]+"'")==renewed,"late release of old group cannot shorten retained parent's renewed lease");
    check(f.ok("sessions.qosHostTicketGuests",accountOnly,f.token[1]).contains("ticket"),"revoked responder releases session cap for retained parent");
}

void BackupAndRestartPreserveLiveGuestAuthority() {
    // Owner schema/lifetime policy. Real SQLite online backup, not a copied expected payload.
    Fixture f("backup");const auto guest=f.open();
    const auto before=f.ok("sessions.createGuests",f.createArgs(Json::array({f.account(0),f.guest(guest)})),f.token[0]);
    const auto copy=f.path.string()+".backup";
    struct Cleanup {Fixture& f;std::string copy;~Cleanup(){f.service.reset();for(const auto* suffix:{"","-wal","-shm"})std::filesystem::remove(copy+suffix);}} cleanup{f,copy};
    sqlite3 *source=nullptr,*destination=nullptr;
    check(sqlite3_open_v2(f.path.string().c_str(),&source,SQLITE_OPEN_READONLY,nullptr)==SQLITE_OK,"backup source");
    check(sqlite3_open(copy.c_str(),&destination)==SQLITE_OK,"backup destination");
    auto* backup=sqlite3_backup_init(destination,"main",source,"main");check(backup!=nullptr,"backup initialization");
    const int copied=sqlite3_backup_step(backup,-1),finished=sqlite3_backup_finish(backup);
    check(sqlite3_close(source)==SQLITE_OK&&sqlite3_close(destination)==SQLITE_OK,"close backup connections");
    check(copied==SQLITE_DONE&&finished==SQLITE_OK,"online backup completed");
    f.service.reset();f.service=std::make_unique<Service>(copy);
    check(f.get(before["session"])==before,"restore preserves complete typed snapshot and valid parent family");
    check(f.open()==guest,"restored family slot retains public identity");
    f.ok("guests.close",{{"guest",guest}},f.token[0]);
    check(f.get(before["session"])["currentGamers"]==1,"restored close still retains parent machine");
    {Store db(copy);Statement integrity(db.db(),"PRAGMA integrity_check");check(integrity.row()&&integrity.text(0)=="ok","restored integrity");Statement fk(db.db(),"PRAGMA foreign_key_check");check(!fk.row(),"restored foreign keys");}
}

}
int main(int argc,char** argv) {
    const std::vector<std::pair<const char*,std::function<void()>>> cases{
        {"identity",OpenIdentityHasNoAccountOrCredential},{"binding",BindingUsesFamilyTitleAndCompleteMachine},
        {"player",[]{FourSlotsAndAdmissionAreAtomic("player");}},{"ranked",[]{FourSlotsAndAdmissionAreAtomic("ranked");}},
        {"ranked-playing",RankedPlayingAdmissionRejectsBothModes},{"lifetime",RotationRevocationCloseAndMigration},{"secondary",SecondaryParentRemovalAndPrivacyCannotBeBypassed},
        {"privilege",OnlineRevocationAndOpenRateAreEnforced},{"progress",GuestCannotOwnProgressOrBorrowParentRating},
        {"compatibility",LegacyIsolationAndQosCapacity},{"budgets",BudgetBoundariesAndRollingWindow},
        {"concurrency",ConcurrentLastSlotIsAtomic},{"rollback",TransactionRollbackAndReplayCannotResurrect},
        {"migration",Schema27MigrationAndFailureAreAtomic},{"invited",GuestInvitationsAndLegacyModeStaySeparate},{"ranked-progress",RankedGuestsAreNeitherSubjectsNorVotes},{"qos",GuestQosHostAuthorityTracksExactGroupAndMigration},{"backup",BackupAndRestartPreserveLiveGuestAuthority}};
    try {int ran=0;for(const auto& [name,run]:cases)if(argc==1||(argc==2&&std::string(argv[1])==name)){run();++ran;std::cout<<name<<" passed\n";}
        check(ran>0,"unknown case");return 0;
    }catch(const std::exception& e){std::cerr<<"guest authority: "<<e.what()<<'\n';return 1;}
}
