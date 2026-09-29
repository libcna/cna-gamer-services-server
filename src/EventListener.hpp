// SPDX-License-Identifier: MIT
#pragma once
#include "RelayListener.hpp"
#include <set>
#include <string>
namespace CnaService {
/** @brief URL path of the account event channel (a WebSocket upgrade). */
inline constexpr std::string_view EventsPath="/cna/v1/events";
/** @brief One connected event channel; offers coalesce. */
class EventChannel {
public:
    virtual ~EventChannel()=default;
    /** @brief Queues a topic; repeated topics before the next write are sent once. @param topic Topic. */
    virtual void offer(const std::string& topic)=0;
};
/** @brief Live event channels by account, across titles. */
class EventHub {
public:
    /** @brief Registers a channel. @param user Account. @param channel Channel.
     * @return Whether it fits (8 per account, 4096 in all). */
    bool attach(const std::string& user,const std::shared_ptr<EventChannel>& channel);
    /** @brief Removes exactly this channel. @param user Account. @param channel Identity. */
    void detach(const std::string& user,const EventChannel* channel);
    /** @brief Tells every channel of an account that a topic changed. @param user Account.
     * @param topic Topic. */
    void notify(const std::string& user,const std::string& topic);
    /** @brief Counts attached channels. @return Channels. */
    std::size_t size();
private:
    std::mutex mutex_;
    std::multimap<std::string,std::weak_ptr<EventChannel>> channels_;
};
/** @brief Serves an event channel over explicit insecure numeric loopback (development only).
 * @param stream Connection. @param request Upgrade. @param service Authority. @param hub Channels.
 * @param workers Storage pool. @param authenticated Called once the channel is attached. */
boost::asio::awaitable<void> serveEvents(RelayTcp stream,RelayRequest request,Service& service,EventHub& hub,
    boost::asio::thread_pool& workers,std::function<void()> authenticated);
/** @brief Serves an event channel over TLS. @param stream Connection. @param request Upgrade.
 * @param service Authority. @param hub Channels. @param workers Storage pool.
 * @param authenticated Called once the channel is attached. */
boost::asio::awaitable<void> serveEvents(RelayTls stream,RelayRequest request,Service& service,EventHub& hub,
    boost::asio::thread_pool& workers,std::function<void()> authenticated);
}
