// SPDX-License-Identifier: MIT
#include "CnaService/Service.hpp"
#include "QosTestAccess.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
using namespace CnaService;
namespace {
void check(bool value, const char *why) {
  if (!value)
    throw std::runtime_error(why);
}
struct Fixture {
  std::filesystem::path path =
      std::filesystem::current_path() / "qos-authority-unit.sqlite3";
  std::unique_ptr<Service> service;
  std::array<Json, 4> credentials;
  std::chrono::steady_clock::time_point time{};
  Json host, find;
  std::string other;
  Fixture() {
    clean();
    {
      Store store(path.string());
      store.title("one", "One");
      store.title("two", "Two");
      for (const auto *name : {"alice", "bob", "carol", "dana"})
        store.user(name, std::string(name) + "-password", name);
    }
    service = std::make_unique<Service>(path.string());
    QosTestAccess::clock(*service, [this] { return time; });
    int index = 0;
    for (const auto *name : {"alice", "bob", "carol", "dana"}) {
      auto login =
          call("auth.login", {{"username", name},
                              {"password", std::string(name) + "-password"}});
      check(login["error"] == "OK", "fixture login");
      credentials[index++] = login["result"];
    }
    other =
        call("auth.login", {{"username", "bob"}, {"password", "bob-password"}},
             {}, "two")["result"]["token"]
            .get<std::string>();
    Json properties = Json::array(
        {73, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr});
    auto created = call("sessions.create",
                        {{"kind", "player"},
                         {"maxGamers", 8},
                         {"privateSlots", 0},
                         {"properties", properties},
                         {"allowJoinInProgress", false},
                         {"participants", Json::array({token(0)})}},
                        token(0));
    check(created["error"] == "OK", "host creation");
    host = created["result"];
    find = {{"kind", "player"},
            {"localCount", 1},
            {"properties", properties},
            {"start", 0},
            {"limit", 32}};
  }
  ~Fixture() {
    service.reset();
    clean();
  }
  void clean() {
    for (const auto *suffix : {"", "-wal", "-shm"})
      std::filesystem::remove(path.string() + suffix);
  }
  std::string token(int index) const {
    return credentials[index]["token"].get<std::string>();
  }
  Json call(const std::string &op, const Json &args,
            const std::string &auth = {}, const std::string &game = "one") {
    static int sequence = 0;
    return parse(
        service->handle(Json{{"v", 1},
                             {"id", "qos-" + std::to_string(++sequence)},
                             {"op", op},
                             {"args", args},
                             {"token", auth},
                             {"game", game}}
                            .dump(),
                        "qos-authority-test"));
  }
  std::string permit() {
    auto response = call("sessions.findQos", find, token(1));
    check(response["error"] == "OK", "findQos supported");
    check(response["result"]["sessions"].size() == 1, "one returned listing");
    auto row = response["result"]["sessions"][0];
    check(row["listing"]["session"] == host["session"],
          "permit listing binding");
    check(row["lifetimeMs"] == 30000, "literal permit policy lifetime");
    auto value = row["permit"].get<std::string>();
    check(value.size() == 64, "256-bit opaque permit");
    check(std::all_of(value.begin(), value.end(),
                      [](char c) {
                        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                      }),
          "permit syntax");
    return value;
  }
};
void PermitOnlyForReturnedListingAndActor() {
  Fixture f;
  auto plain = f.call("sessions.find", f.find, f.token(1));
  check(plain["error"] == "OK", "old find");
  check(!plain["result"]["sessions"][0].contains("permit"),
        "old shape unchanged");
  auto mismatch = f.find;
  mismatch["properties"][0] = 74;
  auto none = f.call("sessions.findQos", mismatch, f.token(1));
  check(none["error"] == "OK" && none["result"]["sessions"].empty(),
        "no permit for unmatched row");
  const auto permit = f.permit();
  check(f.call("sessions.qosTicket", {{"permit", permit}},
               f.token(2))["error"] == "NOT_AUTHORIZED",
        "foreign actor");
  check(f.call("sessions.qosTicket", {{"permit", permit}}, f.other,
               "two")["error"] == "NOT_AUTHORIZED",
        "foreign title");
  check(f.call("sessions.qosTicket", {{"permit", std::string(64, 'a')}},
               f.token(1))["error"] == "NOT_AUTHORIZED",
        "invented permit");
  check(f.call("sessions.qosTicket",
               {{"permit", permit}, {"session", f.host["session"]}},
               f.token(1))["error"] == "INVALID_ARGUMENT",
        "no target override");
  auto issued = f.call("sessions.qosTicket", {{"permit", permit}}, f.token(1));
  check(issued["error"] == "OK", "right actor receives ticket");
  check(issued["result"]["ticket"].get<std::string>().size() == 64 &&
            issued["result"]["probe"].get<std::string>().size() == 32,
        "bounded ticket/id");
  check(issued["result"]["lifetimeMs"] == 5000, "ticket policy lifetime");
  check(f.call("sessions.qosTicket", {{"permit", permit}},
               f.token(1))["error"] == "NOT_AUTHORIZED",
        "one use");
}
void NoMembershipOrGameAuthority() {
  Fixture f;
  const auto before = f.call("sessions.get", {{"session", f.host["session"]}},
                             f.token(0))["result"];
  const auto p = f.permit();
  auto ticket = f.call("sessions.qosTicket", {{"permit", p}}, f.token(1));
  check(ticket["error"] == "OK", "probe issuance");
  const auto after = f.call("sessions.get", {{"session", f.host["session"]}},
                            f.token(0))["result"];
  check(before["members"] == after["members"] &&
            before["currentGamers"] == after["currentGamers"] &&
            before["revision"] == after["revision"],
        "no membership/reservation side effects");
  check(f.call("sessions.get", {{"session", f.host["session"]}},
               f.token(1))["error"] == "NOT_AUTHORIZED",
        "no roster");
  check(f.call("sessions.relayTicket",
               {{"session", f.host["session"]},
                {"participants", Json::array({f.token(1)})}},
               f.token(1))["error"] == "NOT_AUTHORIZED",
        "no game grant");
  bool refused = false;
  try {
    (void)f.service->redeemRelayTicket(
        "one", ticket["result"]["ticket"].get<std::string>());
  } catch (const Error &e) {
    refused = e.code() == "UNAUTHENTICATED";
  }
  check(refused, "probe credential is never a relay credential");
  for (const auto *key : {"members", "machine", "hostMachine", "endpoint",
                          "token", "refreshToken"})
    check(!ticket["result"].contains(key),
          "no secret roster/routing disclosure");
}
void RevisionAndPrivacyRevocation() {
  for (const std::string reason :
       {"host-left", "disabled", "revoked", "blocked", "avoid", "migrated"}) {
    Fixture f;
    const auto permit = f.permit();
    if (reason == "host-left")
      check(f.call("sessions.leave", {{"session", f.host["session"]}},
                   f.token(0))["error"] == "OK",
            "leave");
    else if (reason == "disabled") {
      Store s(f.path.string());
      s.exec("UPDATE users SET online_allowed=0 WHERE username='bob'");
    } else if (reason == "revoked") {
      Store s(f.path.string());
      Statement revoke(s.db(),
                       "UPDATE refresh_families SET revoked=1 WHERE user_id=?");
      revoke.bind(1, f.credentials[1]["identity"]["userId"].get<std::string>());
      (void)revoke.row();
    } else if (reason == "blocked")
      QosTestAccess::block(*f.service, f.credentials[0]["identity"]["userId"],
                           f.credentials[1]["identity"]["userId"]);
    else if (reason == "avoid")
      QosTestAccess::avoid(*f.service, f.credentials[1]["identity"]["userId"],
                           f.credentials[0]["identity"]["userId"]);
    else
      QosTestAccess::advanceHostEpoch(*f.service,
                                      f.host["session"].get<std::string>());
    const auto error =
        f.call("sessions.qosTicket", {{"permit", permit}}, f.token(1))["error"];
    check(error == "NOT_AUTHORIZED" ||
              (reason == "revoked" && error == "UNAUTHENTICATED"),
          "current privacy/epoch/family gates");
  }
}
void BoundedIssuanceAndRestart() {
  Fixture f;
  const auto permit = f.permit();
  f.time += std::chrono::milliseconds(30000);
  check(f.call("sessions.qosTicket", {{"permit", permit}},
               f.token(1))["error"] == "NOT_AUTHORIZED",
        "expiry at exact30s");
  const auto after = f.permit();
  f.service.reset();
  f.service = std::make_unique<Service>(f.path.string());
  QosTestAccess::clock(*f.service, [&f] { return f.time; });
  check(f.call("sessions.qosTicket", {{"permit", after}},
               f.token(1))["error"] == "NOT_AUTHORIZED",
        "restart discards authority");
  // Pure authority seam seeds32 actually-returned entries without32 slow
  // credential derivations.
  const auto permits =
      QosTestAccess::seedReturnedPermits(*f.service, f.token(1), 32);
  check(permits.size() == 32 &&
            QosTestAccess::permitCount(*f.service, f.token(1)) == 32,
        "literal actor permit cap");
  check(f.call("sessions.findQos", f.find, f.token(1))["error"] ==
            "LIMIT_EXCEEDED",
        "33rd permit refused atomically");
  std::vector<std::string> probes;
  for (int i = 0; i < 4; ++i) {
    auto issued =
        f.call("sessions.qosTicket", {{"permit", permits[i]}}, f.token(1));
    check(issued["error"] == "OK", "four actor/host job slots");
    probes.push_back(issued["result"]["probe"]);
  }
  check(QosTestAccess::activeCount(*f.service, f.token(1)) == 4,
        "active cap exact");
  check(f.call("sessions.qosTicket", {{"permit", permits[4]}},
               f.token(1))["error"] == "LIMIT_EXCEEDED",
        "fifth concurrent probe refused");
  QosTestAccess::cancel(*f.service, probes[0]);
  check(QosTestAccess::activeCount(*f.service, f.token(1)) == 3,
        "cancellation releases one");
  check(f.call("sessions.qosTicket", {{"permit", permits[4]}},
               f.token(1))["error"] == "OK",
        "refusal did not consume permit");
  f.time += std::chrono::milliseconds(5000);
  QosTestAccess::expire(*f.service);
  check(QosTestAccess::activeCount(*f.service, f.token(1)) == 0,
        "unused connection tickets release at5s");
}
void GrantRevalidationAndNoLeaseRenewal() {
  Fixture f;
  const auto permit = f.permit();
  const auto issued =
      f.call("sessions.qosTicket", {{"permit", permit}}, f.token(1));
  check(issued["error"] == "OK", "grant fixture");
  auto lease = [&] {
    Store store(f.path.string());
    Statement q(store.db(),
                "SELECT expires FROM directory_machines WHERE id=?");
    q.bind(1, f.host["machine"].get<std::string>());
    check(q.row(), "host lease");
    return q.number(0);
  };
  const auto before = lease();
  const auto grant =
      f.service->redeemQosTicket("one", issued["result"]["ticket"]);
  check(f.service->validateQosGrant(grant), "redeemed search authority valid");
  check(lease() == before, "probe cannot renew member lease");
  auto forged = grant;
  forged.actor = f.credentials[2]["identity"]["userId"];
  check(!f.service->validateQosGrant(forged), "forged actor");
  f.service->releaseQosGrant(forged);
  check(f.service->validateQosGrant(grant),
        "foreign release does not remove real grant");
  QosTestAccess::advanceHostEpoch(*f.service, grant.session);
  check(!f.service->validateQosGrant(grant), "active host epoch revoked");
  f.service->releaseQosGrant(grant);
  check(QosTestAccess::activeCount(*f.service, f.token(1)) == 0,
        "exact grant released");
}
void HostRequiresCompleteLiveMemberAuthority() {
  Fixture f;
  const auto session = f.host["session"].get<std::string>();
  auto args =
      Json{{"session", session}, {"participants", Json::array({f.token(0)})}};
  check(f.call("sessions.qosHostTicket", args, f.token(0))["error"] ==
            "NOT_AUTHORIZED",
        "no live member relay");
  const auto relay = f.call("sessions.relayTicket", args, f.token(0));
  check(relay["error"] == "OK", "member relay fixture");
  const auto gameGrant =
      f.service->redeemRelayTicket("one", relay["result"]["ticket"]);
  auto credential = f.call("sessions.qosHostTicket", args, f.token(0));
  check(credential["error"] == "OK", "current host may respond");
  const auto secret = credential["result"]["ticket"].get<std::string>();
  const auto host = f.service->redeemQosTicket("one", secret);
  check(host.hostRole && host.probe.empty() &&
            f.service->validateQosGrant(host),
        "member responder authority, no search route");
  check(f.call("sessions.qosHostTicket", args, f.token(0))["error"] ==
            "LIMIT_EXCEEDED",
        "one host responder per session");
  bool replay = false;
  try {
    (void)f.service->redeemQosTicket("one", secret);
  } catch (const Error &e) {
    replay = e.code() == "UNAUTHENTICATED";
  }
  check(replay, "one-use host ticket");
  check(f.call("sessions.addMembers",{{"session",session},{"participants",Json::array({f.token(3)})}},f.token(0))["error"]=="OK","host group grows");
  check(!f.service->validateQosGrant(host),"responder must reacquire the complete grown group");
  f.service->releaseQosGrant(host);
  check(f.call("sessions.qosHostTicket",args,f.token(0))["error"]=="NOT_AUTHORIZED","partial grown group refused");
  args["participants"]=Json::array({f.token(0),f.token(3)});
  credential=f.call("sessions.qosHostTicket",args,f.token(0));check(credential["error"]=="OK","complete grown group accepted");
  const auto grown=f.service->redeemQosTicket("one",credential["result"]["ticket"]);
  check(f.service->validateQosGrant(grown),"fresh group responder valid");
  {Store store(f.path.string());store.exec("UPDATE users SET online_allowed=0 WHERE username='dana'");}
  check(!f.service->validateQosGrant(grown),"secondary participant privilege loss revokes responder");
  f.service->releaseQosGrant(grown);
  f.service->releaseRelayGrant(gameGrant);
  check(!f.service->validateQosGrant(host),
        "game grant loss revokes responder");
  f.service->releaseQosGrant(host);
}
void RedeemCannotExtendFiveSecondReservation() {
  Fixture f;const auto permit=f.permit();
  const auto issued=f.call("sessions.qosTicket",{{"permit",permit}},f.token(1));check(issued["error"]=="OK","deadline fixture issuance");
  f.time+=std::chrono::milliseconds(4999);
  const auto grant=f.service->redeemQosTicket("one",issued["result"]["ticket"]);
  check(f.service->validateQosGrant(grant),"ticket still live just before original deadline");
  f.time+=std::chrono::milliseconds(1);
  check(!f.service->validateQosGrant(grant),"redeeming cannot restart the five-second reservation clock");
}

void OperationBudgetIsAccountIsolated() {
  Fixture f;
  for (int i = 0; i < 128; ++i)
    check(f.call("sessions.qosTicket", {{"permit", std::string(64, 'a')}},
                 f.token(1))["error"] == "NOT_AUTHORIZED",
          "bounded invalid attempts");
  check(f.call("sessions.qosTicket", {{"permit", std::string(64, 'a')}},
               f.token(1))["error"] == "RATE_LIMITED",
        "129th actor request refused");
  check(f.call("sessions.findQos", f.find, f.token(2))["error"] == "OK",
        "another actor keeps own budget");
  f.time += std::chrono::seconds(60);
  check(f.call("sessions.findQos", f.find, f.token(1))["error"] == "OK",
        "rolling budget expires");
}

} // namespace
int main() {
  try {
    RedeemCannotExtendFiveSecondReservation();PermitOnlyForReturnedListingAndActor();
    NoMembershipOrGameAuthority();
    RevisionAndPrivacyRevocation();
    BoundedIssuanceAndRestart();
    GrantRevalidationAndNoLeaseRenewal();
    HostRequiresCompleteLiveMemberAuthority();
    OperationBudgetIsAccountIsolated();
    std::cout << "prejoin probe authority assertions passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
