// SPDX-License-Identifier: MIT
#include "CnaService/Service.hpp"
#include "LegacyGuestSchema.hpp"
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace CnaService;
namespace {
// Oracle: owner-approved CNA GAP-18 (2026-11-06), 7*24*60*60=604800 seconds.
constexpr long long T=2000000000LL;
void check(bool condition,const char* why) {if(!condition)throw std::runtime_error(why);}
struct Fixture {
    std::string path=(std::filesystem::current_path()/"recent-leaderboards.sqlite3").string();
    long long clock=T;
    std::unique_ptr<Service> service;
    std::string token,user;
    int sequence=0;
    void remove() {for(const auto* suffix:{"","-wal","-shm"})std::filesystem::remove(path+suffix);}
    Fixture() {
        remove();
        {
            Store db(path,[this]{return clock;});db.title("one","One");db.title("two","Two");
            user=db.user("alice","alice-password","Alice");
            (void)db.user("bob","bob-password","Bob");(void)db.user("carol","carol-password","Carol");
            for(const auto* game:{"one","two"})for(const auto* key:{"BestScoreRecent","BestTimeRecent","BestScoreLifeTime","CustomRecent"})
                db.leaderboard(game,{{"key",key},{"mode",0},{"ascending",std::string(key)=="BestTimeRecent"},
                    {"aggregation","best"},{"arbitrated",false},{"columns",{{"marker","int32"}}}});
            db.leaderboard("one",{{"key","BestScoreRecent"},{"mode",7},{"ascending",false},
                {"aggregation","best"},{"arbitrated",false},{"columns",{{"marker","int32"}}}});
        }
        restart();
    }
    ~Fixture(){service.reset();remove();}
    Json call(const std::string& op,const Json& args,const std::string& game="one",const std::string& id={}) {
        Json request{{"v",1},{"id",id.empty()?"recent-"+std::to_string(++sequence):id},{"game",game},{"op",op},{"args",args}};
        if(!token.empty())request["token"]=token;
        return parse(service->handle(request.dump(),"recent-test"));
    }
    Json ok(const std::string& op,const Json& args,const std::string& game="one") {
        auto reply=call(op,args,game);check(reply["error"]=="OK","service operation refused");return reply["result"];
    }
    void restart() {
        service.reset();service=std::make_unique<Service>(path,[this]{return clock;});
        token.clear();token=ok("auth.login",{{"username","alice"},{"password","alice-password"}})["token"].get<std::string>();
    }
    void seed(const std::string& key,const std::string& tag,long long rating,const std::string& game="one",int mode=0) {
        Store db(path,[this]{return clock;});
        db.seedLeaderboard(game,{{"key",key},{"mode",mode},{"gamertag",tag},{"rating",rating},
            {"columns",{{"marker",{{"type","int32"},{"value",1}}}}}});
    }
    Json read(const std::string& key,Json extra=Json::object(),int mode=0,const std::string& game="one") {
        Json args{{"key",key},{"mode",mode},{"start",0},{"size",10}};
        args.update(extra);return ok("leaderboards.read",args,game);
    }
    Json row(const std::string& key,long long rating,int marker=1) {
        return {{"userId",user},{"key",key},{"mode",0},{"rating",rating},
            {"columns",{{"marker",{{"type","int32"},{"value",marker}}}}}};
    }
    Json begin() {return ok("leaderboards.game.begin",{{"kind","local"},{"participants",Json::array({token})}})["gameplay"];}
    Json submit(const Json& rows) {
        Json args{{"gameplay",begin()},{"entries",rows}};ok("leaderboards.game.commit",args);return args;
    }
    long long stamp(const std::string& key) {
        Store db(path,[this]{return clock;});Statement query(db.db(),"SELECT updated FROM leaderboard_entries WHERE game_id='one' AND key=? AND mode=0 AND user_id=?");
        query.bind(1,key);query.bind(2,user);check(query.row(),"expected stored row");return query.number(0);
    }
    long long count(const std::string& key) {
        Store db(path);Statement query(db.db(),"SELECT COUNT(*) FROM leaderboard_entries WHERE game_id='one' AND key=?");
        query.bind(1,key);check(query.row(),"count query");return query.number(0);
    }
};

void ExactExpiryAndList() {
    Fixture f;
    for(const auto* key:{"BestScoreRecent","BestTimeRecent","BestScoreLifeTime","CustomRecent"})f.seed(key,"Alice",100);
    f.seed("BestScoreRecent","Alice",77,"one",7);
    f.seed("BestScoreRecent","Alice",200,"two");
    f.clock=T+604799;
    check(f.read("BestScoreRecent")["total"]==1,"visible one second before expiry");
    f.clock=T+604800;
    // Listing itself must filter; it is checked BEFORE any read can prune the rows.
    const auto boards=f.ok("leaderboards.list",Json::object())["boards"];
    for(const auto& b:boards)if(b["key"]=="BestScoreRecent"||b["key"]=="BestTimeRecent")
        check(b["entries"]==0,"listing excludes exactly-expired rows");
    check(f.read("BestScoreRecent")["total"]==0,"score exact expiry");
    check(f.read("BestTimeRecent")["entries"].empty(),"time exact expiry");
    check(f.read("BestScoreRecent",Json::object(),7)["total"]==0,"expiry applies to other mode");
    check(f.read("BestScoreLifeTime")["total"]==1,"lifetime retained");
    check(f.read("CustomRecent")["total"]==1,"custom key retained");
    check(f.count("BestScoreRecent")==0,"expired rows physically pruned by read");
    // Another title's row is not pruned by a title-one request.
    {Store db(f.path);Statement q(db.db(),"SELECT COUNT(*) FROM leaderboard_entries WHERE game_id='two'");
     check(q.row()&&q.number(0)==1,"pruning remains title isolated");}
}

void RefreshRewriteAndRetry() {
    Fixture f;
    for(const auto* key:{"BestScoreRecent","BestTimeRecent"}) {
        f.clock=T;f.submit(Json::array({f.row(key,100)}));
        f.clock=T+10;f.submit(Json::array({f.row(key,std::string(key)=="BestTimeRecent"?110:90,2)}));
        check(f.stamp(key)==T,"worse score did not renew");
        check(f.read(key)["entries"][0]["rating"]==100,"worse score did not replace");
        const long long better=std::string(key)=="BestTimeRecent"?90:110;
        f.clock=T+20;f.submit(Json::array({f.row(key,better,3)}));
        check(f.stamp(key)==T+20,"better score refreshes");
        f.clock=T+30;const auto commit=f.submit(Json::array({f.row(key,better,4)}));
        check(f.stamp(key)==T+30,"equal explicit rewrite refreshes");
        f.clock=T+40;f.ok("leaderboards.game.commit",commit);
        check(f.stamp(key)==T+30,"closed epoch retry does not refresh");
        f.restart();f.ok("leaderboards.game.commit",commit);
        check(f.stamp(key)==T+30,"retry after restart does not refresh");
        const auto rows=f.read(key)["entries"];
        check(rows.size()==1&&rows[0]["rating"]==better,"retained best value after restart");
        check(rows[0]["columns"]["marker"]["value"]==4,"equal rewrite replaced columns");
        f.clock=T+604829;check(f.read(key)["total"]==1,"refreshed boundary minus one");
        f.clock=T+604830;check(f.read(key)["total"]==0,"refreshed exact boundary");
    }
}

void ExpiredBestIsReplaceableAndInvalidBatchIsAtomic() {
    Fixture f;f.submit(Json::array({f.row("BestScoreRecent",100),f.row("BestTimeRecent",100)}));
    f.clock=T+604800;
    // No read/prune occurs before either replacement.
    f.submit(Json::array({f.row("BestScoreRecent",80),f.row("BestTimeRecent",120)}));
    check(f.read("BestScoreRecent")["entries"][0]["rating"]==80,"lower score replaces expired best");
    check(f.read("BestTimeRecent")["entries"][0]["rating"]==120,"slower time replaces expired best");
    check(f.stamp("BestScoreRecent")==T+604800,"replacement gets new time");
    f.clock=T+604810;
    auto bad=f.row("missing",5);
    auto reply=f.call("leaderboards.game.commit",{{"gameplay",f.begin()},
        {"entries",Json::array({f.row("BestScoreRecent",90),bad})}});
    check(reply["error"]=="NOT_FOUND","invalid second row refused");
    check(f.read("BestScoreRecent")["entries"][0]["rating"]==80,"failed batch did not write first row");
    check(f.stamp("BestScoreRecent")==T+604800,"failed batch did not refresh timestamp");
}

void FilterBeforeRanksPageAndFriends() {
    Fixture f;f.seed("BestScoreRecent","Alice",100);
    f.clock=T+1;f.seed("BestScoreRecent","Bob",90);f.seed("BestScoreRecent","Carol",80);
    f.clock=T+604800;
    auto page=f.read("BestScoreRecent",{{"size",1}});
    check(page["total"]==2&&page["start"]==0&&page["entries"].size()==1,"filtered page size and total");
    check(page["entries"][0]["gamertag"]=="Bob"&&page["entries"][0]["rank"]==1,"rank after removing expired leader");
    page=f.read("BestScoreRecent",{{"size",1},{"start",1}});
    check(page["entries"].size()==1&&page["entries"][0]["gamertag"]=="Carol"&&page["entries"][0]["rank"]==2,"second page");
    auto friends=f.read("BestScoreRecent",{{"gamers",Json::array({"Alice","Carol"})}});
    check(friends["total"]==1&&friends["entries"][0]["rank"]==2,"friend subset retains live global rank");
    check(f.read("BestScoreRecent",{{"gamers",Json::array()}})["total"]==0,"empty gamer subset");
    auto centered=f.read("BestScoreRecent",{{"pivot","Carol"},{"size",1}});
    check(centered["start"]==1&&centered["entries"][0]["gamertag"]=="Carol","pivot after expiry filtering");
    check(f.read("BestScoreRecent",{{"pivot","Alice"},{"size",1}})["start"]==0,"expired pivot falls back to top");
    f.clock=T+604801;
    check(f.read("BestScoreRecent",{{"start",1},{"size",1}})["entries"].empty(),"live page request sees later expiry");
}

void ExistingTimestampMigrationAndRestart() {
    Fixture f;f.seed("BestScoreRecent","Alice",100);f.seed("BestTimeRecent","Alice",100);
    f.service.reset();
    {
        // Real pre-policy schema-25 shape: every entry already has NOT NULL updated.
        Store db(f.path);removeGuestSchemaForMigration(db);db.exec("DROP INDEX IF EXISTS leaderboard_recent_expiry; PRAGMA user_version=25;");
        db.exec("UPDATE leaderboards SET aggregation='latest',ascending=0 WHERE key='BestTimeRecent'");
    }
    f.clock=T+604800;f.restart();
    check(f.stamp("BestScoreRecent")==T,"migration preserves known timestamp, never grants restart grace");
    auto definition=f.ok("leaderboards.definition",{{"key","BestTimeRecent"},{"mode",0}});
    check(definition["aggregation"]=="best"&&definition["ascending"]==true,"v26 canonical best-time policy");
    check(f.read("BestScoreRecent")["total"]==0,"old row expires across migration");
    f.clock=T+604900;f.restart();
    check(f.read("BestScoreRecent")["total"]==0,"second restart does not renew row");
    Store db(f.path);Statement version(db.db(),"PRAGMA user_version");
    check(version.row()&&version.number(0)==27,"v26 migration followed by current guest schema");
}

void MigrationFailureRollsBackPolicyAndVersion() {
    Fixture f;f.service.reset();
    {Store db(f.path);removeGuestSchemaForMigration(db);db.exec("DROP INDEX IF EXISTS leaderboard_recent_expiry; PRAGMA user_version=25; "
        "UPDATE leaderboards SET aggregation='latest',ascending=0 WHERE key='BestTimeRecent'; "
        "CREATE INDEX leaderboard_recent_expiry ON users(username)");}
    bool failed=false;try{Store migrate(f.path);}catch(const Error&){failed=true;}
    check(failed,"conflicting index rejects migration");
    sqlite3* database=nullptr;
    check(sqlite3_open(f.path.c_str(),&database)==SQLITE_OK,"inspect failed migration");
    {
        Statement version(database,"PRAGMA user_version");check(version.row()&&version.number(0)==25,"failed migration version rolled back");
        Statement definition(database,"SELECT aggregation,ascending FROM leaderboards WHERE game_id='one' AND key='BestTimeRecent'");
        check(definition.row()&&definition.text(0)=="latest"&&definition.number(1)==0,"failed migration policy update rolled back");
    }
    check(sqlite3_exec(database,"DROP INDEX leaderboard_recent_expiry",nullptr,nullptr,nullptr)==SQLITE_OK,"remove fixture conflict");
    check(sqlite3_close(database)==SQLITE_OK,"close raw inspection");
    f.restart();check(f.ok("leaderboards.definition",{{"key","BestTimeRecent"},{"mode",0}})["ascending"]==true,"retry migration succeeds");
}

void ArbitrationUsesResolutionTimeAndDoesNotRenewOnReplay() {
    Fixture f;
    {Store db(f.path);db.exec("UPDATE leaderboards SET arbitrated=1 WHERE key IN ('BestScoreRecent','BestTimeRecent')");}
    const auto alice=f.token;
    const auto bob=f.ok("auth.login",{{"username","bob"},{"password","bob-password"}})["token"].get<std::string>();
    const auto properties=Json::array({nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr});
    auto session=f.ok("sessions.create",{{"kind","ranked"},{"maxGamers",2},{"privateSlots",0},
        {"properties",properties},{"allowJoinInProgress",false},{"participants",Json::array({alice})}});
    f.token=bob;f.ok("sessions.join",{{"session",session["session"]},{"participants",Json::array({bob})}});
    f.token=alice;session=f.ok("sessions.get",{{"session",session["session"]}});
    session=f.ok("sessions.update",{{"session",session["session"]},{"revision",session["revision"]},
        {"maxGamers",2},{"privateSlots",0},{"properties",properties},{"allowJoinInProgress",false},{"state","playing"}});
    const Json context{{"session",session["session"]},{"revision",session["revision"]}};
    const auto rows=Json::array({f.row("BestScoreRecent",100),f.row("BestTimeRecent",90)});
    auto report=[&] {Json args{{"gameplay",f.begin()},{"entries",rows},{"arbitration",context}};
        f.ok("leaderboards.game.commit",args);return args;};
    (void)report();check(f.count("BestScoreRecent")==0,"first report is not yet a stored Recent row");
    f.clock=T+10;f.token=bob;const auto final=report();
    check(f.stamp("BestScoreRecent")==T+10&&f.stamp("BestTimeRecent")==T+10,"resolution timestamps all winning rows once");
    f.clock=T+20;f.ok("leaderboards.game.commit",final);
    check(f.stamp("BestScoreRecent")==T+10,"arbitration replay cannot renew");
    f.clock=T+604809;check(f.read("BestScoreRecent")["total"]==1,"resolved row before expiry");
    f.clock=T+604810;check(f.read("BestScoreRecent")["total"]==0,"resolved row exact expiry");
}

void ClockCorrectionsAndCatalogValidation() {
    Fixture f;f.seed("BestScoreRecent","Alice",100);
    f.clock=T-100;check(f.read("BestScoreRecent")["total"]==1,"rollback leaves future-stamped row visible");
    check(f.stamp("BestScoreRecent")==T,"clock rollback does not retimestamp");
    f.clock=T+604800;check(f.read("BestScoreRecent")["total"]==0,"forward correction expires");
    f.clock=T;check(f.read("BestScoreRecent")["total"]==0,"pruning irreversible after rollback");
    Store db(f.path);
    for(const auto& definition:{
        Json{{"key","BestTimeRecent"},{"mode",9},{"ascending",false},{"aggregation","best"},{"arbitrated",false},{"columns",Json::object()}},
        Json{{"key","BestScoreRecent"},{"mode",9},{"ascending",false},{"aggregation","latest"},{"arbitrated",false},{"columns",Json::object()}}}) {
        bool refused=false;try{db.leaderboard("one",definition);}catch(const Error& e){refused=e.code()=="INVALID_ARGUMENT";}
        check(refused,"canonical Recent board policy cannot be misprovisioned");
    }
}
}
int main(int argc,char** argv) {
    const std::pair<const char*,void(*)()> cases[]{
        {"ExactExpiryAndList",ExactExpiryAndList},
        {"RefreshRewriteAndRetry",RefreshRewriteAndRetry},
        {"ExpiredBestIsReplaceableAndInvalidBatchIsAtomic",ExpiredBestIsReplaceableAndInvalidBatchIsAtomic},
        {"FilterBeforeRanksPageAndFriends",FilterBeforeRanksPageAndFriends},
        {"ExistingTimestampMigrationAndRestart",ExistingTimestampMigrationAndRestart},
        {"MigrationFailureRollsBackPolicyAndVersion",MigrationFailureRollsBackPolicyAndVersion},
        {"ArbitrationUsesResolutionTimeAndDoesNotRenewOnReplay",ArbitrationUsesResolutionTimeAndDoesNotRenewOnReplay},
        {"ClockCorrectionsAndCatalogValidation",ClockCorrectionsAndCatalogValidation}
    };
    int failures=0,ran=0;
    for(const auto& [name,run]:cases) {
        if(argc>1 && std::string(argv[1])!=name)continue;
        ++ran;
        try {run();std::cout<<"PASS "<<name<<'\n';}
        catch(const std::exception& error){++failures;std::cerr<<"FAIL "<<name<<": "<<error.what()<<'\n';}
    }
    return ran==0?2:failures?1:0;
}
