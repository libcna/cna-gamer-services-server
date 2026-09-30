// SPDX-License-Identifier: MIT
#include "CnaService/Service.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace CnaService {
long long integerField(const Json& object,const char* key,long long minimum,long long maximum) {
    if(!object.contains(key)||!object[key].is_number_integer())throw Error("INVALID_ARGUMENT");
    const auto& value=object[key];
    if(value.is_number_unsigned()&&value.get<unsigned long long>()>static_cast<unsigned long long>(std::numeric_limits<long long>::max()))throw Error("INVALID_ARGUMENT");
    const auto number=value.get<long long>();if(number<minimum||number>maximum)throw Error("INVALID_ARGUMENT");return number;
}
namespace {
constexpr long long MaximumTicks=3155378975999999999LL;
const std::set<std::string> Types{"int32","int64","single","double","string","datetime","timespan","outcome","stream"};
// XNA's Stream columns (PropertyDictionary.GetValueStream): at most this many bytes, as lowercase hex.
constexpr std::size_t MaxStreamBytes=256;
void schemaGuard(const Json& schema) {
    if(!schema.is_object()||schema.size()>32)throw Error("INVALID_ARGUMENT");
    for(const auto& [name,type]:schema.items())if(!identifier(name)||name.size()>64||!type.is_string()||!Types.contains(type.get<std::string>()))throw Error("INVALID_ARGUMENT");
}
}
void validateColumns(const Json& columns,const Json& schema) {
    schemaGuard(schema);
    if(!columns.is_object()||columns.size()>32||columns.dump().size()>2048)throw Error("LIMIT_EXCEEDED");
    for(const auto& [name,field]:columns.items()) {
        if(!schema.contains(name)||!field.is_object()||field.size()!=2||!field.contains("type")||field["type"]!=schema[name]||!field.contains("value"))throw Error("INVALID_ARGUMENT");
        const auto type=field["type"].get<std::string>();
        if(type=="string"){(void)stringField(field,"value",256);continue;}
        if(type=="stream") {
            const auto hex=stringField(field,"value",MaxStreamBytes*2);
            if(hex.size()%2||hex.find_first_not_of("0123456789abcdef")!=std::string::npos)throw Error("INVALID_ARGUMENT");
            continue;
        }
        if(type=="single"||type=="double") {
            if(!field["value"].is_number())throw Error("INVALID_ARGUMENT");
            const auto value=field["value"].get<double>();if(!std::isfinite(value)||(type=="single"&&std::abs(value)>std::numeric_limits<float>::max()))throw Error("INVALID_ARGUMENT");
        }else {
            auto low=std::numeric_limits<long long>::min(),high=std::numeric_limits<long long>::max();
            if(type=="int32"){low=-2147483648LL;high=2147483647LL;}
            if(type=="datetime"){low=0;high=MaximumTicks;}
            if(type=="outcome"){low=0;high=3;}
            (void)integerField(field,"value",low,high);
        }
    }
}
void Store::leaderboard(const std::string& game,const Json& definition) {
    const auto key=stringField(definition,"key",64), aggregation=stringField(definition,"aggregation",16);
    const auto mode=integerField(definition,"mode",-2147483648LL,2147483647LL);
    if(!identifier(game)||!identifier(key)||(aggregation!="best"&&aggregation!="latest")||!definition.contains("ascending")||!definition["ascending"].is_boolean()||!definition.contains("arbitrated")||!definition["arbitrated"].is_boolean()||!definition.contains("columns"))throw Error("INVALID_ARGUMENT");
    schemaGuard(definition["columns"]);
    Statement cap(db(),"SELECT COUNT(*) FROM leaderboards WHERE game_id=?");cap.bind(1,game);(void)cap.row();if(cap.number(0)>=128)throw Error("LIMIT_EXCEEDED");
    Statement insert(db(),"INSERT INTO leaderboards(game_id,key,mode,ascending,aggregation,arbitrated,columns) VALUES(?,?,?,?,?,?,?)");
    insert.bind(1,game);insert.bind(2,key);insert.bind(3,mode);insert.bind(4,definition["ascending"].get<bool>()?1LL:0LL);
    insert.bind(5,aggregation);insert.bind(6,definition["arbitrated"].get<bool>()?1LL:0LL);insert.bind(7,definition["columns"].dump());(void)insert.row();
}
void Store::seedLeaderboard(const std::string& game,const Json& entry) {
    const auto key=stringField(entry,"key",64),tag=stringField(entry,"gamertag",32);
    const auto mode=integerField(entry,"mode",-2147483648LL,2147483647LL),rating=integerField(entry,"rating",std::numeric_limits<long long>::min(),std::numeric_limits<long long>::max());
    Statement definition(db(),"SELECT columns FROM leaderboards WHERE game_id=? AND key=? AND mode=?");definition.bind(1,game);definition.bind(2,key);definition.bind(3,mode);if(!definition.row())throw Error("NOT_FOUND");
    if(!entry.contains("columns"))throw Error("INVALID_ARGUMENT");
    validateColumns(entry["columns"],parse(definition.text(0)));
    Statement user(db(),"SELECT id FROM users WHERE gamertag=?");user.bind(1,tag);if(!user.row())throw Error("NOT_FOUND");
    Statement insert(db(),"INSERT INTO leaderboard_entries(game_id,key,mode,user_id,rating,columns,updated) VALUES(?,?,?,?,?,?,?) ON CONFLICT(game_id,key,mode,user_id) DO UPDATE SET rating=excluded.rating,columns=excluded.columns,updated=excluded.updated");
    insert.bind(1,game);insert.bind(2,key);insert.bind(3,mode);insert.bind(4,user.text(0));insert.bind(5,rating);insert.bind(6,entry["columns"].dump());insert.bind(7,now());(void)insert.row();
}
Json Service::readLeaderboard(const std::string& game,const Json& args) {
    const auto key=stringField(args,"key",64);if(!identifier(key))throw Error("INVALID_ARGUMENT");
    const auto mode=integerField(args,"mode",-2147483648LL,2147483647LL),size=integerField(args,"size",1,100);
    auto start=integerField(args,"start",0,2147483647LL);
    const auto pivot=args.contains("pivot")?stringField(args,"pivot",32):std::string{};
    Json gamers=Json::array();const bool restricted=args.contains("gamers");
    if(restricted) {
        gamers=args["gamers"];if(!gamers.is_array()||gamers.size()>100)throw Error("LIMIT_EXCEEDED");
        for(const auto& gamer:gamers)if(!gamer.is_string()||gamer.get_ref<const std::string&>().empty()||gamer.get_ref<const std::string&>().size()>32)throw Error("INVALID_ARGUMENT");
    }
    Statement definition(store_.db(),"SELECT ascending FROM leaderboards WHERE game_id=? AND key=? AND mode=?");definition.bind(1,game);definition.bind(2,key);definition.bind(3,mode);if(!definition.row())throw Error("NOT_FOUND");
    // Only the provisioned sort direction selects a trusted SQL fragment; every request value is bound.
    const auto direction=definition.number(0)?"ASC":"DESC";
    const std::string prefix=std::string("WITH ranked AS (SELECT u.id,u.gamertag,e.rating,e.columns,ROW_NUMBER() OVER(ORDER BY e.rating ")+direction+",e.user_id) rank FROM leaderboard_entries e JOIN users u ON u.id=e.user_id WHERE e.game_id=? AND e.key=? AND e.mode=?), selected AS (SELECT *,ROW_NUMBER() OVER(ORDER BY rank)-1 idx FROM ranked WHERE (?=0 OR gamertag COLLATE NOCASE IN (SELECT value FROM json_each(?)))) ";
    auto bind=[&](Statement& query){query.bind(1,game);query.bind(2,key);query.bind(3,mode);query.bind(4,restricted?1LL:0LL);query.bind(5,gamers.dump());};
    Statement count(store_.db(),(prefix+"SELECT COUNT(*),COALESCE(MAX(CASE WHEN gamertag=? COLLATE NOCASE THEN idx END),-1) FROM selected").c_str());bind(count);count.bind(6,pivot);(void)count.row();
    const auto total=count.number(0);if(total>2147483647LL)throw Error("LIMIT_EXCEEDED");
    if(!pivot.empty())start=std::max(0LL,count.number(1)-size/2);
    Statement page(store_.db(),(prefix+"SELECT id,gamertag,rating,columns,rank FROM selected ORDER BY idx LIMIT ? OFFSET ?").c_str());bind(page);page.bind(6,size);page.bind(7,start);
    Json rows=Json::array();while(page.row())rows.push_back(Json{{"userId",page.text(0)},{"gamertag",page.text(1)},{"rating",page.number(2)},{"columns",parse(page.text(3))},{"rank",page.number(4)}});
    return Json{{"start",start},{"total",total},{"entries",rows}};
}
Json Service::beginLeaderboardGame(const std::string& user,const std::string& game,const Json& args) {
    if(stringField(args,"kind",16)!="local")throw Error("NOT_SUPPORTED");
    if(!args.contains("participants")||!args["participants"].is_array()||args["participants"].empty()||args["participants"].size()>4)throw Error("INVALID_ARGUMENT");
    std::set<std::string> members;const auto timestamp=now();
    for(const auto& credential:args["participants"]) {
        if(!credential.is_string()||credential.get_ref<const std::string&>().size()!=64)throw Error("INVALID_ARGUMENT");
        Statement identity(store_.db(),"SELECT s.user_id,u.online_allowed FROM sessions s JOIN users u ON u.id=s.user_id WHERE s.hash=? AND s.game_id=? AND s.expires>?");
        identity.bind(1,sha256(credential.get_ref<const std::string&>()));identity.bind(2,game);identity.bind(3,timestamp);
        if(!identity.row()||!identity.number(1))throw Error("NOT_AUTHORIZED");
        if(!members.insert(identity.text(0)).second)throw Error("INVALID_ARGUMENT");
    }
    if(!members.contains(user))throw Error("NOT_AUTHORIZED");
    Statement prune(store_.db(),"DELETE FROM leaderboard_games WHERE expires<?");prune.bind(1,timestamp);(void)prune.row();
    Statement cap(store_.db(),"SELECT COUNT(*) FROM leaderboard_games WHERE owner_id=? AND committed='' AND expires>?");cap.bind(1,user);cap.bind(2,timestamp);(void)cap.row();if(cap.number(0)>=16)throw Error("LIMIT_EXCEEDED");
    const auto id=randomHex(16);Store::Transaction transaction(store_);
    Statement insert(store_.db(),"INSERT INTO leaderboard_games(id,game_id,owner_id,kind,created,expires) VALUES(?,?,?,'local',?,?)");
    insert.bind(1,id);insert.bind(2,game);insert.bind(3,user);insert.bind(4,timestamp);insert.bind(5,timestamp+86400);(void)insert.row();
    for(const auto& member:members){Statement join(store_.db(),"INSERT INTO leaderboard_game_members(gameplay_id,user_id) VALUES(?,?)");join.bind(1,id);join.bind(2,member);(void)join.row();}
    transaction.commit();
    return Json{{"gameplay",id}};
}
Json Service::abortLeaderboardGame(const std::string& user,const std::string& game,const Json& args) {
    const auto gameplay=stringField(args,"gameplay",32);
    if(gameplay.size()!=32||gameplay.find_first_not_of("0123456789abcdef")!=std::string::npos)throw Error("INVALID_ARGUMENT");
    Statement scope(store_.db(),"SELECT owner_id,committed FROM leaderboard_games WHERE id=? AND game_id=?");scope.bind(1,gameplay);scope.bind(2,game);
    if(!scope.row())return Json::object();
    if(scope.text(0)!=user)throw Error("NOT_AUTHORIZED");
    if(!scope.text(1).empty())throw Error("INVALID_STATE");
    Statement remove(store_.db(),"DELETE FROM leaderboard_games WHERE id=?");remove.bind(1,gameplay);(void)remove.row();return Json::object();
}
namespace {
struct BoardPolicy {bool ascending=false,latest=false,arbitrated=false;Json columns;};
BoardPolicy policyFor(sqlite3* db,const std::string& game,const std::string& key,long long mode) {
    Statement definition(db,"SELECT ascending,aggregation,arbitrated,columns FROM leaderboards WHERE game_id=? AND key=? AND mode=?");
    definition.bind(1,game);definition.bind(2,key);definition.bind(3,mode);if(!definition.row())throw Error("NOT_FOUND");
    return BoardPolicy{definition.number(0)!=0,definition.text(1)=="latest",definition.number(2)!=0,Json::parse(definition.text(3))};
}
void writeEntry(sqlite3* db,const std::string& game,const std::string& key,long long mode,const std::string& user,
    long long rating,const std::string& columns,const BoardPolicy& policy,long long timestamp) {
    Statement write(db,"INSERT INTO leaderboard_entries(game_id,key,mode,user_id,rating,columns,updated) VALUES(?,?,?,?,?,?,?) ON CONFLICT(game_id,key,mode,user_id) DO UPDATE SET rating=excluded.rating,columns=excluded.columns,updated=excluded.updated WHERE ?=1 OR (?=1 AND excluded.rating<leaderboard_entries.rating) OR (?=0 AND excluded.rating>leaderboard_entries.rating)");
    write.bind(1,game);write.bind(2,key);write.bind(3,mode);write.bind(4,user);write.bind(5,rating);write.bind(6,columns);write.bind(7,timestamp);
    write.bind(8,policy.latest?1LL:0LL);write.bind(9,policy.ascending?1LL:0LL);write.bind(10,policy.ascending?1LL:0LL);(void)write.row();
}
}
Json Service::commitLeaderboardGame(const std::string& user,const std::string& game,const Json& args) {
    const auto gameplay=stringField(args,"gameplay",32);
    if(!args.contains("entries")||!args["entries"].is_array()||args["entries"].size()>128)throw Error("LIMIT_EXCEEDED");
    Statement scope(store_.db(),"SELECT owner_id,committed FROM leaderboard_games WHERE id=? AND game_id=? AND expires>?");scope.bind(1,gameplay);scope.bind(2,game);scope.bind(3,now());
    if(!scope.row())throw Error("NOT_FOUND");
    if(scope.text(0)!=user)throw Error("NOT_AUTHORIZED");
    const auto digest=sha256(args["entries"].dump());
    if(!scope.text(1).empty()){if(scope.text(1)!=digest)throw Error("INVALID_STATE");return Json::object();}
    // Optional Ranked context: arbitrated rows become this machine's report for the round.
    std::string round,machine;std::set<std::string> roundUsers;
    if(args.contains("arbitration")) {
        const auto& context=args["arbitration"];
        if(!context.is_object()||context.size()!=2)throw Error("INVALID_ARGUMENT");
        const auto session=stringField(context,"session",32);
        if(session.size()!=32||session.find_first_not_of("0123456789abcdef")!=std::string::npos)throw Error("INVALID_ARGUMENT");
        const auto revision=integerField(context,"revision",1,2147483647);
        // The game a report is about is the latest round started at or before the revision the machine
        // knows. Its end does not bound it: after EndGame the session moves on (a machine leaves, the
        // host changes), and a machine reporting then still reports the game that just ended.
        Statement found(store_.db(),"SELECT id,members FROM arbitration_rounds WHERE game_id=? AND session_id=? AND start_revision<=? ORDER BY start_revision DESC LIMIT 1");
        found.bind(1,game);found.bind(2,session);found.bind(3,revision);
        if(!found.row())throw Error("NOT_FOUND");
        round=found.text(0);
        for(const auto& member:Json::parse(found.text(1))) {
            roundUsers.insert(member["userId"].get<std::string>());
            if(member["userId"]==user)machine=member["machine"].get<std::string>();
        }
        if(machine.empty())throw Error("NOT_AUTHORIZED");
    }
    std::set<std::tuple<std::string,std::string,long long>> unique;
    struct Pending {std::string user,key,columns;long long mode,rating;BoardPolicy policy;};std::vector<Pending> rows;
    Json reported=Json::array();
    for(const auto& entry:args["entries"]) {
        const auto target=stringField(entry,"userId",64),key=stringField(entry,"key",64);
        const auto mode=integerField(entry,"mode",-2147483648LL,2147483647LL),rating=integerField(entry,"rating",std::numeric_limits<long long>::min(),std::numeric_limits<long long>::max());
        if(!unique.emplace(target,key,mode).second)throw Error("INVALID_ARGUMENT");
        const auto policy=policyFor(store_.db(),game,key,mode);
        if(!entry.contains("columns"))throw Error("INVALID_ARGUMENT");
        validateColumns(entry["columns"],policy.columns);
        if(policy.arbitrated) {
            // Arbitrated statistics are reported by every machine for every gamer of the round.
            if(round.empty()||!roundUsers.contains(target))throw Error("NOT_AUTHORIZED");
            reported.push_back(Json{{"userId",target},{"key",key},{"mode",mode},{"rating",rating},{"columns",entry["columns"]}});
            continue;
        }
        // Nonarbitrated statistics are written only by their own machine's epoch.
        Statement member(store_.db(),"SELECT 1 FROM leaderboard_game_members WHERE gameplay_id=? AND user_id=?");member.bind(1,gameplay);member.bind(2,target);if(!member.row())throw Error("NOT_AUTHORIZED");
        rows.push_back({target,key,entry["columns"].dump(),mode,rating,policy});
    }
    {
        Store::Transaction transaction(store_);
        const auto timestamp=now();
        for(const auto& row:rows)writeEntry(store_.db(),game,row.key,row.mode,row.user,row.rating,row.columns,row.policy,timestamp);
        if(!round.empty()) {
            const auto report=reported.dump(),reportDigest=sha256(report);
            Statement existing(store_.db(),"SELECT digest FROM arbitration_submissions WHERE round_id=? AND machine_id=?");existing.bind(1,round);existing.bind(2,machine);
            if(existing.row()) {if(existing.text(0)!=reportDigest)throw Error("INVALID_STATE");}
            else {
                Statement insert(store_.db(),"INSERT INTO arbitration_submissions(round_id,machine_id,entries,digest) VALUES(?,?,?,?)");
                insert.bind(1,round);insert.bind(2,machine);insert.bind(3,report);insert.bind(4,reportDigest);(void)insert.row();
            }
            Statement touch(store_.db(),"UPDATE arbitration_rounds SET updated=? WHERE id=?");touch.bind(1,timestamp);touch.bind(2,round);(void)touch.row();
        }
        Statement close(store_.db(),"UPDATE leaderboard_games SET committed=? WHERE id=?");close.bind(1,digest);close.bind(2,gameplay);(void)close.row();
        transaction.commit();
    }
    resolveArbitration(game,round);
    return Json::object();
}
void Service::resolveArbitration(const std::string& game,const std::string& round) {
    const auto timestamp=now();
    // Complete rounds resolve immediately; ended rounds after 60 s and abandoned ones after a day.
    std::vector<std::string> due;
    Statement ready(store_.db(),"SELECT r.id FROM arbitration_rounds r WHERE r.game_id=? AND r.resolved=0 AND (r.id=? AND (SELECT COUNT(*) FROM arbitration_submissions s WHERE s.round_id=r.id)>=json_array_length(r.machines) OR (r.end_revision IS NOT NULL AND r.updated<?) OR r.created<?) LIMIT 8");
    ready.bind(1,game);ready.bind(2,round);ready.bind(3,timestamp-60);ready.bind(4,timestamp-86400);
    while(ready.row())due.push_back(ready.text(0));
    for(const auto& id:due) {
        try {
            Store::Transaction transaction(store_);
            std::vector<Json> reports;
            Statement submissions(store_.db(),"SELECT entries FROM arbitration_submissions WHERE round_id=?");submissions.bind(1,id);
            while(submissions.row())reports.push_back(Json::parse(submissions.text(0)));
            // A row is committed only when a strict majority of the machines that reported it agree
            // exactly. Finishing machines report every gamer, so a machine that left early and
            // reported only its own gamers still needs their agreement.
            std::map<std::tuple<std::string,std::string,long long>,std::map<std::string,std::pair<int,Json>>> votes;
            for(const auto& report:reports)for(const auto& row:report) {
                auto& tally=votes[{row["userId"].get<std::string>(),row["key"].get<std::string>(),row["mode"].get<long long>()}][row.dump()];
                ++tally.first;tally.second=row;
            }
            for(const auto& [identity,candidates]:votes) {
                int reporters=0;for(const auto& [value,tally]:candidates)reporters+=tally.first;
                for(const auto& [value,tally]:candidates) {
                if(tally.first*2<=reporters)continue;
                const auto& row=tally.second;const auto& [target,key,mode]=identity;
                writeEntry(store_.db(),game,key,mode,target,row["rating"].get<long long>(),row["columns"].dump(),policyFor(store_.db(),game,key,mode),timestamp);
                }
            }
            Statement done(store_.db(),"UPDATE arbitration_rounds SET resolved=1,updated=? WHERE id=?");done.bind(1,timestamp);done.bind(2,id);(void)done.row();
            transaction.commit();
        }catch(...){}
    }
    Statement prune(store_.db(),"DELETE FROM arbitration_rounds WHERE resolved=1 AND updated<?");prune.bind(1,timestamp-86400);(void)prune.row();
}

}
