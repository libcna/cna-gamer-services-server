// SPDX-License-Identifier: MIT
#include "CnaService/Store.hpp"
#include <iostream>
#include <fstream>
#include <filesystem>
int main(int argc,char** argv) {
    try {
        if(argc<3)throw CnaService::Error("INVALID_ARGUMENT");
        CnaService::Store store(argv[1]);const std::string command=argv[2];
        if(command=="title"&&argc==5)store.title(argv[3],argv[4]);
        else if(command=="user"&&argc==5) {
            std::string password;std::getline(std::cin,password);
            std::cout<<store.user(argv[3],password,argv[4])<<'\n';
        } else if(command=="achievement"&&argc==4) {
            std::string data((std::istreambuf_iterator<char>(std::cin)),{});
            store.achievement(argv[3],CnaService::parse(data));
        } else if((command=="leaderboard"||command=="seed-leaderboard")&&argc==4) {
            std::string data((std::istreambuf_iterator<char>(std::cin)),{});const auto value=CnaService::parse(data);
            if(command=="leaderboard")store.leaderboard(argv[3],value);else store.seedLeaderboard(argv[3],value);
        } else if(command=="asset"&&argc==6) {
            if(std::filesystem::file_size(argv[5])>16777216)throw CnaService::Error("LIMIT_EXCEEDED");
            std::ifstream input(argv[5],std::ios::binary);if(!input)throw CnaService::Error("NOT_FOUND");
            std::string bytes((std::istreambuf_iterator<char>(input)),{});std::cout<<store.asset(argv[3],argv[4],bytes)<<'\n';
        } else if(command=="picture"&&argc==5) {store.picture(argv[3],argv[4]);
        } else if(command=="inspect"&&argc==3) {
            for(const auto* sql:{"SELECT COUNT(*) FROM users","SELECT COUNT(*) FROM titles","SELECT COUNT(*) FROM sessions","SELECT COUNT(*) FROM earned"}) {
                CnaService::Statement s(store.db(),sql);(void)s.row();std::cout<<s.number(0)<<'\n';
            }
        } else if((command=="inspect-online"||command=="reset-online")&&argc==4) {
            if(!CnaService::identifier(argv[3]))throw CnaService::Error("INVALID_ARGUMENT");
            CnaService::Statement title(store.db(),"SELECT 1 FROM titles WHERE id=?");title.bind(1,argv[3]);
            if(!title.row())throw CnaService::Error("UNKNOWN_TITLE");
            if(command=="reset-online") {
                CnaService::Statement remove(store.db(),"DELETE FROM directory_sessions WHERE game_id=?");remove.bind(1,argv[3]);(void)remove.row();
            } else {
                CnaService::Json counts=CnaService::Json::object();
                for(const auto& [name,query]:std::initializer_list<std::pair<const char*,const char*>>{
                    {"sessions","SELECT COUNT(*) FROM directory_sessions WHERE game_id=?"},
                    {"members","SELECT COUNT(*) FROM directory_members WHERE game_id=?"},
                    {"invitations","SELECT COUNT(*) FROM session_invitations WHERE game_id=?"},
                    {"senderLimits","SELECT COUNT(*) FROM invitation_send_limits WHERE game_id=?"},
                    {"relayTickets","SELECT COUNT(*) FROM relay_tickets WHERE game_id=?"}}) {
                    CnaService::Statement count(store.db(),query);count.bind(1,argv[3]);(void)count.row();counts[name]=count.number(0);
                }
                std::cout<<counts.dump()<<'\n';
            }
        } else if(command=="revoke-user"&&argc==4) {
            store.exec("BEGIN IMMEDIATE");
            CnaService::Statement families(store.db(),"DELETE FROM refresh_families WHERE user_id=(SELECT id FROM users WHERE username=?)");families.bind(1,argv[3]);(void)families.row();
            CnaService::Statement s(store.db(),"DELETE FROM sessions WHERE user_id=(SELECT id FROM users WHERE username=?)");s.bind(1,argv[3]);(void)s.row();store.exec("COMMIT");
        } else if(command=="expire-access"&&argc==4) {
            CnaService::Statement s(store.db(),"UPDATE sessions SET expires=0 WHERE user_id=(SELECT id FROM users WHERE username=?)");s.bind(1,argv[3]);(void)s.row();
        } else if(command=="reset-earned"&&argc==4) {
            CnaService::Statement s(store.db(),"DELETE FROM earned WHERE game_id=?");s.bind(1,argv[3]);(void)s.row();
        } else throw CnaService::Error("INVALID_ARGUMENT");
        return 0;
    } catch(const CnaService::Error& e){std::cerr<<e.code()<<'\n';}
      catch(...){std::cerr<<"Administration failed\n";}
    return 1;
}
