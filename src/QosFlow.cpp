// SPDX-License-Identifier: MIT
#include "QosHub.hpp"
namespace CnaService {
bool QosQueue::offer(std::vector<unsigned char> frame) {
  std::lock_guard lock(mutex_);
  if (closed_ || frame.size() > 1200 || count_ >= 16 ||
      bytes_ + frame.size() > 19200)
    return false;
  ++count_;
  bytes_ += frame.size();
  queue_.push_back(std::move(frame));
  return true;
}
std::optional<std::vector<unsigned char>> QosQueue::take() {
  std::lock_guard lock(mutex_);
  if (active_ || queue_.empty())
    return {};
  auto out = std::move(queue_.front());
  queue_.pop_front();
  active_ = out.size();
  return out;
}
void QosQueue::complete(std::size_t size) {
  std::lock_guard lock(mutex_);
  if (closed_)
    return;
  if (!active_ || active_ != size)
    throw QosError("QOS_QUEUE_ORDER");
  --count_;
  bytes_ -= size;
  active_ = 0;
}
bool QosQueue::pending() {
  std::lock_guard lock(mutex_);
  return !queue_.empty();
}
void QosQueue::close() {
  std::lock_guard lock(mutex_);
  closed_ = true;
  queue_.clear();
  count_ = bytes_ = active_ = 0;
}
} // namespace CnaService
