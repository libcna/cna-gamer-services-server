// SPDX-License-Identifier: MIT
#include "CnaService/QosProtocol.hpp"
#include "CnaService/RelayProtocol.hpp"
#include "QosHub.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace CnaService;
struct QosHubTestAccess {
  static QosBinding binding(const std::string &game, const std::string &session,
                            const std::string &host, std::uint64_t epoch,
                            const std::string &actor, std::uint64_t generation,
                            std::chrono::steady_clock::time_point end) {
    QosBinding b;
    b.game = game;
    b.session = session;
    b.host = host;
    b.epoch = epoch;
    b.actor = actor;
    b.generation = generation;
    b.deadline = end;
    b.probe = std::string(31, '0') + static_cast<char>('0' + generation);
    return b;
  }
  static QosBinding withTitle(QosBinding b, const std::string &t) {
    b.game = t;
    return b;
  }
  static QosBinding withSession(QosBinding b, const std::string &s) {
    b.session = s;
    return b;
  }
  static std::vector<unsigned char> ping(const QosBinding &b, unsigned seq,
                                         std::uint64_t nonce) {
    return qosFrame(QosType::Ping, relayMachineId(b.probe), seq, nonce);
  }
};
namespace {
void check(bool v, const char *why) {
  if (!v)
    throw std::runtime_error(why);
}
struct Sink final : QosChannel {
  std::vector<std::vector<unsigned char>> frames;
  bool offer(std::vector<unsigned char> frame) override {
    frames.push_back(std::move(frame));
    return true;
  }
};
} // namespace
int main() {
  try {
    using namespace std::chrono;
    steady_clock::time_point now{};
    QosHub hub([&] { return now; });
    auto host = std::make_shared<Sink>(), searcher = std::make_shared<Sink>(),
         stranger = std::make_shared<Sink>();
    // Server-only immutable bindings; constructor arguments are never
    // network-request authority.
    auto binding = QosHubTestAccess::binding("one", "session", "host", 7, "bob",
                                             1, now + seconds(3));
    check(hub.attachHost(binding, host), "bound current host");
    check(hub.open(binding, searcher), "searcher opens only returned probe");
    check(host->frames.size() == 1 &&
              parseQosFrame(host->frames[0]).type == QosType::Begin,
          "Begin delivered only to host");
    host->frames.clear();
    auto ping = QosHubTestAccess::ping(binding, 0, 42);
    now += milliseconds(10);
    check(hub.fromSearcher(binding, searcher.get(), ping), "first relay leg");
    check(searcher->frames.empty(),
          "relay cannot echo a successful host probe");
    check(host->frames.size() == 1, "only actual host receives ping");
    now += milliseconds(
        35); // host leg includes15ms controlled delay beyond20ms baseline
    auto pong = host->frames[0];
    pong[4] = 2;
    host->frames.clear();
    now += milliseconds(35);
    check(hub.fromHost(binding, host.get(), pong), "return host leg");
    now += milliseconds(10);
    check(searcher->frames.size() == 1, "four-leg90ms host response");
    check(duration_cast<milliseconds>(now.time_since_epoch()).count() == 90,
          "hand-derived path time");
    check(!hub.fromHost(binding, stranger.get(), pong),
          "channel source cannot be forged");
    check(!hub.fromHost(binding, host.get(), pong),
          "duplicate nonce response refused");
    auto wrong = ping;
    wrong[8] ^= 1;
    check(!hub.fromSearcher(binding, searcher.get(), wrong), "wrong probe id");
    check(!hub.fromSearcher(binding, searcher.get(), pong), "wrong role type");
    check(stranger->frames.empty(), "no broadcast or arbitrary target");
    check(hub.cancel(binding, searcher.get()), "cancel exact job");
    const auto count = searcher->frames.size();
    check(!hub.fromHost(binding, host.get(), pong), "late host traffic");
    check(searcher->frames.size() == count && hub.active() == 0,
          "cancel releases all job resources");
    auto exp = QosHubTestAccess::binding("one", "session", "host", 7, "bob", 2,
                                         now + seconds(3));
    check(hub.open(exp, searcher), "another authorized probe");
    now += seconds(3);
    hub.expire();
    check(hub.active() == 0 &&
              !hub.fromSearcher(exp, searcher.get(),
                                QosHubTestAccess::ping(exp, 0, 99)),
          "expiry boundary");
    // Bounded queue accounts for active async write, with no executor-post
    // growth.
    QosQueue queue;
    std::vector<unsigned char> frame(1200, 0);
    for (int i = 0; i < 16; ++i)
      check(queue.offer(frame), "capacity sixteen");
    auto active = queue.take();
    check(active && active->size() == 1200, "active write owns bytes");
    check(!queue.offer(frame), "take cannot release active capacity");
    queue.complete(1200);
    check(queue.offer(frame), "completed write releases exactly one frame");
    auto isolated = QosHubTestAccess::binding("one", "session", "host", 7,
                                              "bob", 3, now + seconds(3));
    auto alien = QosHubTestAccess::withTitle(isolated, "two");
    check(hub.open(isolated, searcher), "isolated probe");
    const auto deliveries = host->frames.size();
    check(!hub.fromSearcher(alien, searcher.get(),
                            QosHubTestAccess::ping(isolated, 0, 43)),
          "cross-title binding refused");
    auto otherSession =
        QosHubTestAccess::withSession(isolated, "another-session");
    check(!hub.fromSearcher(otherSession, searcher.get(),
                            QosHubTestAccess::ping(isolated, 0, 43)),
          "cross-session binding refused");
    check(host->frames.size() == deliveries,
          "foreign authority has zero delivery");
    hub.revokeHost("one", "session", "host", 7);
    check(hub.active() == 0 &&
              !hub.fromSearcher(isolated, searcher.get(),
                                QosHubTestAccess::ping(isolated, 0, 43)),
          "host epoch revoked");
    auto live = QosHubTestAccess::binding("one", "session", "host", 8, "bob", 4,
                                          now + seconds(3));
    check(hub.attachHost(live, host) && hub.open(live, searcher),
          "new host epoch owns fresh probe");
    // Literal policy:32 ingress messages including controls for the entire job.
    for (int i = 0; i < 32; ++i)
      check(hub.control(live, searcher.get()), "whole-job ingress budget");
    check(!hub.control(live, searcher.get()),
          "33rd control frame refused without unbounded work");
    std::cout << "prejoin route/clock/queue assertions passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
