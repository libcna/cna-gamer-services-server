// SPDX-License-Identifier: MIT
#include "CnaService/Service.hpp"
#include <algorithm>

namespace CnaService {
void Service::revokeFamily(const std::string& family) {
    store_.exec("BEGIN IMMEDIATE");
    try {
        Statement mark(store_.db(),"UPDATE refresh_families SET revoked=1 WHERE id=?");mark.bind(1,family);(void)mark.row();
        Statement remove(store_.db(),"DELETE FROM sessions WHERE refresh_family=?");remove.bind(1,family);(void)remove.row();
        store_.exec("COMMIT");
    }catch(...){store_.exec("ROLLBACK");throw;}
}
Json Service::issueCredentials(const std::string& user,const std::string& game,const std::string& existingFamily) {
    const auto timestamp=now();auto family=existingFamily;auto refreshExpires=timestamp+30LL*86400;
    Statement prune(store_.db(),"DELETE FROM refresh_families WHERE expires<=? OR revoked=1");prune.bind(1,timestamp);(void)prune.row();
    Statement expired(store_.db(),"DELETE FROM sessions WHERE expires<=?");expired.bind(1,timestamp);(void)expired.row();
    if(family.empty()) {
        Statement cap(store_.db(),"SELECT COUNT(*) FROM refresh_families WHERE user_id=? AND revoked=0");cap.bind(1,user);(void)cap.row();if(cap.number(0)>=32)throw Error("LIMIT_EXCEEDED");
        Statement access(store_.db(),"SELECT COUNT(*) FROM sessions WHERE user_id=?");access.bind(1,user);(void)access.row();if(access.number(0)>=32)throw Error("LIMIT_EXCEEDED");
        family=randomHex(16);
    }else {
        Statement scope(store_.db(),"SELECT expires,rotations,revoked FROM refresh_families WHERE id=? AND user_id=? AND game_id=?");scope.bind(1,family);scope.bind(2,user);scope.bind(3,game);
        if(!scope.row()||scope.number(2)||scope.number(0)<=timestamp)throw Error("UNAUTHENTICATED");
        if(scope.number(1)>=1024)throw Error("LIMIT_EXCEEDED");
        refreshExpires=scope.number(0);
    }
    const auto token=randomHex(32),refresh=randomHex(32);const auto expires=std::min(timestamp+3600,refreshExpires);
    store_.exec("BEGIN IMMEDIATE");
    try {
        if(existingFamily.empty()) {
            Statement create(store_.db(),"INSERT INTO refresh_families(id,user_id,game_id,expires) VALUES(?,?,?,?)");create.bind(1,family);create.bind(2,user);create.bind(3,game);create.bind(4,refreshExpires);(void)create.row();
        }else {
            Statement retire(store_.db(),"UPDATE refresh_credentials SET used=1 WHERE family_id=? AND used=0");retire.bind(1,family);(void)retire.row();
            Statement remove(store_.db(),"DELETE FROM sessions WHERE refresh_family=?");remove.bind(1,family);(void)remove.row();
            Statement count(store_.db(),"UPDATE refresh_families SET rotations=rotations+1 WHERE id=?");count.bind(1,family);(void)count.row();
        }
        Statement access(store_.db(),"INSERT INTO sessions(hash,user_id,game_id,expires,last_seen,refresh_family) VALUES(?,?,?,?,?,?)");access.bind(1,sha256(token));access.bind(2,user);access.bind(3,game);access.bind(4,expires);access.bind(5,timestamp);access.bind(6,family);(void)access.row();
        Statement credential(store_.db(),"INSERT INTO refresh_credentials(hash,family_id) VALUES(?,?)");credential.bind(1,sha256(refresh));credential.bind(2,family);(void)credential.row();
        store_.exec("COMMIT");
    }catch(...){store_.exec("ROLLBACK");throw;}
    return Json{{"identity",identity(user)},{"token",token},{"expires",expires},{"refreshToken",refresh},{"refreshExpires",refreshExpires}};
}
Json Service::refreshCredentials(const std::string& game,const Json& args) {
    const auto credential=stringField(args,"refreshToken",64);
    if(credential.size()!=64||credential.find_first_not_of("0123456789abcdef")!=std::string::npos)throw Error("UNAUTHENTICATED");
    Statement scope(store_.db(),"SELECT f.id,f.user_id,f.revoked,f.expires,c.used FROM refresh_credentials c JOIN refresh_families f ON f.id=c.family_id WHERE c.hash=? AND f.game_id=?");scope.bind(1,sha256(credential));scope.bind(2,game);
    if(!scope.row()||scope.number(2)||scope.number(3)<=now())throw Error("UNAUTHENTICATED");
    const auto family=scope.text(0),user=scope.text(1);
    if(scope.number(4)){revokeFamily(family);throw Error("UNAUTHENTICATED");}
    return issueCredentials(user,game,family);
}
}
