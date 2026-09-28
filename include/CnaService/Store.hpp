// SPDX-License-Identifier: MS-PL
#pragma once
#include "CnaService/Protocol.hpp"
#include <sqlite3.h>
#include <string>
#include <string_view>

namespace CnaService {
/** @brief RAII prepared statement used only with fixed SQL. */
class Statement {
public:
    /** @brief Prepares fixed SQL. @param db Database. @param sql SQL literal. */
    Statement(sqlite3* db, const char* sql);
    /** @brief Finalizes the statement. */
    ~Statement();
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    /** @brief Binds text. @param index One-based index. @param value Value. */
    void bind(int index, std::string_view value);
    /** @brief Binds bounded binary content. @param index Index. @param value Bytes. */
    void blob(int index, std::string_view value);
    /** @brief Binds an integer. @param index One-based index. @param value Value. */
    void bind(int index, long long value);
    /** @brief Advances. @return Whether a row exists. */
    bool row();
    /** @brief Reads text. @param index Zero-based column. @return Text. */
    std::string text(int index) const;
    /** @brief Reads bounded binary content. @param index Column. @return Bytes. */
    std::string blob(int index) const;
    /** @brief Reads an integer. @param index Zero-based column. @return Value. */
    long long number(int index) const;
private:
    sqlite3_stmt* statement_ = nullptr;
};
/** @brief Persistent service database with transactional versioned migration. */
class Store {
public:
    /** @brief Opens and migrates. @param path Database location. */
    explicit Store(const std::string& path);
    /** @brief Closes the database. */
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    /** @brief Returns database for prepared queries. @return SQLite connection. */
    sqlite3* db() const;
    /** @brief Executes fixed trusted SQL. @param sql Fixed SQL. */
    void exec(const char* sql);
    /** @brief Provisions a title. @param id Stable title ID. @param name Display name. */
    void title(const std::string& id, const std::string& name);
    /** @brief Provisions a user. @param username Login. @param password Password.
     * @param gamertag Public name. @return User identifier. */
    std::string user(const std::string& username, const std::string& password, const std::string& gamertag);
    /** @brief Defines title achievement metadata. @param game Title. @param definition Metadata. */
    void achievement(const std::string& game, const Json& definition);
    /** @brief Imports immutable content for a title. @param game Title. @param mime Type.
     * @param bytes Content. @return SHA-256 identifier. */
    std::string asset(const std::string& game,const std::string& mime,std::string_view bytes);
    /** @brief Associates user picture. @param username Account. @param hash Existing asset hash. */
    void picture(const std::string& username,const std::string& hash);
private:
    sqlite3* db_ = nullptr;
};
/** @brief Generates secure random hex bytes. @param bytes Count. @return Hex encoding. */
std::string randomHex(std::size_t bytes);
/** @brief SHA-256 hashes bytes. @param bytes Input. @return Lowercase hex digest. */
std::string sha256(std::string_view bytes);
/** @brief Derives a password verifier via OpenSSL scrypt. @param password Secret.
 * @param salt Random salt. @return Verifier. */
std::string passwordHash(std::string_view password, std::string_view salt);
/** @brief Returns Unix seconds. @return Time. */
long long now();
}
