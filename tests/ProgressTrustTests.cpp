// SPDX-License-Identifier: MIT
#include "CnaService/Service.hpp"
#include <atomic>
#include <array>
#include <filesystem>
#include <iostream>
#include <limits>
#include <thread>
#include <barrier>
using namespace CnaService;
namespace {
int checks=0;
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
Json call(Service& s,const std::string& op,Json args,const std::string& token={},const std::string& repeat={}) {
    static std::atomic<int> sequence{0};
    Json r{{"v",1},{"id",repeat.empty()?"trust-"+std::to_string(++sequence):repeat},{"game","one"},{"op",op},{"args",std::move(args)}};
    if(!token.empty())r["token"]=token;
    return parse(s.handle(r.dump(),"trust-test"));
}
}
int main() {
    const auto path=(std::filesystem::current_path()/"progress-trust.sqlite3").string();
    auto clean=[&]{for(const auto* suffix:{"","-wal","-shm"})std::filesystem::remove(path+suffix);};
    try {
        clean();std::string aliceId,bobId;
        {Store db(path);db.title("one","One");db.title("two","Two");aliceId=db.user("alice","alice-password","Alice");bobId=db.user("bob","bob-password","Bob");
         db.achievement("one",{{"key","first"},{"name","First"},{"description","d"},{"howToEarn","h"},{"score",10}});
         db.leaderboard("one",{{"key","Score"},{"mode",0},{"ascending",false},{"aggregation","latest"},{"arbitrated",false},
             {"columns",{{"When","datetime"},{"Count","int32"},{"Scale","single"},{"Precision","double"}}}});
         for(const auto& [key,ascending]:{std::pair{"BestHigh",false},std::pair{"BestLow",true}})
             db.leaderboard("one",{{"key",key},{"mode",0},{"ascending",ascending},{"aggregation","best"},{"arbitrated",false},{"columns",Json::object()}});
         db.leaderboard("two",{{"key","Foreign"},{"mode",0},{"ascending",false},{"aggregation","latest"},{"arbitrated",false},{"columns",Json::object()}});}
        Service service(path);
        auto login=[&](const char* name){auto r=call(service,"auth.login",{{"username",name},{"password",std::string(name)+"-password"}});check(r["error"]=="OK","login");return r["result"]["token"].get<std::string>();};
        const auto alice=login("alice"),bob=login("bob");
        check(call(service,"achievements.award",{{"key","unknown"}},alice)["error"]=="NOT_FOUND","unknown achievement");
        check(call(service,"achievements.award",{{"key",nullptr}},alice)["error"]=="INVALID_ARGUMENT","malformed achievement");
        const auto start=now();
        auto award=call(service,"achievements.award",{{"key","first"},{"userId",bobId},{"earnedTicks",-1},{"progress",1e300}},alice,"award-repeat");
        check(award["error"]=="OK"&&award["result"]["awarded"]==true,"valid configured self-award is title-authored");
        check(call(service,"achievements.award",{{"key","first"}},alice,"award-repeat")==award,"request-id replay idempotent");
        check(call(service,"achievements.award",{{"key","first"}},alice)["result"]["awarded"]==false,"new-id duplicate not reawarded");
        {Store db(path);Statement row(db.db(),"SELECT user_id,ticks FROM earned");check(row.row()&&row.text(0)==aliceId,"token binds owner");
         const auto ticks=row.number(1);check(ticks>=621355968000000000LL+start*10000000LL&&ticks<=621355968000000000LL+(now()+1)*10000000LL,"timestamp is server-generated, not claimed");check(!row.row(),"duplicate creates no second row");}
        const auto originalTicks=call(service,"achievements.list",Json::object(),alice)["result"]["achievements"][0]["earnedTicks"];
        for(const auto& claimed:{Json(-1),Json(0),Json(2),Json(1e300),Json("malformed"),Json("2999-01-01")}) {
            check(call(service,"achievements.award",{{"key","first"},{"progress",claimed},{"earnedTicks",claimed}},alice)["result"]["awarded"]==false,"unsupported progress/timestamps cannot change completion");
            check(call(service,"achievements.list",Json::object(),alice)["result"]["achievements"][0]["earnedTicks"]==originalTicks,"duplicate preserves first timestamp");
        }
        check(call(service,"achievements.list",Json::object(),bob)["result"]["achievements"][0]["earnedTicks"]==0,"foreign account remains unearned");
        check(call(service,"achievements.award",{{"key","first"}},std::string(64,'0'))["error"]=="UNAUTHENTICATED","anonymous award refused");
        auto begin=[&]{auto r=call(service,"leaderboards.game.begin",{{"kind","local"},{"participants",Json::array({alice})}},alice);check(r["error"]=="OK","begin epoch");return r["result"]["gameplay"].get<std::string>();};
        const auto epoch=begin();
        auto row=[&](const std::string& user,Json rating,Json columns=Json::object()){return Json{{"userId",user},{"key","Score"},{"mode",0},{"rating",rating},{"columns",columns}};};
        auto commit=[&](Json entries,const std::string& token=std::string{}){return call(service,"leaderboards.game.commit",{{"gameplay",epoch},{"entries",entries}},token.empty()?alice:token);};
        auto noRows=[&]{Store db(path);Statement count(db.db(),"SELECT COUNT(*) FROM leaderboard_entries");check(count.row()&&count.number(0)==0,"rejected commit writes no rows");};
        check(commit(Json::array({row(bobId,5)}))["error"]=="NOT_AUTHORIZED","foreign gamer refused");noRows();
        check(commit(Json::array({row(aliceId,5)}),bob)["error"]=="NOT_AUTHORIZED","foreign epoch owner refused");noRows();
        for(const auto& value:{Json(9223372036854775808ULL),Json(18446744073709551615ULL),Json(1.5),Json("NaN"),Json(nullptr),Json(true)}) {
            check(commit(Json::array({row(aliceId,value)}))["error"]=="INVALID_ARGUMENT","impossible rating refused");noRows();
        }
        for(const char* literal:{"NaN","Infinity","1e999"}) {
            auto request=Json{{"v",1},{"id",std::string("invalid-number-")+literal},{"game","one"},{"op","leaderboards.game.commit"},{"token",alice},
                {"args",{{"gameplay",epoch},{"entries",Json::array({row(aliceId,777)})}}}}.dump();
            request.replace(request.find("777"),3,literal);
            check(parse(service.handle(request,"trust-test"))["error"]!="OK","nonfinite wire number rejected");noRows();
        }
        for(const auto& field:{Json{{"When",{{"type","datetime"},{"value",-1}}}},Json{{"When",{{"type","datetime"},{"value",3155378976000000000LL}}}},
            Json{{"Count",{{"type","int32"},{"value",2147483648LL}}}},Json{{"Scale",{{"type","single"},{"value",1e300}}}},
            Json{{"Precision",{{"type","double"},{"value",nullptr}}}},Json{{"Missing",{{"type","int32"},{"value",1}}}}}) {
            check(commit(Json::array({row(aliceId,5,field)}))["error"]=="INVALID_ARGUMENT","invalid column refused");noRows();
        }
        for(const char* key:{"Missing","Foreign"}) {
            auto foreign=row(aliceId,5);foreign["key"]=key;
            check(commit(Json::array({foreign}))["error"]=="NOT_FOUND","undefined or other-title board refused");noRows();
        }
        check(call(service,"leaderboards.game.commit",{{"gameplay",std::string(32,'0')},{"entries",Json::array({row(aliceId,5)})}},alice)["error"]=="NOT_FOUND","nonexistent epoch refused");
        check(call(service,"leaderboards.game.commit",{{"gameplay",epoch},{"entries",Json::array({row(aliceId,5)})}},std::string(64,'0'))["error"]=="UNAUTHENTICATED","anonymous score refused");
        check(commit(Json::array({row(aliceId,5),row(aliceId,6)}))["error"]=="INVALID_ARGUMENT","duplicate row refused");noRows();
        check(commit(Json::array({row(aliceId,5),row(bobId,6)}))["error"]=="NOT_AUTHORIZED","mixed batch refused atomically");noRows();
        const auto entries=Json::array({row(aliceId,std::numeric_limits<long long>::max(),{{"When",{{"type","datetime"},{"value",3155378975999999999LL}}}})});
        check(commit(entries)["error"]=="OK","full XNA signed rating range is legal client-authored state");
        check(commit(entries)["error"]=="OK","same epoch same payload idempotent");
        check(commit(Json::array({row(aliceId,7)}))["error"]=="INVALID_STATE","committed epoch cannot change report");
        auto read=call(service,"leaderboards.read",{{"key","Score"},{"mode",0},{"start",0},{"size",10}},alice);
        check(read["error"]=="OK"&&read["result"]["entries"].size()==1&&read["result"]["entries"][0]["rating"]==std::numeric_limits<long long>::max(),"server ranks stored client assertion exactly");
        auto write=[&](const std::string& key,long long score) {
            auto entry=row(aliceId,score);entry["key"]=key;
            const auto game=begin();
            check(call(service,"leaderboards.game.commit",{{"gameplay",game},{"entries",Json::array({entry})}},alice)["error"]=="OK","provisioned aggregation write");
        };
        auto stored=[&](const std::string& key) {
            auto page=call(service,"leaderboards.read",{{"key",key},{"mode",0},{"start",0},{"size",10}},alice);
            check(page["error"]=="OK"&&page["result"]["entries"].size()==1,"read aggregated row");
            return page["result"]["entries"][0]["rating"].get<long long>();
        };
        write("Score",-7);check(stored("Score")==-7,"latest permits deliberate rollback and negative signed scores");
        for(const char* key:{"BestHigh","BestLow"}) {write(key,10);write(key,5);write(key,20);write(key,20);}
        check(stored("BestHigh")==20&&stored("BestLow")==5,"best follows configured direction, repeated ties stable");
        const auto firstEpoch=begin(),secondEpoch=begin();std::barrier gate(3);std::array<Json,2> outcomes;
        auto competing=[&](int index,const std::string& game,long long score) {
            gate.arrive_and_wait();auto entry=row(aliceId,score);entry["key"]="BestHigh";
            outcomes[index]=call(service,"leaderboards.game.commit",{{"gameplay",game},{"entries",Json::array({entry})}},alice);
        };
        std::thread first(competing,0,firstEpoch,100),second(competing,1,secondEpoch,50);gate.arrive_and_wait();first.join();second.join();
        check(outcomes[0]["error"]=="OK"&&outcomes[1]["error"]=="OK"&&stored("BestHigh")==100,"concurrent best cannot roll back");
        auto bobGame=call(service,"leaderboards.game.begin",{{"kind","local"},{"participants",Json::array({bob})}},bob);
        check(bobGame["error"]=="OK","second account epoch");
        auto high=row(bobId,90),low=row(bobId,7);high["key"]="BestHigh";low["key"]="BestLow";
        check(call(service,"leaderboards.game.commit",{{"gameplay",bobGame["result"]["gameplay"]},{"entries",Json::array({high,low})}},bob)["error"]=="OK","own second account scores");
        for(const char* key:{"BestHigh","BestLow"}) {
            auto page=call(service,"leaderboards.read",{{"key",key},{"mode",0},{"start",0},{"size",10}},alice)["result"]["entries"];
            check(page.size()==2&&page[0]["userId"]==aliceId&&page[1]["userId"]==bobId&&page[0]["rank"]==1&&page[1]["rank"]==2,"server derives sorted ranks in both directions");
        }
        for(const char* op:{"achievements.progress","profile.setStatistics","skill.submit","rewards.claim","avatars.unlock"})
            check(call(service,op,Json::object(),alice)["error"]=="UNKNOWN_OPERATION","no unimplemented progression mutation route");
        check(call(service,"auth.logout",Json::object(),alice)["error"]=="OK","logout");
        check(call(service,"achievements.award",{{"key","first"}},alice)["error"]=="UNAUTHENTICATED","revoked token cannot award");
        clean();std::cout<<checks<<" progress trust checks passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
