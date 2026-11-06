// SPDX-License-Identifier: MIT
#pragma once
#include "CnaService/Service.hpp"
#include <chrono>
#include <functional>
namespace CnaService {
// Phase 2 private friend implementation; never accepted from a network request.
struct QosTestAccess {
  static void clock(Service &,
                    std::function<std::chrono::steady_clock::time_point()>);
  static void block(Service &, const std::string &host,
                    const std::string &actor);
  static void avoid(Service &, const std::string &actor,
                    const std::string &host);
  static void advanceHostEpoch(Service &, const std::string &session);
  static std::vector<std::string>
  seedReturnedPermits(Service &, const std::string &actorToken, int count);
  static std::size_t permitCount(Service &, const std::string &actorToken);
  static std::size_t activeCount(Service &, const std::string &actorToken);
  static void cancel(Service &, const std::string &probe);
  static void expire(Service &);
};
} // namespace CnaService
