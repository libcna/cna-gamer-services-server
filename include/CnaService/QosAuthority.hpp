// SPDX-License-Identifier: MIT
#pragma once
#include "CnaService/Protocol.hpp"
#include "CnaService/QosProtocol.hpp"
#include "CnaService/RelayAuthorization.hpp"
#include <chrono>
#include <deque>
#include <functional>
#include <map>
namespace CnaService {
/** @brief Server-owned probe route, never accepted as peer-supplied authority.
 */
struct QosBinding {
  std::string game, session, host, actor, probe, hash;
  std::uint64_t epoch = 1, generation = 1;
  std::chrono::steady_clock::time_point deadline{};
  bool hostRole = false;
  bool operator==(const QosBinding &) const = default;
};
/** @brief Process-local bounded ephemeral authority. Secrets are indexed only
 * by hashes. */
struct QosAuthorityState {
  using Time = std::chrono::steady_clock::time_point;
  struct Permit {
    QosBinding binding;
    std::string family;
    Json filters;
    Time expires;
    bool used = false;
  };
  struct Ticket {
    Permit permit;
    RelayGrant relay;
    std::vector<std::pair<std::string, std::string>> families;
    bool redeemed = false;
  };
  std::function<Time()> clock = [] { return std::chrono::steady_clock::now(); };
  std::map<std::string, Permit> permits;
  std::map<std::string, Ticket> tickets;
  std::map<std::string, std::deque<Time>> searches, operations;
  std::map<std::string, Time> requests;
  std::map<std::string, std::uint64_t> epochs;
};
} // namespace CnaService
