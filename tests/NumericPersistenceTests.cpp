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
Json call(Service& s,const std::string& op,const Json& args,const std::string& token={}) {
    static std::atomic<int> sequence{0};
    Json r{{"v",1},{"id","numeric-"+std::to_string(++sequence)},{"game","one"},{"op",op},{"args",args}};
    if(!token.empty())r["token"]=token;
    return parse(s.handle(r.dump(),"numeric-test"));
}
constexpr std::array<long long,5> Values{9007199254740991LL,9007199254740992LL,9007199254740993LL,9223372036854775807LL,(-9223372036854775807LL-1)};
}
int main() {
    try {
        const auto path=(std::filesystem::current_path()/"numeric-persistence.sqlite3").string();
        for(const auto* suffix:{"","-wal","-shm"})std::filesystem::remove(path+suffix);
        {
            Store db(path);db.title("one","One");
            (void)db.user("alice","alice-password","Alice");(void)db.user("bob","bob-password","Bob");
            for(int mode=0;mode<5;++mode) {
                const Json schema{{"integer","int64"},{"duration","timespan"},{"date","datetime"}};
                db.leaderboard("one",{{"key","BestScoreLifeTime"},{"mode",mode},{"ascending",false},{"aggregation","latest"},{"arbitrated",false},{"columns",schema}});
                const auto value=Values[mode];
                db.seedLeaderboard("one",{{"key","BestScoreLifeTime"},{"mode",mode},{"gamertag","Alice"},{"rating",value},
                    {"columns",{{"integer",{{"type","int64"},{"value",value}}},{"duration",{{"type","timespan"},{"value",value}}},
                    {"date",{{"type","datetime"},{"value",3155378975999999999LL}}}}}});
            }
            for(int i=0;i<8;++i)db.achievement("one",{{"key","award"+std::to_string(i)},{"name","Award"},{"description","d"},{"howToEarn","h"},{"score",1}});
        }
        std::string token;
        {
            Service s(path);auto login=call(s,"auth.login",{{"username","alice"},{"password","alice-password"}});check(login["error"]=="OK","login");token=login["result"]["token"];
            std::barrier start(8);std::array<Json,8> replies;std::array<std::thread,8> workers;
            for(int i=0;i<8;++i)workers[i]=std::thread([&,i]{start.arrive_and_wait();replies[i]=call(s,"achievements.award",{{"key","award"+std::to_string(i)},{"userId","bob"}},token);});
            for(auto& worker:workers)worker.join();
            for(const auto& r:replies)check(r["error"]=="OK"&&r["result"]["awarded"]==true,"concurrent authenticated awards accepted without gameplay proof");
        }
        {
            Service restarted(path);
            for(int mode=0;mode<5;++mode) {
                const auto reply=call(restarted,"leaderboards.read",{{"key","BestScoreLifeTime"},{"mode",mode},{"start",0},{"size",10}},token);
                check(reply["error"]=="OK","leaderboard read after restart");
                const auto& row=reply["result"]["entries"][0];
                check(row["rating"].get<long long>()==Values[mode],"exact rating after SQLite/restart/response JSON");
                check(row["columns"]["integer"]["value"].get<long long>()==Values[mode],"exact int64 column");
                check(row["columns"]["duration"]["value"].get<long long>()==Values[mode],"exact duration ticks");
                check(row["columns"]["date"]["value"].get<long long>()==3155378975999999999LL,"exact date ticks");
            }
            const auto list=call(restarted,"achievements.list",Json::object(),token);
            check(list["error"]=="OK"&&list["result"]["achievements"].size()==8,"all concurrent awards survive restart");
            for(const auto& row:list["result"]["achievements"])check(row["earnedTicks"].get<long long>()>0,"award persisted");
            Store db(path);Statement earned(db.db(),"SELECT u.username,COUNT(*) FROM earned e JOIN users u ON u.id=e.user_id GROUP BY e.user_id");
            check(earned.row()&&earned.text(0)=="alice"&&earned.number(1)==8&&!earned.row(),"token identity wins over supplied userId; no lost updates");
            for(const auto& value:{Json(9223372036854775808ULL),Json(18446744073709551615ULL),Json(1.5)}) {
                bool refused=false;try{validateColumns({{"integer",{{"type","int64"},{"value",value}}}},{{"integer","int64"}});}catch(const Error& e){refused=e.code()=="INVALID_ARGUMENT";}
                check(refused,"out-of-range and noninteger columns refused");
            }
        }
        for(const auto* suffix:{"","-wal","-shm"})std::filesystem::remove(path+suffix);
        std::cout<<checks<<" numeric/persistence/concurrency checks passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
