// SPDX-License-Identifier: MS-PL
#include "CnaService/Store.hpp"
#include "InitialMigration.hpp"
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
        Statement version(db_,"PRAGMA user_version");
        (void)version.row();
        const auto current=version.number(0);
        if (current>1) throw Error("UNSUPPORTED_DATABASE_VERSION");
        if (current==0) {
            exec("BEGIN IMMEDIATE");
            exec(InitialMigration);
            exec("COMMIT");
        }
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
    s.bind(8,a.value("picture",std::string{}));(void)s.row();
}
}
