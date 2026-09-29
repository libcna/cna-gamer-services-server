// SPDX-License-Identifier: MIT
#include "CnaService/Store.hpp"
#include "InitialMigration.hpp"
#include "AssetMigration.hpp"
#include "LeaderboardMigration.hpp"
#include "LeaderboardGameMigration.hpp"
#include "RefreshMigration.hpp"
#include "SessionDirectoryMigration.hpp"
#include "InvitationMigration.hpp"
#include "RelayTicketMigration.hpp"
#include "RankedLobbyMigration.hpp"
#include "RankedArbitrationMigration.hpp"
#include "SocialMigration.hpp"
#include "AvatarMigration.hpp"
#include "AvatarFeatureMigration.hpp"
#include "PresenceStatusMigration.hpp"
#include "HostMigrationMigration.hpp"
#include "SessionRemovalMigration.hpp"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <chrono>
#include <fstream>
#include <array>

namespace CnaService {
namespace {
std::string hex(const unsigned char* bytes, std::size_t length) {
    constexpr char digits[]="0123456789abcdef";
    std::string result(length*2,'0');
    for (std::size_t i=0;i<length;++i) { result[i*2]=digits[bytes[i]>>4]; result[i*2+1]=digits[bytes[i]&15]; }
    return result;
}
}
std::string randomHex(std::size_t bytes) {
    if (bytes > 128) throw Error("LIMIT_EXCEEDED");
    std::array<unsigned char,128> buffer{};
    if (RAND_bytes(buffer.data(),static_cast<int>(bytes))!=1) throw Error("INTERNAL_ERROR");
    return hex(buffer.data(),bytes);
}
std::string sha256(std::string_view bytes) {
    std::array<unsigned char,32> digest{};
    unsigned int size=0;
    if (EVP_Digest(bytes.data(),bytes.size(),digest.data(),&size,EVP_sha256(),nullptr)!=1 || size!=32)
        throw Error("INTERNAL_ERROR");
    return hex(digest.data(),digest.size());
}
std::string passwordHash(std::string_view password, std::string_view salt) {
    std::array<unsigned char,32> digest{};
    if (EVP_PBE_scrypt(password.data(),password.size(),reinterpret_cast<const unsigned char*>(salt.data()),
        salt.size(),32768,8,1,64*1024*1024,digest.data(),digest.size())!=1) throw Error("INTERNAL_ERROR");
    return hex(digest.data(),digest.size());
}
long long now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
Statement::Statement(sqlite3* db, const char* sql) {
    if (sqlite3_prepare_v2(db,sql,-1,&statement_,nullptr)!=SQLITE_OK) throw Error("INTERNAL_ERROR");
}
Statement::~Statement() { sqlite3_finalize(statement_); }
void Statement::bind(int index,std::string_view value) {
    if (sqlite3_bind_text(statement_,index,value.data(),static_cast<int>(value.size()),SQLITE_TRANSIENT)!=SQLITE_OK)
        throw Error("INTERNAL_ERROR");
}
void Statement::blob(int index,std::string_view value) {
    if(value.size()>16777216||sqlite3_bind_blob(statement_,index,value.data(),static_cast<int>(value.size()),SQLITE_TRANSIENT)!=SQLITE_OK)
        throw Error("LIMIT_EXCEEDED");
}
std::string Statement::blob(int index) const {
    const auto size=sqlite3_column_bytes(statement_,index);if(size<0||size>16777216)throw Error("LIMIT_EXCEEDED");
    const auto* bytes=static_cast<const char*>(sqlite3_column_blob(statement_,index));return bytes?std::string(bytes,size):std::string{};
}
void Statement::bind(int index,long long value) {
    if (sqlite3_bind_int64(statement_,index,value)!=SQLITE_OK) throw Error("INTERNAL_ERROR");
}
bool Statement::row() {
    int result=sqlite3_step(statement_);
    if (result==SQLITE_ROW) return true;
    if (result==SQLITE_DONE) return false;
    if (result==SQLITE_CONSTRAINT) throw Error("CONFLICT");
    throw Error("INTERNAL_ERROR");
}
std::string Statement::text(int index) const {
    const auto* text=sqlite3_column_text(statement_,index);
    return text ? std::string(reinterpret_cast<const char*>(text),static_cast<std::size_t>(sqlite3_column_bytes(statement_,index))) : "";
}
long long Statement::number(int index) const { return sqlite3_column_int64(statement_,index); }
Store::Store(const std::string& path) {
    if (sqlite3_open_v2(path.c_str(),&db_,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX,nullptr)!=SQLITE_OK) {
        sqlite3_close(db_); db_=nullptr; throw Error("INTERNAL_ERROR");
    }
    try {
        sqlite3_busy_timeout(db_,5000);
        exec("PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL;");
        // Read in its own scope: a live statement would keep migrations from dropping tables.
        const auto current=[&] {
            Statement version(db_,"PRAGMA user_version");
            (void)version.row();
            return version.number(0);
        }();
        if (current>SchemaVersion) throw Error("UNSUPPORTED_DATABASE_VERSION");
        if (current==0) {
            exec("BEGIN IMMEDIATE");
            exec(InitialMigration);
            exec("COMMIT");
        }
        if(current<2){exec("BEGIN IMMEDIATE");exec(AssetMigration);exec("COMMIT");}
        if(current<3){exec("BEGIN IMMEDIATE");exec(LeaderboardMigration);exec("COMMIT");}
        if(current<4){exec("BEGIN IMMEDIATE");exec(LeaderboardGameMigration);exec("COMMIT");}
        if(current<5){exec("BEGIN IMMEDIATE");exec(RefreshMigration);exec("COMMIT");}
        if(current<6){exec("BEGIN IMMEDIATE");exec(SessionDirectoryMigration);exec("COMMIT");}
        if(current<7){exec("BEGIN IMMEDIATE");exec(InvitationMigration);exec("COMMIT");}
        if(current<8){exec("BEGIN IMMEDIATE");exec(RelayTicketMigration);exec("COMMIT");}
        if(current<9){exec("BEGIN IMMEDIATE");exec(RankedLobbyMigration);exec("COMMIT");}
        if(current<10){exec("BEGIN IMMEDIATE");exec(RankedArbitrationMigration);exec("COMMIT");}
        if(current<11){exec("BEGIN IMMEDIATE");exec(SocialMigration);exec("COMMIT");}
        if(current<12){exec("BEGIN IMMEDIATE");exec(AvatarMigration);exec("COMMIT");}
        if(current<13){exec("BEGIN IMMEDIATE");exec(SessionRemovalMigration);exec("COMMIT");}
        if(current<14){exec("BEGIN IMMEDIATE");exec(AvatarFeatureMigration);exec("COMMIT");}
        if(current<15){exec("BEGIN IMMEDIATE");exec(PresenceStatusMigration);exec("COMMIT");}
        if(current<16){exec("BEGIN IMMEDIATE");exec(HostMigrationMigration);exec("COMMIT");}
    } catch (...) { sqlite3_close(db_); db_=nullptr; throw; }
}
Store::~Store() { sqlite3_close(db_); }
sqlite3* Store::db() const { return db_; }
void Store::exec(const char* sql) {
    if (sqlite3_exec(db_,sql,nullptr,nullptr,nullptr)!=SQLITE_OK) throw Error("INTERNAL_ERROR");
}
void Store::title(const std::string& id,const std::string& name) {
    if (!identifier(id) || name.empty() || name.size()>128) throw Error("INVALID_ARGUMENT");
    Statement s(db_,"INSERT INTO titles(id,name) VALUES(?,?)"); s.bind(1,id);s.bind(2,name);(void)s.row();
}
std::string Store::user(const std::string& username,const std::string& password,const std::string& gamertag) {
    if (!identifier(username) || !identifier(gamertag) || gamertag.size()>32 || password.size()<8 || password.size()>256)
        throw Error("INVALID_ARGUMENT");
    const auto id=randomHex(16), salt=randomHex(16), verifier=passwordHash(password,salt);
    Statement s(db_,"INSERT INTO users(id,username,gamertag,salt,verifier) VALUES(?,?,?,?,?)");
    s.bind(1,id);s.bind(2,username);s.bind(3,gamertag);s.bind(4,salt);s.bind(5,verifier);(void)s.row();return id;
}
void Store::achievement(const std::string& game,const Json& a) {
    const auto key=stringField(a,"key",64);
    if (!identifier(game)||!identifier(key)||!a.contains("score")||!a["score"].is_number_integer()||a["score"]<0||a["score"]>1000)
        throw Error("INVALID_ARGUMENT");
    Statement s(db_,"INSERT INTO achievements(game_id,key,name,description,how_to_earn,score,display,picture) VALUES(?,?,?,?,?,?,?,?)");
    s.bind(1,game);s.bind(2,key);s.bind(3,stringField(a,"name",128));s.bind(4,stringField(a,"description",1024));
    s.bind(5,stringField(a,"howToEarn",1024));s.bind(6,a["score"].get<long long>());s.bind(7,a.value("display",true)?1LL:0LL);
    const auto picture=a.value("picture",std::string{});
    if(!picture.empty()) {
        Statement resource(db_,"SELECT 1 FROM title_assets WHERE game_id=? AND hash=?");resource.bind(1,game);resource.bind(2,picture);
        if(!resource.row())throw Error("NOT_FOUND");
    }
    s.bind(8,picture);(void)s.row();
}
std::string Store::asset(const std::string& game,const std::string& mime,std::string_view bytes) {
    if(!identifier(game)||bytes.empty()||bytes.size()>16777216)throw Error("INVALID_ARGUMENT");
    if(mime!="image/png"&&mime!="model/gltf-binary")throw Error("INVALID_ARGUMENT");
    if(mime=="image/png"&&(bytes.size()<24||bytes.substr(0,8)!=std::string_view("\x89PNG\r\n\x1a\n",8)))throw Error("INVALID_ARGUMENT");
    if(mime=="image/png") {
        auto integer=[&](std::size_t offset){unsigned int value=0;for(std::size_t i=offset;i<offset+4;++i)value=(value<<8)|static_cast<unsigned char>(bytes[i]);return value;};
        if(bytes.size()>524288||bytes.substr(12,4)!="IHDR"||integer(8)!=13||integer(16)<1||integer(16)>512||integer(20)<1||integer(20)>512)
            throw Error("INVALID_ARGUMENT");
    }
    if(mime=="model/gltf-binary") {
        if(bytes.size()<12||bytes.substr(0,4)!="glTF")throw Error("INVALID_ARGUMENT");
        auto integer=[&](std::size_t offset){unsigned int value=0;for(int i=3;i>=0;--i)value=(value<<8)|static_cast<unsigned char>(bytes[offset+i]);return value;};
        if(integer(4)!=2||integer(8)!=bytes.size())throw Error("INVALID_ARGUMENT");
    }
    const auto hash=sha256(bytes);
    exec("BEGIN IMMEDIATE");
    try {
        Statement insert(db_,"INSERT OR IGNORE INTO assets(hash,mime,size,bytes) VALUES(?,?,?,?)");
        insert.bind(1,hash);insert.bind(2,mime);insert.bind(3,static_cast<long long>(bytes.size()));insert.blob(4,bytes);(void)insert.row();
        Statement title(db_,"INSERT OR IGNORE INTO title_assets(game_id,hash) VALUES(?,?)");title.bind(1,game);title.bind(2,hash);(void)title.row();exec("COMMIT");
    }catch(...){exec("ROLLBACK");throw;}
    return hash;
}
void Store::picture(const std::string& username,const std::string& hash) {
    Statement asset(db_,"SELECT 1 FROM assets WHERE hash=? AND mime IN ('image/png','image/jpeg')");asset.bind(1,hash);
    if(!asset.row())throw Error("NOT_FOUND");
    Statement update(db_,"UPDATE users SET picture=? WHERE username=?");update.bind(1,hash);update.bind(2,username);(void)update.row();
    if(sqlite3_changes(db_)!=1)throw Error("NOT_FOUND");
}

}
