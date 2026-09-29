// SPDX-License-Identifier: MIT
#pragma once
#include "CnaService/RelayAuthorization.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/asio/thread_pool.hpp>
#include <functional>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <vector>
namespace CnaService {
class Service;
/** @brief Private bounded delivery surface; caller never controls source identity. */
class RelayChannel {
public:
    virtual ~RelayChannel()=default;
    /** @brief Offers a frame without an unbounded executor post. @param frame Bounded bytes. */
    virtual void offer(std::vector<unsigned char> frame)=0;
};
/** @brief Live machine routing isolated by title/session, independent of directory persistence. */
class RelayHub {
public:
    /** @brief Registers one channel per machine. @param grant Server authority.
     * @param channel Live channel. @return Whether unique and within resource cap. */
    bool attach(const RelayGrant& grant,const std::shared_ptr<RelayChannel>& channel);
    /** @brief Removes only this exact channel. @param grant Authority. @param channel Identity. */
    void detach(const RelayGrant& grant,const RelayChannel* channel);
    /** @brief Routes within the same title/session. @param grant Source authority.
     * @param destination Service machine. @param frame Source-injected bytes. */
    void route(const RelayGrant& grant,const std::string& destination,std::vector<unsigned char> frame);
private:
    using Key=std::tuple<std::string,std::string,std::string>;
    std::mutex mutex_;
    std::map<Key,std::weak_ptr<RelayChannel>> channels_;
};
using RelayRequest=boost::beast::http::request<boost::beast::http::string_body>;
using RelayTcp=boost::beast::tcp_stream;
using RelayTls=boost::beast::ssl_stream<RelayTcp>;
/** @brief Serves explicit insecure numeric-loopback development only.
 * @param stream Owned connection. @param request Upgrade. @param service Authority. @param hub Routing.
 * @param workers Pool that runs the service's storage work off the network threads.
 * @param authenticated Called once the ticket was redeemed and the machine attached. */
boost::asio::awaitable<void> serveRelay(RelayTcp stream,RelayRequest request,Service& service,RelayHub& hub,
    boost::asio::thread_pool& workers,std::function<void()> authenticated);
/** @brief Serves authenticated secure relay independently of HTTPS control requests.
 * @param stream Owned TLS connection. @param request Upgrade. @param service Authority. @param hub Routing.
 * @param workers Pool that runs the service's storage work off the network threads.
 * @param authenticated Called once the ticket was redeemed and the machine attached. */
boost::asio::awaitable<void> serveRelay(RelayTls stream,RelayRequest request,Service& service,RelayHub& hub,
    boost::asio::thread_pool& workers,std::function<void()> authenticated);
}
