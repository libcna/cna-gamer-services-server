// SPDX-License-Identifier: MIT
#pragma once
#include "CnaService/Protocol.hpp"
#include "CnaService/RelayProtocol.hpp"
#include <chrono>
#include <deque>
#include <mutex>
#include <optional>

namespace CnaService {
/** @brief Parsed encrypted first-message credentials; never log or persist. */
struct RelayHello { std::string id,game,ticket; };
/** @brief Validates exact authentication fields and limits. @param bytes Text message.
 * @return Bounded ephemeral credentials. */
RelayHello parseRelayHello(std::string_view bytes);
/** @brief Bounded per-connection ingress window, including peer control frames. */
class RelayRate {
public:
    /** @brief Checks ingress without allocating. @param bytes Encoded bytes.
     * @param time Monotonic timestamp. @return Whether limits permit it. */
    bool accept(std::size_t bytes,std::chrono::steady_clock::time_point time);
private:
    std::chrono::steady_clock::time_point start_{};
    std::size_t messages_=0,bytes_=0;
};
/** @brief Cross-strand queue; capacity includes the active write and pending notifications. */
class RelayQueue {
public:
    enum class Result { Accepted,Wake,Full,Closed };
    /** @brief Reserves bounded storage before posting to an executor.
     * @param frame Encoded frame. @return Wake only for the first pending notification. */
    Result push(std::vector<unsigned char> frame);
    /** @brief Takes a frame while retaining its capacity reservation through async write.
     * @return Next owned frame, or empty and notification released. */
    std::optional<std::vector<unsigned char>> take();
    /** @brief Releases completed write capacity. @param bytes Frame size. */
    void complete(std::size_t bytes);
    /** @brief Permanently refuses new messages and clears pending frames. */
    void close();
private:
    std::mutex mutex_;
    std::deque<std::vector<unsigned char>> queue_;
    std::size_t count_=0,bytes_=0;
    bool notified_=false,closed_=false;
};
}
