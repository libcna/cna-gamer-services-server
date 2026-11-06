// SPDX-License-Identifier: MIT
#include "QosTestAccess.hpp"
#include <stdexcept>
namespace CnaService {
void QosTestAccess::clock(
    Service &s, std::function<std::chrono::steady_clock::time_point()> c) {
  s.qos_.clock = std::move(c);
}
void QosTestAccess::block(Service &s, const std::string &h,
                          const std::string &a) {
  Statement q(s.store_.db(),
              "INSERT INTO blocks(user_id,blocked_id,created) VALUES(?,?,?)");
  q.bind(1, h);
  q.bind(2, a);
  q.bind(3, now());
  (void)q.row();
}
void QosTestAccess::avoid(Service &s, const std::string &a,
                          const std::string &h) {
  Statement q(
      s.store_.db(),
      "INSERT INTO player_reviews(reviewer_id,subject_id,rating,updated) "
      "VALUES(?,?,'avoid',?)");
  q.bind(1, a);
  q.bind(2, h);
  q.bind(3, now());
  (void)q.row();
}
void QosTestAccess::advanceHostEpoch(Service &s, const std::string &session) {
  s.invalidateQosHost(session);
}
std::vector<std::string>
QosTestAccess::seedReturnedPermits(Service &s, const std::string &token,
                                   int count) {
  Statement q(s.store_.db(),
              "SELECT user_id FROM sessions WHERE hash=? AND game_id='one'");
  q.bind(1, sha256(token));
  if (!q.row())
    throw std::runtime_error("fixture actor");
  const auto actor = q.text(0);
  Json filters{
      {"kind", "player"},
      {"localCount", 1},
      {"properties", Json::array({73, nullptr, nullptr, nullptr, nullptr,
                                  nullptr, nullptr, nullptr})},
      {"start", 0},
      {"limit", 32}};
  const auto page =
      s.qosControl(Json{{"token", token}, {"id", "seed-returned"}}, actor,
                   "one", "sessions.findQos", filters);
  const auto secret = page["sessions"][0]["permit"].get<std::string>();
  const auto p = s.qos_.permits.at(sha256(secret));
  std::vector<std::string> result{secret};
  // Seed repeated receipts of this actually returned listing to exercise the
  // lower storage cap, independently of the separate8/min control search
  // budget. Permission verdict is real.
  for (int i = 1; i < count; ++i) {
    if (s.qos_.permits.size() >= 32)
      throw std::runtime_error("fixture cap");
    const auto next = randomHex(32);
    s.qos_.permits.emplace(sha256(next), p);
    result.push_back(next);
  }
  return result;
}
std::size_t QosTestAccess::permitCount(Service &s, const std::string &token) {
  Statement q(s.store_.db(), "SELECT user_id FROM sessions WHERE hash=?");
  q.bind(1, sha256(token));
  if (!q.row())
    return 0;
  std::size_t n = 0;
  for (const auto &[id, p] : s.qos_.permits)
    if (p.binding.actor == q.text(0))
      ++n;
  return n;
}
std::size_t QosTestAccess::activeCount(Service &s, const std::string &token) {
  Statement q(s.store_.db(), "SELECT user_id FROM sessions WHERE hash=?");
  q.bind(1, sha256(token));
  if (!q.row())
    return 0;
  std::size_t n = 0;
  for (const auto &[id, t] : s.qos_.tickets)
    if (t.permit.binding.actor == q.text(0) && !t.permit.binding.hostRole)
      ++n;
  return n;
}
void QosTestAccess::cancel(Service &s, const std::string &probe) {
  for (const auto &[id, t] : s.qos_.tickets)
    if (t.permit.binding.probe == probe) {
      const auto b = t.permit.binding;
      s.releaseQosGrant(b);
      return;
    }
}
void QosTestAccess::expire(Service &s) { s.pruneQos(); }
} // namespace CnaService
