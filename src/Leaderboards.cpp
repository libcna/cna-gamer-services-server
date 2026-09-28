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
}
