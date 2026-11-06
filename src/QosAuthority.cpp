// SPDX-License-Identifier: MIT
#include "CnaService/Service.hpp"
#include <algorithm>
#include <set>
namespace CnaService {
namespace {
using namespace std::chrono;
bool hex(const std::string &v, std::size_t size) {
  return v.size() == size &&
         v.find_first_not_of("0123456789abcdef") == std::string::npos;
}
std::string key(const std::string &game, const std::string &actor) {
  return game + '\n' + actor;
}
std::string family(Store &store, const std::string &token,
                   const std::string &game, const std::string &actor) {
  Statement q(store.db(),
              "SELECT f.id FROM sessions s JOIN refresh_families f ON "
              "f.id=s.refresh_family JOIN users u ON u.id=s.user_id WHERE "
              "s.hash=? AND s.game_id=? AND s.user_id=? AND s.expires>? AND "
              "f.game_id=s.game_id AND f.user_id=s.user_id AND f.revoked=0 AND "
              "f.expires>? AND u.online_allowed=1");
  q.bind(1, sha256(token));
  q.bind(2, game);
  q.bind(3, actor);
  q.bind(4, now());
  q.bind(5, now());
  if (!q.row())
    throw Error("NOT_AUTHORIZED");
  return q.text(0);
}
} // namespace
void Service::pruneQos() {
  const auto at = qos_.clock();
  std::erase_if(qos_.permits,
                [&](const auto &p) { return at >= p.second.expires; });
  std::erase_if(qos_.tickets, [&](const auto &p) {
    if(at >= p.second.permit.binding.deadline)return true;
    if(!p.second.permit.binding.hostRole)return false;
    if(!qosValid(p.second.permit)||!validateRelayGrantLocked(p.second.relay,true))return true;
    Statement group(store_.db(),"SELECT COUNT(*) FROM directory_members WHERE machine_id=? AND session_id=?");
    group.bind(1,p.second.permit.binding.host);group.bind(2,p.second.permit.binding.session);(void)group.row();
    return group.number(0)!=static_cast<long long>(p.second.families.size());
  });
  std::erase_if(qos_.requests, [&](const auto &p) { return at >= p.second; });
  for (auto i = qos_.searches.begin(); i != qos_.searches.end();) {
    auto &times = i->second;
    while (!times.empty() && at - times.front() >= std::chrono::seconds(60))
      times.pop_front();
    if (times.empty())
      i = qos_.searches.erase(i);
    else
      ++i;
  }
  for (auto i = qos_.operations.begin(); i != qos_.operations.end();) {
    auto &times = i->second;
    while (!times.empty() && at - times.front() >= std::chrono::seconds(60))
      times.pop_front();
    if (times.empty())
      i = qos_.operations.erase(i);
    else
      ++i;
  }
  // Build the live-session set once: scanning every permit for every epoch is quadratic
  // under the process cap and would stall the shared service mutex during validation.
  std::set<std::string> retained;
  for(const auto& [hash,p]:qos_.permits)retained.insert(p.binding.session);
  for(const auto& [hash,t]:qos_.tickets)retained.insert(t.permit.binding.session);
  std::erase_if(qos_.epochs,[&](const auto& entry){return !retained.contains(entry.first);});
}
void Service::invalidateQosHost(const std::string &session) {
  if (auto it = qos_.epochs.find(session); it != qos_.epochs.end())
    ++it->second;
}
bool Service::qosValid(const QosAuthorityState::Permit &p) {
  const auto &b = p.binding;
  if (qos_.clock() >= b.deadline)
    return false;
  const auto epoch = qos_.epochs.find(b.session);
  if (epoch == qos_.epochs.end() || epoch->second != b.epoch)
    return false;
  Statement f(store_.db(),
              "SELECT 1 FROM refresh_families f JOIN users u ON u.id=f.user_id "
              "WHERE f.id=? AND f.user_id=? AND f.game_id=? AND f.revoked=0 "
              "AND f.expires>? AND u.online_allowed=1");
  f.bind(1, p.family);
  f.bind(2, b.actor);
  f.bind(3, b.game);
  f.bind(4, now());
  if (!f.row())
    return false;
  Statement h(store_.db(),
              "SELECT 1 FROM directory_sessions d JOIN directory_machines m ON "
              "m.id=d.host_machine WHERE d.id=? AND d.game_id=? AND "
              "d.host_machine=? AND d.expires>? AND m.expires>?");
  h.bind(1, b.session);
  h.bind(2, b.game);
  h.bind(3, b.host);
  h.bind(4, now());
  h.bind(5, now());
  if (!h.row())
    return false;
  if (b.hostRole)
    return true;
  // Same public visibility predicates as sessions.find; no membership is
  // conferred.
  Statement q(
      store_.db(),
      "SELECT 1 FROM directory_sessions d WHERE d.id=?1 AND d.game_id=?2 AND "
      "d.kind=?3 AND (d.state='lobby' OR (d.kind='player' AND d.allow_join=1)) "
      "AND d.max_gamers-d.private_slots-(SELECT COUNT(*) FROM "
      "directory_members m WHERE m.session_id=d.id AND m.private_slot=0)>=?4 "
      "AND NOT EXISTS(SELECT 1 FROM directory_members m WHERE "
      "m.session_id=d.id AND m.user_id=?5) AND NOT EXISTS(SELECT 1 FROM "
      "player_reviews r WHERE r.reviewer_id=?5 AND r.subject_id=d.host_id AND "
      "r.rating='avoid') AND NOT EXISTS(SELECT 1 FROM blocks b WHERE "
      "(b.user_id=?5 AND b.blocked_id=d.host_id) OR (b.user_id=d.host_id AND "
      "b.blocked_id=?5)) AND NOT EXISTS(SELECT 1 FROM json_each(?6) p WHERE "
      "p.value IS NOT NULL AND (json_extract(d.properties,'$['||p.key||']') IS "
      "NULL OR json_extract(d.properties,'$['||p.key||']')!=p.value))");
  q.bind(1, b.session);
  q.bind(2, b.game);
  q.bind(3, p.filters["kind"].get<std::string>());
  q.bind(4, p.filters["localCount"].get<long long>());
  q.bind(5, b.actor);
  q.bind(6, p.filters["properties"].dump());
  if(!q.row())return false;
  Statement mode(store_.db(),"SELECT guest_protocol FROM directory_sessions WHERE id=?");mode.bind(1,b.session);
  if(!mode.row()||(!p.filters.value("guestAware",false)&&mode.number(0)!=0))return false;
  return mode.number(0)==0||guestVisible(b.actor,b.session);
}
Json Service::qosControl(const Json &request, const std::string &actor,
                         const std::string &game, const std::string &operation,
                         const Json &args) {
  using namespace std::chrono;
  const bool guestAware=operation.ends_with("Guests");
  const auto op=guestAware?operation.substr(0,operation.size()-6):operation;
  pruneDirectory();
  pruneQos();
  const auto at = qos_.clock();
  const auto fam =
      family(store_, stringField(request, "token", 128), game, actor);
  const auto who = key(game, actor);
  if (!qos_.operations.contains(who) && qos_.operations.size() >= 8192)
    throw Error("LIMIT_EXCEEDED");
  auto &calls = qos_.operations[who];
  if (calls.size() >= 128)
    throw Error("RATE_LIMITED");
  calls.push_back(at);
  const auto req = game + '\n' + stringField(request, "id", 64);
  if (qos_.requests.contains(req))
    throw Error("DUPLICATE_REQUEST");
  if (qos_.requests.size() >= 8192)
    throw Error("LIMIT_EXCEEDED");
  qos_.requests.emplace(req, at + seconds(30));
  if (op == "sessions.findQos") {
    const auto page = directory(actor, game, "sessions.find", args,guestAware);
    const auto count = page["sessions"].size();
    std::size_t account = 0, title = 0;
    for (const auto &[hash, p] : qos_.permits) {
      if (p.binding.game == game) {
        ++title;
        if (p.binding.actor == actor)
          ++account;
      }
    }
    if (account + count > 32 || title + count > 1024 ||
        qos_.permits.size() + count > 8192)
      throw Error("LIMIT_EXCEEDED");
    const auto who = key(game, actor);
    if (!qos_.searches.contains(who) && qos_.searches.size() >= 8192)
      throw Error("LIMIT_EXCEEDED");
    auto &times = qos_.searches[who];
    if (times.size() >= 8)
      throw Error("RATE_LIMITED");
    times.push_back(at);
    Json rows = Json::array();
    for (const auto &listing : page["sessions"]) {
      const auto secret = randomHex(32);
      QosBinding b;
      b.game = game;
      b.actor = actor;
      b.session = listing["session"];
      b.host = listing["hostMachine"];
      b.epoch = qos_.epochs.try_emplace(b.session, 1).first->second;
      b.deadline = at + seconds(30);
      qos_.permits.emplace(
          sha256(secret),
          QosAuthorityState::Permit{b, fam, [&]{auto filters=args;filters["guestAware"]=guestAware;return filters;}(), b.deadline, false});
      rows.push_back(Json{
          {"listing", listing}, {"permit", secret}, {"lifetimeMs", 30000}});
    }
    return Json{
        {"sessions", rows}, {"start", page["start"]}, {"more", page["more"]}};
  }
  if (op == "sessions.qosTicket") {
    if (args.size() != 1 || !args.contains("permit") ||
        !hex(stringField(args, "permit", 64), 64))
      throw Error("INVALID_ARGUMENT");
    auto it = qos_.permits.find(sha256(args["permit"].get<std::string>()));
    if (it == qos_.permits.end() || it->second.used ||
        it->second.binding.game != game || it->second.binding.actor != actor ||
        it->second.family != fam || !qosValid(it->second))
      throw Error("NOT_AUTHORIZED");
    std::size_t account = 0, host = 0, title = 0, total = 0;
    for (const auto &[hash, t] : qos_.tickets)
      if (!t.permit.binding.hostRole) {
        ++total;
        const auto &b = t.permit.binding;
        if (b.game == game) {
          ++title;
          if (b.actor == actor)
            ++account;
          if (b.session == it->second.binding.session &&
              b.host == it->second.binding.host)
            ++host;
        }
      }
    if (account >= 4 || host >= 4 || title >= 64 || total >= 128)
      throw Error("LIMIT_EXCEEDED");
    auto p = it->second;
    p.binding.probe = randomHex(16);
    p.binding.deadline = std::min(at + seconds(5), p.expires);
    const auto ticket = randomHex(32);
    p.binding.hash = sha256(ticket);
    qos_.tickets.emplace(p.binding.hash,
                         QosAuthorityState::Ticket{p, {}, {}, false});
    it->second.used = true;
    return Json{
        {"ticket", ticket}, {"probe", p.binding.probe}, {"lifetimeMs", 5000}};
  }
  if (op != "sessions.qosHostTicket")
    throw Error("UNKNOWN_OPERATION");
  if (args.size() != 2 || !args.contains("session") ||
      !args.contains("participants"))
    throw Error("INVALID_ARGUMENT");
  const auto session = stringField(args, "session", 32);
  if (!hex(session, 32))
    throw Error("INVALID_ARGUMENT");
  const auto users = directoryParticipants(actor, game, args,true,guestAware);
  const auto snapshot =
      directory(actor, game, "sessions.get", Json{{"session", session}},guestAware);
  if (snapshot["hostId"] != actor ||
      snapshot["hostMachine"] != snapshot["machine"])
    throw Error("NOT_AUTHORIZED");
  std::set<std::string> expected;
  for (const auto &member : snapshot["members"])
    if (member["machine"] == snapshot["machine"])
      expected.insert(member["userId"]);
  if (expected != std::set<std::string>(users.begin(), users.end()))
    throw Error("NOT_AUTHORIZED");
  Statement live(
      store_.db(),
      "SELECT hash FROM relay_tickets WHERE session_id=? AND game_id=? AND "
      "machine_id=? AND owner_id=? AND used=1 ORDER BY created DESC");
  live.bind(1, session);
  live.bind(2, game);
  live.bind(3, snapshot["machine"].get<std::string>());
  live.bind(4, actor);
  RelayGrant relay;
  bool found = false;
  while (live.row()) {
    relay = {live.text(0), game, session, snapshot["machine"], actor};
    if (validateRelayGrantLocked(relay, true)) {
      found = true;
      break;
    }
  }
  if (!found)
    throw Error("NOT_AUTHORIZED");
  std::size_t hosts = 0;
  for (const auto &[hash, t] : qos_.tickets)
    if (t.permit.binding.hostRole) {
      ++hosts;
      if (t.permit.binding.session == session)
        throw Error("LIMIT_EXCEEDED");
    }
  if (hosts >= 256)
    throw Error("LIMIT_EXCEEDED");
  QosBinding b;
  b.game = game;
  b.actor = actor;
  b.session = session;
  b.host = snapshot["machine"];
  b.hostRole = true;
  b.epoch = qos_.epochs.try_emplace(session, 1).first->second;
  b.deadline = at + seconds(5);
  const auto ticket = randomHex(32);
  b.hash = sha256(ticket);
  QosAuthorityState::Ticket record{
      {b, fam, {}, at + hours(1), false}, relay, {}, false};
  for (const auto& participant:participantAuthority(actor,game,args,true,guestAware))
    record.families.emplace_back(participant.participant,participant.family);
  qos_.tickets.emplace(b.hash, std::move(record));
  return Json{{"ticket", ticket}, {"lifetimeMs", 5000}};
}
QosBinding Service::redeemQosTicket(const std::string &game,
                                    const std::string &secret) {
  if (!identifier(game) || !hex(secret, 64))
    throw Error("UNAUTHENTICATED");
  std::lock_guard lock(mutex_);
  pruneDirectory();
  pruneQos();
  auto it = qos_.tickets.find(sha256(secret));
  if (it == qos_.tickets.end() || it->second.redeemed ||
      it->second.permit.binding.game != game || !qosValid(it->second.permit))
    throw Error("UNAUTHENTICATED");
  auto &t = it->second;
  if (t.permit.binding.hostRole && !validateRelayGrantLocked(t.relay, true))
    throw Error("UNAUTHENTICATED");
  if(t.permit.binding.hostRole) {
    Statement group(store_.db(),"SELECT COUNT(*) FROM directory_members WHERE machine_id=? AND session_id=? AND game_id=?");
    group.bind(1,t.permit.binding.host);group.bind(2,t.permit.binding.session);group.bind(3,game);(void)group.row();
    if(group.number(0)!=static_cast<long long>(t.families.size()))throw Error("UNAUTHENTICATED");
  }
  if (t.permit.binding.hostRole)
    for (const auto &[user, familyId] : t.families) {
      Statement f(store_.db(),"SELECT 1 FROM refresh_families f JOIN users u ON u.id=f.user_id JOIN directory_members m ON COALESCE(m.parent_id,m.user_id)=u.id WHERE f.id=? AND m.user_id=? AND f.game_id=? AND m.game_id=f.game_id AND f.revoked=0 AND f.expires>? AND u.online_allowed=1 AND (m.family_id IS NULL OR m.family_id=f.id) AND m.machine_id=? AND m.session_id=?");
      f.bind(1,familyId);f.bind(2,user);f.bind(3,game);f.bind(4,now());f.bind(5,t.permit.binding.host);f.bind(6,t.permit.binding.session);
      if (!f.row())
        throw Error("UNAUTHENTICATED");
    }
  t.redeemed = true;
  t.permit.binding.deadline =
      t.permit.binding.hostRole
          ? qos_.clock() + std::chrono::hours(1)
          : t.permit.binding.deadline;
  return t.permit.binding;
}
bool Service::validateQosGrant(const QosBinding &b) {
  std::lock_guard lock(mutex_);
  pruneDirectory();
  pruneQos();
  auto it = qos_.tickets.find(b.hash);
  if (it == qos_.tickets.end() || !it->second.redeemed ||
      it->second.permit.binding != b || !qosValid(it->second.permit))
    return false;
  if (b.hostRole) {
    Statement group(store_.db(),"SELECT COUNT(*) FROM directory_members WHERE machine_id=? AND session_id=? AND game_id=?");
    group.bind(1,b.host);group.bind(2,b.session);group.bind(3,b.game);(void)group.row();
    if(group.number(0)!=static_cast<long long>(it->second.families.size()))return false;
    if (!validateRelayGrantLocked(it->second.relay, true))
      return false;
    for (const auto &[actor, familyId] : it->second.families) {
      Statement f(store_.db(),"SELECT 1 FROM refresh_families f JOIN users u ON u.id=f.user_id JOIN directory_members m ON COALESCE(m.parent_id,m.user_id)=u.id WHERE f.id=? AND m.user_id=? AND f.game_id=? AND m.game_id=f.game_id AND f.revoked=0 AND f.expires>? AND u.online_allowed=1 AND (m.family_id IS NULL OR m.family_id=f.id) AND m.machine_id=? AND m.session_id=?");
      f.bind(1,familyId);f.bind(2,actor);f.bind(3,b.game);f.bind(4,now());f.bind(5,b.host);f.bind(6,b.session);
      if (!f.row())
        return false;
    }
  }
  return true;
}
void Service::releaseQosGrant(const QosBinding &b) {
  std::lock_guard lock(mutex_);
  auto it = qos_.tickets.find(b.hash);
  if (it != qos_.tickets.end() && it->second.permit.binding == b)
    qos_.tickets.erase(it);
}
} // namespace CnaService
