// SPDX-License-Identifier: MIT
#pragma once
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <utility>
namespace CnaService {
/** @brief Preauthentication connection admission and per-address rate history. */
class Admission {
public:
    /** @brief Non-destructive current/cumulative counters for diagnostics. */
    struct Snapshot { int connections=0; unsigned long long refused=0; };
    /** @brief Attempts to acquire a connection lease. @param peer Server-derived address.
     * @param now Monotonic admission time. @return Whether the lease was granted. */
    bool admit(const std::string& peer, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    /** @brief Takes open connections and recent refusals. @return Connection and refusal counts. */
    std::pair<int,unsigned long long> take();
    /** @brief Reads open connections and all refusals since startup. */
    Snapshot snapshot();
    /** @brief Releases a connection lease. @param peer Address owning the lease. */
    void leave(const std::string& peer);
private:
    std::mutex mutex_;
    std::map<std::string,int> peers_;
    std::map<std::string,std::pair<std::chrono::steady_clock::time_point,int>> rates_;
    int control_=0;
    unsigned long long refused_=0;
    unsigned long long refusedTotal_=0;
};
}
