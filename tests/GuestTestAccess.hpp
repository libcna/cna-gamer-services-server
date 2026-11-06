// SPDX-License-Identifier: MIT
#pragma once
#include "CnaService/Service.hpp"
namespace CnaService {
// Friend-only test mechanisms. Every verdict is made by production Service/SQL,
// never by a test implementation of the cap or authorization policy.
struct GuestTestAccess {
    static void clock(Service& s,std::function<long long()> clock){s.guestClock_=std::move(clock);}
    static void seed(Service& s,const std::string& actor,const std::string& game,int count,bool sameAccount,bool sameTitle) {
        Store::Transaction transaction(s.store_);
        std::string parent,family,title;
        for(int i=0;i<count;++i) {
            title=sameTitle?game:"guestfill"+std::to_string(i/1024);
            if(i%1024==0&&!sameTitle)s.store_.title(title,title);
            if(i%24==0) {
                parent=actor;
                if(!sameAccount) {
                    parent=randomHex(16);const auto name="Seed"+std::to_string(i);
                    Statement user(s.store_.db(),"INSERT INTO users(id,username,gamertag,salt,verifier) SELECT ?,?,?,salt,verifier FROM users WHERE id=?");
                    user.bind(1,parent);user.bind(2,name);user.bind(3,name);user.bind(4,actor);(void)user.row();
                }
            }
            // Family and title change at either boundary. These fixture accounts are not
            // logged in; no password/access credentials are fabricated for an operation.
            if(i%3==0||i%1024==0) {
                family=randomHex(16);Statement f(s.store_.db(),"INSERT INTO refresh_families(id,user_id,game_id,expires) VALUES(?,?,?,?)");
                f.bind(1,family);f.bind(2,parent);f.bind(3,title);f.bind(4,now()+3600);(void)f.row();
            }
            Statement guest(s.store_.db(),"INSERT INTO guest_identities(id,parent_id,family_id,game_id,slot,gamertag) VALUES(?,?,?,?,?,?)");
            guest.bind(1,"g:"+randomHex(16));guest.bind(2,parent);guest.bind(3,family);guest.bind(4,title);guest.bind(5,static_cast<long long>(i%3+1));guest.bind(6,"Seed (1)");(void)guest.row();
        }
        transaction.commit();
    }
};
}
