// SPDX-License-Identifier: MIT
// A request ID, the change it makes and its outcome commit together. Each case runs the request in
// a child process that dies at a named point with its transaction open -- a real crash, recovered
// by SQLite when the next process opens the database -- then restarts the service and retries the
// same request ID, as a client whose response never arrived does.
#include "CnaService/Service.hpp"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

using namespace CnaService;
namespace {
int checks=0;
void check(bool value,const std::string& reason){++checks;if(!value)throw std::runtime_error(reason);}
const char* crashAt=nullptr;
void crash(const char* point){if(crashAt&&std::strcmp(point,crashAt)==0)std::_Exit(0);}
void failAt(const char* point){if(crashAt&&std::strcmp(point,crashAt)==0)throw Error("INTERNAL_ERROR");}
Json call(Service& service,const std::string& op,const Json& args,const std::string& token,const std::string& id) {
    Json request{{"v",1},{"id",id},{"game","one"},{"op",op},{"args",args}};
    if(!token.empty())request["token"]=token;
    return parse(service.handle(request.dump(),"atomicity-test"));
}
long long count(const std::filesystem::path& path,const char* sql) {
    Store store(path.string());Statement s(store.db(),sql);(void)s.row();return s.number(0);
}
struct Fixture {
    std::filesystem::path path;std::string alice,bob;
    explicit Fixture(const std::string& name):path(std::filesystem::current_path()/("atomicity-"+name+".sqlite3")) {
        for(const auto* suffix:{"","-wal","-shm"})std::filesystem::remove(path.string()+suffix);
        {
            Store store(path.string());store.title("one","One");
            (void)store.user("alice","alice-password","Alice");(void)store.user("bob","bob-password","Bob");
            (void)store.user("carol","carol-password","Carol");
            store.achievement("one",Json{{"key","first"},{"name","First"},{"description","d"},{"howToEarn","h"},{"score",10}});
        }
        Service service(path.string());
        alice=call(service,"auth.login",{{"username","alice"},{"password","alice-password"}},"",name+"-login-a")["result"]["token"];
        bob=call(service,"auth.login",{{"username","bob"},{"password","bob-password"}},"",name+"-login-b")["result"]["token"];
    }
    // Runs one request in a child process that dies at the point; nothing of the child survives
    // but what it committed.
    void crashDuring(const char* point,const std::string& op,const Json& args,const std::string& token,const std::string& id) {
        const auto child=fork();
        if(child==0) {
            crashAt=point;Service::setFaultHookForTesting(crash);
            try{Service service(path.string());(void)call(service,op,args,token,id);}catch(...){}
            std::_Exit(3);
        }
        int status=0;check(child>0&&waitpid(child,&status,0)==child,"child");
        check(WIFEXITED(status)&&WEXITSTATUS(status)==0,std::string("the request reached ")+point);
    }
};
const Json Message{{"gamertags",Json::array({"Bob","Carol"})},{"text","hello"}};
constexpr const char* Messages="SELECT COUNT(*) FROM messages";
}

int main() {
    try {
        // messages.send must happen exactly once: every crash before the commit leaves nothing, and
        // the retry sends both messages; a crash after it leaves both, and the retry sends none.
        for(const auto* point:{"before-record","after-record","hint","before-commit"}) {
            Fixture fixture(point);
            fixture.crashDuring(point,"messages.send",Message,fixture.alice,"send-1");
            check(count(fixture.path,Messages)==0,std::string("nothing of the request survives a crash at ")+point);
            check(count(fixture.path,"SELECT COUNT(*) FROM request_ids WHERE id='send-1'")==0,std::string("the ID is unused after a crash at ")+point);
            Service restarted(fixture.path.string());
            const auto retry=call(restarted,"messages.send",Message,fixture.alice,"send-1");
            check(retry["error"]=="OK"&&retry["result"]["sent"]==2,std::string("the retry runs after a crash at ")+point);
            check(call(restarted,"messages.send",Message,fixture.alice,"send-1")==retry,"a second retry is answered from the record");
            check(count(fixture.path,Messages)==2,std::string("sent exactly once after a crash at ")+point);
        }
        {
            Fixture fixture("after-commit");
            fixture.crashDuring("after-commit","messages.send",Message,fixture.alice,"send-1");
            check(count(fixture.path,Messages)==2,"a committed request survives a crash before its response");
            Service restarted(fixture.path.string());
            const auto retry=call(restarted,"messages.send",Message,fixture.alice,"send-1");
            check(retry["error"]=="OK"&&retry["result"]["sent"]==2,"the retry hears the committed outcome");
            check(count(fixture.path,Messages)==2,"and sends nothing again");
            // The ID belongs to its account and operation: anything else reusing it is refused.
            check(call(restarted,"messages.send",Message,fixture.bob,"send-1")["error"]=="DUPLICATE_REQUEST","another account's reuse");
            check(call(restarted,"friends.add",{{"gamertag","Bob"}},fixture.alice,"send-1")["error"]=="DUPLICATE_REQUEST","another operation's reuse");
            check(count(fixture.path,Messages)==2,"no reuse sends anything");
        }
        {
            // A session is created once: the retry after a crash before the response returns the
            // same session rather than a second one.
            Fixture fixture("session");
            const Json create{{"kind","player"},{"maxGamers",4},{"privateSlots",0},{"allowJoinInProgress",false},
                {"participants",Json::array({fixture.alice})},{"properties",Json::array({nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr})}};
            fixture.crashDuring("after-commit","sessions.create",create,fixture.alice,"create-1");
            Service restarted(fixture.path.string());
            const auto retry=call(restarted,"sessions.create",create,fixture.alice,"create-1");
            check(retry["error"]=="OK","the created session is reported");
            check(count(fixture.path,"SELECT COUNT(*) FROM directory_sessions")==1,"exactly one session");
            Store store(fixture.path.string());Statement id(store.db(),"SELECT id FROM directory_sessions");(void)id.row();
            check(retry["result"]["session"]==id.text(0),"the retry names the session the crashed request created");
        }
        {
            // An award's "newly earned" survives a lost response: the retry still says it was this
            // request that earned it, so the unlock is shown once.
            Fixture fixture("award");
            fixture.crashDuring("after-commit","achievements.award",{{"key","first"}},fixture.alice,"award-1");
            Service restarted(fixture.path.string());
            const auto retry=call(restarted,"achievements.award",{{"key","first"}},fixture.alice,"award-1");
            check(retry["error"]=="OK"&&retry["result"]["awarded"]==true,"the retry reports the award it made");
            check(call(restarted,"achievements.award",{{"key","first"}},fixture.alice,"award-2")["result"]["awarded"]==false,"a new request earns nothing new");
        }
        {
            // A refusal is an outcome: its repeat is refused the same way even after the state
            // changed, while a new request sees the new state.
            Fixture fixture("refusal");
            Service service(fixture.path.string());
            const Json toDave{{"gamertags",Json::array({"Dave"})},{"text","hi"}};
            check(call(service,"messages.send",toDave,fixture.alice,"dave-1")["error"]=="NOT_FOUND","unknown recipient");
            {Store store(fixture.path.string());(void)store.user("dave","dave-password","Dave");}
            check(call(service,"messages.send",toDave,fixture.alice,"dave-1")["error"]=="NOT_FOUND","the repeat hears the recorded refusal");
            check(call(service,"messages.send",toDave,fixture.alice,"dave-2")["error"]=="OK","a new request runs");
        }
        {
            // An internal failure is not an outcome: everything, the ID included, rolls back, so the
            // retry runs the request.
            Fixture fixture("internal");
            Service service(fixture.path.string());
            crashAt="before-commit";Service::setFaultHookForTesting(failAt);
            check(call(service,"messages.send",Message,fixture.alice,"send-1")["error"]=="INTERNAL_ERROR","the failure is reported");
            Service::setFaultHookForTesting(nullptr);crashAt=nullptr;
            check(count(fixture.path,Messages)==0&&count(fixture.path,"SELECT COUNT(*) FROM request_ids WHERE id='send-1'")==0,"nothing of it is kept");
            check(call(service,"messages.send",Message,fixture.alice,"send-1")["result"]["sent"]==2,"the retry runs");
            check(count(fixture.path,Messages)==2,"exactly once");
        }
        {
            // Results that carry a secret are never stored: a repeated sign-in is refused as before,
            // and no access or refresh token is kept in the request record.
            Fixture fixture("secret");
            Service service(fixture.path.string());
            const Json login{{"username","carol"},{"password","carol-password"}};
            const auto first=call(service,"auth.login",login,"","login-1");
            check(first["error"]=="OK","sign-in");
            check(call(service,"auth.login",login,"","login-1")["error"]=="DUPLICATE_REQUEST","a repeated sign-in is refused");
            check(count(fixture.path,"SELECT COUNT(*) FROM request_ids WHERE result IS NOT NULL AND op IN ('auth.login','auth.refresh','sessions.relayTicket')")==0,
                "no credential is stored");
            // A wrong password is recorded like any refusal and repeats as one.
            check(call(service,"auth.login",{{"username","carol"},{"password","wrong-password"}},"","login-2")["error"]=="AUTHENTICATION_FAILED","wrong password");
            check(call(service,"auth.login",login,"","login-2")["error"]=="AUTHENTICATION_FAILED","its ID is spent on the refusal");
            // A replayed refresh credential still revokes its family although the request is refused.
            const auto refresh=first["result"]["refreshToken"].get<std::string>();
            const auto rotated=call(service,"auth.refresh",{{"refreshToken",refresh}},"","refresh-1");
            check(rotated["error"]=="OK","rotation");
            check(call(service,"auth.refresh",{{"refreshToken",refresh}},"","refresh-2")["error"]=="UNAUTHENTICATED","replayed credential refused");
            check(call(service,"auth.ping",Json::object(),rotated["result"]["token"],"ping-1")["error"]=="UNAUTHENTICATED","and its family revoked");
        }
        {
            // A database from before request outcomes were kept answers every old ID as a duplicate.
            Fixture fixture("legacy");
            {Store store(fixture.path.string());store.exec("INSERT INTO request_ids(game_id,id,created) VALUES('one','old-1',strftime('%s','now'))");}
            Service service(fixture.path.string());
            check(call(service,"messages.send",Message,fixture.alice,"old-1")["error"]=="DUPLICATE_REQUEST","legacy ID");
            check(count(fixture.path,Messages)==0,"legacy ID runs nothing");
        }
        std::cout<<checks<<" atomicity checks passed\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
