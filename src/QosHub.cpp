// SPDX-License-Identifier: MIT
#include "QosHub.hpp"
#include "CnaService/RelayProtocol.hpp"
namespace CnaService {
QosHub::QosHub(std::function<Time()> clock) : clock_(std::move(clock)) {}
QosHub::Key QosHub::key(const QosBinding &b) {
  return {b.game, b.session, b.host, b.epoch};
}
bool QosHub::Budget::accept(std::size_t size) {
  if (frames >= 32 || bytes + size > 20480)
    return false;
  ++frames;
  bytes += size;
  return true;
}
bool QosHub::attachHost(const QosBinding &b,
                        const std::shared_ptr<QosChannel> &channel) {
  std::lock_guard lock(mutex_);
  std::erase_if(hosts_, [](const auto &p) { return p.second.expired(); });
  if (clock_() >= b.deadline || hosts_.contains(key(b)) || hosts_.size() >= 256)
    return false;
  hosts_[key(b)] = channel;
  return true;
}
void QosHub::detachHost(const QosBinding &b, const QosChannel *channel) {
  {
    std::lock_guard lock(mutex_);
    auto i = hosts_.find(key(b));
    if (i == hosts_.end() || i->second.lock().get() != channel)
      return;
  }
  revokeHost(b.game, b.session, b.host, b.epoch);
}
bool QosHub::open(const QosBinding &b,
                  const std::shared_ptr<QosChannel> &channel) {
  std::shared_ptr<QosChannel> host;
  {
    std::lock_guard lock(mutex_);
    auto h = hosts_.find(key(b));
    if (h == hosts_.end())
      return false;
    host = h->second.lock();
    if (!host || clock_() >= b.deadline || jobs_.contains(b.probe) ||
        jobs_.size() >= 128)
      return false;
    std::size_t account = 0, title = 0, target = 0;
    for (const auto &[id, j] : jobs_)
      if (j.binding.game == b.game) {
        ++title;
        if (j.binding.actor == b.actor)
          ++account;
        if (key(j.binding) == key(b))
          ++target;
      }
    if (account >= 4 || target >= 4 || title >= 64)
      return false;
    Job job;
    job.binding = b;
    job.searcher = channel;
    jobs_.emplace(b.probe, std::move(job));
  }
  if (!host->offer(qosFrame(QosType::Begin, relayMachineId(b.probe)))) {
    cancel(b, nullptr);
    return false;
  }
  return true;
}
bool QosHub::route(const QosBinding &b, const QosChannel *source,
                   std::span<const unsigned char> bytes, bool hostSide) {
  QosFrameView f;
  try {
    f = parseQosFrame(bytes);
  } catch (const QosError &) {
    return false;
  }
  const auto id = relayMachineName(f.probe);
  std::shared_ptr<QosChannel> target;
  QosBinding binding;
  {
    std::lock_guard lock(mutex_);
    auto it = jobs_.find(id);
    if (it == jobs_.end())
      return false;
    auto &j = it->second;
    binding = j.binding;
    if (clock_() >= j.binding.deadline ||
        (j.trafficDeadline && clock_() >= *j.trafficDeadline))
      return false;
    auto h = hosts_.find(key(j.binding));
    if (h == hosts_.end())
      return false;
    auto hc = h->second.lock();
    auto sc = j.searcher.lock();
    if (!hc || !sc)
      return false;
    if (hostSide) {
      if (key(b) != key(j.binding) || source != hc.get() ||
          !j.down.accept(bytes.size()))
        return false;
      if (f.type == QosType::Pong) {
        if (j.pongs[f.sequence] || !j.nonces[f.sequence] ||
            j.nonces[f.sequence] != f.nonce)
          return false;
        j.pongs[f.sequence] = true;
      } else if (f.type == QosType::Down) {
        if (j.downs[f.sequence])
          return false;
        j.downs[f.sequence] = true;
      } else if (f.type == QosType::Report) {
        if (j.reported)
          return false;
        j.reported = true;
      } else if (f.type != QosType::Cancel)
        return false;
      target = sc;
    } else {
      if (b != j.binding || source != sc.get() || !j.up.accept(bytes.size()))
        return false;
      if (f.type == QosType::Ping) {
        if (j.nonces[f.sequence])
          return false;
        j.nonces[f.sequence] = f.nonce;
        if (!j.trafficDeadline)
          j.trafficDeadline =
              std::min(j.binding.deadline, clock_() + std::chrono::seconds(3));
      } else if (f.type == QosType::Up) {
        if (j.ups[f.sequence])
          return false;
        j.ups[f.sequence] = true;
      } else if (f.type != QosType::Cancel)
        return false;
      target = hc;
    }
  }
  if (f.type == QosType::Cancel)
    return cancel(binding, hostSide ? nullptr : source);
  if (!target->offer({bytes.begin(), bytes.end()})) {
    cancel(binding, nullptr);
    return false;
  }
  return true;
}
bool QosHub::fromSearcher(const QosBinding &b, const QosChannel *c,
                          std::span<const unsigned char> v) {
  return route(b, c, v, false);
}
bool QosHub::fromHost(const QosBinding &b, const QosChannel *c,
                      std::span<const unsigned char> v) {
  return route(b, c, v, true);
}
bool QosHub::cancel(const QosBinding &b, const QosChannel *source) {
  std::shared_ptr<QosChannel> host, searcher;
  {
    std::lock_guard lock(mutex_);
    auto i = jobs_.find(b.probe);
    if (i == jobs_.end() || i->second.binding != b)
      return false;
    searcher = i->second.searcher.lock();
    if (source && searcher.get() != source)
      return false;
    auto h = hosts_.find(key(b));
    if (h != hosts_.end())
      host = h->second.lock();
    jobs_.erase(i);
  }
  const auto frame = qosFrame(QosType::Cancel, relayMachineId(b.probe));
  if (host)
    host->offer(frame);
  if (searcher)
    searcher->offer(frame);
  return true;
}
bool QosHub::control(const QosBinding &b, const QosChannel *source) {
  std::lock_guard lock(mutex_);
  if (b.hostRole) {
    auto h = hosts_.find(key(b));
    if (h == hosts_.end() || h->second.lock().get() != source)
      return false;
    for (auto &[id, j] : jobs_)
      if (key(j.binding) == key(b) && !j.down.accept(0))
        return false;
    return true;
  }
  auto i = jobs_.find(b.probe);
  return i != jobs_.end() && i->second.binding == b &&
         i->second.searcher.lock().get() == source && i->second.up.accept(0);
}
bool QosHub::alive(const QosBinding &b) {
  std::lock_guard lock(mutex_);
  auto i = jobs_.find(b.probe);
  return i != jobs_.end() && i->second.binding == b && clock_() < b.deadline;
}
void QosHub::expire() {
  std::vector<QosBinding> expired;
  {
    std::lock_guard lock(mutex_);
    for (const auto &[id, j] : jobs_)
      if (clock_() >= j.binding.deadline ||
          (j.trafficDeadline && clock_() >= *j.trafficDeadline) ||
          j.searcher.expired())
        expired.push_back(j.binding);
  }
  for (const auto &b : expired)
    cancel(b, nullptr);
}
std::size_t QosHub::active() {
  std::lock_guard lock(mutex_);
  return jobs_.size();
}
void QosHub::revokeHost(const std::string &game, const std::string &session,
                        const std::string &host, std::uint64_t epoch) {
  std::vector<QosBinding> dead;
  {
    std::lock_guard lock(mutex_);
    Key k{game, session, host, epoch};
    hosts_.erase(k);
    for (const auto &[id, j] : jobs_)
      if (key(j.binding) == k)
        dead.push_back(j.binding);
  }
  for (const auto &b : dead)
    cancel(b, nullptr);
}
} // namespace CnaService
