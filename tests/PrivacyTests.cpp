// SPDX-License-Identifier: MIT
// XNA GamerPrivileges as operator-set account policy, and members' block lists: each must change
// what the service actually does, not only what it reports.
#include "CnaService/Service.hpp"
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>

using namespace CnaService;
namespace {
int checks=0;
void check(bool value,const std::string& reason){++checks;if(!value)throw std::runtime_error(reason);}
Json call(Service& s,const std::string& op,const Json& args,const std::string& token) {
    static int sequence=0;
    Json r{{"v",1},{"id","privacy-"+std::to_string(++sequence)},{"game","one"},{"op",op},{"args",args}};
    if(!token.empty())r["token"]=token;
    return parse(s.handle(r.dump(),"privacy-test"));
}
std::string error(Service& s,const std::string& op,const Json& args,const std::string& token){return call(s,op,args,token)["error"];}
Json message(const std::string& to){return Json{{"gamertags",Json::array({to})},{"text","hi"}};}
}
int main() {
    try {
        const auto path=std::filesystem::current_path()/"privacy-unit.sqlite3";
        for(const auto* suffix:{"","-wal","-shm"})std::filesystem::remove(path.string()+suffix);
        {
            Store store(path.string());store.title("one","One");
            for(const auto* name:{"alice","bob","carol","dave"})(void)store.user(name,std::string(name)+"-password",std::string(1,name[0]-32)+(name+1));
        }
        Service s(path.string());
        std::map<std::string,std::string> token;
        auto signIn=[&](const std::string& name) {
            auto result=call(s,"auth.login",{{"username",name},{"password",name+"-password"}},"");
            check(result["error"]=="OK","sign-in "+name);token[name]=result["result"]["token"];return result["result"];
        };
        // Every privilege is allowed until the operator says otherwise, and the signed-in client is told.
        const auto first=signIn("alice");
        check(first["privileges"]==Json{{"communication","everyone"},{"profileViewing","everyone"},{"userContent","everyone"},
            {"trade",true},{"purchase",true},{"premium",true}},"default privileges");
        signIn("bob");signIn("carol");signIn("dave");
        auto befriend=[&](const std::string& a,const std::string& A,const std::string& b,const std::string& B) {
            check(error(s,"friends.add",{{"gamertag",B}},token[a])=="OK","request "+a);
            check(error(s,"friends.accept",{{"gamertag",A}},token[b])=="OK","accept "+b);
        };
        {
            Store store(path.string());
            store.privilege("carol","communication","friends");
            store.privilege("dave","communication","blocked");
            store.privilege("bob","profileViewing","friends");
            store.privilege("alice","purchase","blocked");
            bool refused=false;try{store.privilege("alice","communication","sometimes");}catch(const Error&){refused=true;}
            check(refused,"unknown setting value");
            refused=false;try{store.privilege("nobody","trade","blocked");}catch(const Error&){refused=true;}
            check(refused,"unknown account");
        }
        check(signIn("alice")["privileges"]["purchase"]==false,"the operator's setting reaches the next sign-in");
        signIn("carol");signIn("dave");signIn("bob");

        // Communication, friends only: nobody outside Carol's friends reaches her, nor she them.
        check(error(s,"messages.send",message("Carol"),token["bob"])=="NOT_AUTHORIZED","message to a friends-only account");
        check(error(s,"messages.send",message("Bob"),token["carol"])=="NOT_AUTHORIZED","message from a friends-only account");
        befriend("carol","Carol","alice","Alice");  // a friend request is how friends are made
        check(error(s,"messages.send",message("Carol"),token["alice"])=="OK","friends talk");
        check(error(s,"messages.send",message("Alice"),token["carol"])=="OK","both ways");
        // Communication blocked: no messages, friend requests or invitations either way.
        check(error(s,"messages.send",message("Alice"),token["dave"])=="NOT_AUTHORIZED","blocked sender");
        check(error(s,"messages.send",message("Dave"),token["alice"])=="NOT_AUTHORIZED","blocked recipient");
        check(error(s,"friends.add",{{"gamertag","Alice"}},token["dave"])=="NOT_AUTHORIZED","blocked friend request");
        check(error(s,"messages.send",message("Bob"),token["alice"])=="OK","others unaffected");

        // Profile viewing, friends only: Bob reads his own profile and his friends', nobody else's.
        check(error(s,"profile.get",{{"gamertag","Bob"}},token["bob"])=="OK","own profile");
        check(error(s,"profile.get",{{"gamertag","Alice"}},token["bob"])=="NOT_AUTHORIZED","stranger's profile");
        check(error(s,"gamer.lookup",{{"gamertag","Alice"}},token["bob"])=="NOT_AUTHORIZED","stranger's lookup");
        befriend("bob","Bob","alice","Alice");
        check(error(s,"profile.get",{{"gamertag","Alice"}},token["bob"])=="OK","friend's profile");
        check(error(s,"profile.get",{{"gamertag","Bob"}},token["alice"])=="OK","viewing is the viewer's setting");

        // Alice hosts a session Bob could find and be invited to.
        const Json create{{"kind","player"},{"maxGamers",4},{"privateSlots",0},{"allowJoinInProgress",true},
            {"participants",Json::array({token["alice"]})},{"properties",Json::array({nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr})}};
        const auto session=call(s,"sessions.create",create,token["alice"])["result"]["session"].get<std::string>();
        const Json find{{"kind","player"},{"localCount",1},{"properties",Json::array({nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr})},{"start",0},{"limit",10}};
        auto found=call(s,"sessions.find",find,token["bob"]);
        check(found["error"]=="OK"&&found["result"]["sessions"].size()==1,"visible before a block");
        const auto pending=call(s,"invites.send",{{"session",session},{"gamertag","Bob"}},token["alice"]);
        check(pending["error"]=="OK","invitation before a block");

        // Alice blocks Bob: the friendship ends, the pending invitation is withdrawn, and nothing
        // passes either way until she unblocks him.
        check(error(s,"privacy.block",{{"gamertag","Bob"}},token["alice"])=="OK","block");
        check(error(s,"privacy.block",{{"gamertag","Bob"}},token["alice"])=="OK","blocking twice is harmless");
        check(call(s,"privacy.list",Json::object(),token["alice"])["result"]["blocked"]==Json::array({"Bob"}),"block list");
        check(call(s,"privacy.list",Json::object(),token["bob"])["result"]["blocked"].empty(),"a block list is its owner's");
        check(call(s,"friends.list",Json::object(),token["bob"])["result"]["friends"].empty(),"the friendship ended");
        check(call(s,"invites.get",{{"invite",pending["result"]["invite"]}},token["bob"])["result"]["status"]=="dismissed","the invitation was withdrawn");
        check(error(s,"messages.send",message("Alice"),token["bob"])=="NOT_AUTHORIZED","blocked message in");
        check(error(s,"messages.send",message("Bob"),token["alice"])=="NOT_AUTHORIZED","blocked message out");
        check(error(s,"friends.add",{{"gamertag","Alice"}},token["bob"])=="NOT_AUTHORIZED","blocked friend request");
        check(error(s,"invites.send",{{"session",session},{"gamertag","Bob"}},token["alice"])=="NOT_AUTHORIZED","blocked invitation");
        check(error(s,"invites.joinFriend",{{"gamertag","Alice"}},token["bob"])=="NOT_AUTHORIZED","blocked join request");
        check(error(s,"profile.get",{{"gamertag","Alice"}},token["bob"])=="NOT_AUTHORIZED","blocked profile");
        check(error(s,"profile.get",{{"gamertag","Bob"}},token["alice"])=="NOT_AUTHORIZED","both ways");
        check(call(s,"sessions.find",find,token["bob"])["result"]["sessions"].empty(),"the blocker's session is not offered");
        check(error(s,"messages.send",message("Carol"),token["alice"])=="OK","a block concerns only the two");

        check(error(s,"privacy.unblock",{{"gamertag","Bob"}},token["alice"])=="OK","unblock");
        check(call(s,"privacy.list",Json::object(),token["alice"])["result"]["blocked"].empty(),"unblocked");
        check(error(s,"messages.send",message("Bob"),token["alice"])=="OK","talking again");
        check(call(s,"sessions.find",find,token["bob"])["result"]["sessions"].size()==1,"offered again");
        check(error(s,"privacy.block",{{"gamertag","Alice"}},token["alice"])=="INVALID_ARGUMENT","not oneself");
        check(error(s,"privacy.block",{{"gamertag","Nobody"}},token["alice"])=="NOT_FOUND","unknown gamertag");
        check(error(s,"privacy.list",{{"extra",1}},token["alice"])=="INVALID_ARGUMENT","list takes nothing");
        std::cout<<checks<<" privacy checks passed\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
