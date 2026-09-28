// SPDX-License-Identifier: MS-PL
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
const std::set<std::string> Types{"int32","int64","single","double","string","datetime","timespan","outcome"};
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
    const auto id=randomHex(16);store_.exec("BEGIN IMMEDIATE");
    try {
        Statement insert(store_.db(),"INSERT INTO leaderboard_games(id,game_id,owner_id,kind,created,expires) VALUES(?,?,?,'local',?,?)");
        insert.bind(1,id);insert.bind(2,game);insert.bind(3,user);insert.bind(4,timestamp);insert.bind(5,timestamp+86400);(void)insert.row();
        for(const auto& member:members){Statement join(store_.db(),"INSERT INTO leaderboard_game_members(gameplay_id,user_id) VALUES(?,?)");join.bind(1,id);join.bind(2,member);(void)join.row();}
        store_.exec("COMMIT");
    }catch(...){store_.exec("ROLLBACK");throw;}
    return Json{{"gameplay",id}};
}
Json Service::commitLeaderboardGame(const std::string& user,const std::string& game,const Json& args) {
    const auto gameplay=stringField(args,"gameplay",32);
    if(!args.contains("entries")||!args["entries"].is_array()||args["entries"].size()>128)throw Error("LIMIT_EXCEEDED");
    Statement scope(store_.db(),"SELECT owner_id,committed FROM leaderboard_games WHERE id=? AND game_id=? AND expires>?");scope.bind(1,gameplay);scope.bind(2,game);scope.bind(3,now());
    if(!scope.row())throw Error("NOT_FOUND");
    if(scope.text(0)!=user)throw Error("NOT_AUTHORIZED");
    const auto digest=sha256(args["entries"].dump());
    if(!scope.text(1).empty()){if(scope.text(1)!=digest)throw Error("INVALID_STATE");return Json::object();}
    std::set<std::tuple<std::string,std::string,long long>> unique;
    struct Pending {std::string user,key,columns;long long mode,rating;bool latest,ascending;};std::vector<Pending> rows;
    for(const auto& entry:args["entries"]) {
        const auto target=stringField(entry,"userId",64),key=stringField(entry,"key",64);
        const auto mode=integerField(entry,"mode",-2147483648LL,2147483647LL),rating=integerField(entry,"rating",std::numeric_limits<long long>::min(),std::numeric_limits<long long>::max());
        if(!unique.emplace(target,key,mode).second)throw Error("INVALID_ARGUMENT");
        Statement member(store_.db(),"SELECT 1 FROM leaderboard_game_members WHERE gameplay_id=? AND user_id=?");member.bind(1,gameplay);member.bind(2,target);if(!member.row())throw Error("NOT_AUTHORIZED");
        Statement definition(store_.db(),"SELECT ascending,aggregation,arbitrated,columns FROM leaderboards WHERE game_id=? AND key=? AND mode=?");definition.bind(1,game);definition.bind(2,key);definition.bind(3,mode);if(!definition.row())throw Error("NOT_FOUND");
        if(definition.number(2))throw Error("NOT_AUTHORIZED");
        if(!entry.contains("columns"))throw Error("INVALID_ARGUMENT");
        validateColumns(entry["columns"],parse(definition.text(3)));
        rows.push_back({target,key,entry["columns"].dump(),mode,rating,definition.text(1)=="latest",definition.number(0)!=0});
    }
    store_.exec("BEGIN IMMEDIATE");
    try {
        for(const auto& row:rows) {
            Statement write(store_.db(),"INSERT INTO leaderboard_entries(game_id,key,mode,user_id,rating,columns,updated) VALUES(?,?,?,?,?,?,?) ON CONFLICT(game_id,key,mode,user_id) DO UPDATE SET rating=excluded.rating,columns=excluded.columns,updated=excluded.updated WHERE ?=1 OR (?=1 AND excluded.rating<leaderboard_entries.rating) OR (?=0 AND excluded.rating>leaderboard_entries.rating)");
            write.bind(1,game);write.bind(2,row.key);write.bind(3,row.mode);write.bind(4,row.user);write.bind(5,row.rating);write.bind(6,row.columns);write.bind(7,now());write.bind(8,row.latest?1LL:0LL);write.bind(9,row.ascending?1LL:0LL);write.bind(10,row.ascending?1LL:0LL);(void)write.row();
        }
        Statement close(store_.db(),"UPDATE leaderboard_games SET committed=? WHERE id=?");close.bind(1,digest);close.bind(2,gameplay);(void)close.row();store_.exec("COMMIT");
    }catch(...){store_.exec("ROLLBACK");throw;}
    return Json::object();
}

}
