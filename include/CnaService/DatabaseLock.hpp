// SPDX-License-Identifier: MIT
#pragma once
#include <string>

namespace CnaService {
/** @brief Exclusive ownership of a database by one server process: an operating-system lock on
 * `<database>.lock` (flock on POSIX, a file opened with no sharing on Windows), held while this
 * object lives and released by the system when the process ends, however it ends. */
class DatabaseLock {
public:
    /** @brief The outcome of taking the lock. */
    enum class Result {
        /** @brief This object owns the database. */
        Owned,
        /** @brief Another holder owns it. */
        InUse,
        /** @brief The lock file could not be opened. */
        Failed
    };
    /** @brief Takes the lock without waiting. @param database Database path. */
    explicit DatabaseLock(const std::string& database);
    /** @brief Releases the lock if owned. */
    ~DatabaseLock();
    DatabaseLock(const DatabaseLock&) = delete;
    DatabaseLock& operator=(const DatabaseLock&) = delete;
    /** @brief Whether the lock was taken. @return The outcome. */
    Result result() const { return result_; }
private:
    Result result_ = Result::Failed;
#ifdef _WIN32
    void* handle_ = nullptr;
#else
    int descriptor_ = -1;
#endif
};
}
