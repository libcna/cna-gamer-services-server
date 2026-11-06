// SPDX-License-Identifier: MIT
#pragma once
#include "RelayListener.hpp"
namespace CnaService {
boost::asio::awaitable<void> serveQos(RelayTcp stream, RelayRequest request,
                                      Service &service,
                                      boost::asio::thread_pool &workers,
                                      std::function<void()> authenticated);
boost::asio::awaitable<void> serveQos(RelayTls stream, RelayRequest request,
                                      Service &service,
                                      boost::asio::thread_pool &workers,
                                      std::function<void()> authenticated);
} // namespace CnaService
