// SPDX-License-Identifier: MS-PL
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
        } else if(command=="revoke-user"&&argc==4) {
            CnaService::Statement s(store.db(),"DELETE FROM sessions WHERE user_id=(SELECT id FROM users WHERE username=?)");s.bind(1,argv[3]);(void)s.row();
        } else if(command=="reset-earned"&&argc==4) {
            CnaService::Statement s(store.db(),"DELETE FROM earned WHERE game_id=?");s.bind(1,argv[3]);(void)s.row();
        } else throw CnaService::Error("INVALID_ARGUMENT");
        return 0;
    } catch(const CnaService::Error& e){std::cerr<<e.code()<<'\n';}
      catch(...){std::cerr<<"Administration failed\n";}
    return 1;
}
