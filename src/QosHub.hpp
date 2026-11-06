// SPDX-License-Identifier: MIT
#pragma once
#include "CnaService/QosAuthority.hpp"
#include <array>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <tuple>
namespace CnaService {
class QosChannel {
public:
  virtual ~QosChannel() = default;
  virtual bool offer(std::vector<unsigned char> bytes) = 0;
};
class QosQueue {
public:
  bool offer(std::vector<unsigned char> bytes);
  std::optional<std::vector<unsigned char>> take();
  void complete(std::size_t bytes);
  void close();
  bool pending();

private:
  std::mutex mutex_;
  std::deque<std::vector<unsigned char>> queue_;
  std::size_t count_ = 0, bytes_ = 0, active_ = 0;
  bool closed_ = false;
};
class QosHub {
public:
  using Time = std::chrono::steady_clock::time_point;
  explicit QosHub(std::function<Time()> clock = [] {
    return std::chrono::steady_clock::now();
  });
  bool attachHost(const QosBinding &, const std::shared_ptr<QosChannel> &);
  void detachHost(const QosBinding &, const QosChannel *);
  bool open(const QosBinding &, const std::shared_ptr<QosChannel> &);
  bool fromSearcher(const QosBinding &, const QosChannel *,
                    std::span<const unsigned char>);
  bool fromHost(const QosBinding &, const QosChannel *,
                std::span<const unsigned char>);
  bool cancel(const QosBinding &, const QosChannel *);
  bool control(const QosBinding &, const QosChannel *);
  bool alive(const QosBinding &);
  void expire();
  std::size_t active();
  void revokeHost(const std::string &, const std::string &, const std::string &,
                  std::uint64_t);

private:
  using Key = std::tuple<std::string, std::string, std::string, std::uint64_t>;
  struct Budget {
    std::size_t frames = 0, bytes = 0;
    bool accept(std::size_t size);
  };
  struct Job {
    QosBinding binding;
    std::weak_ptr<QosChannel> searcher;
    Budget up, down;
    std::array<std::uint64_t, 4> nonces{};
    std::array<bool, 4> pongs{};
    std::array<bool, 8> ups{}, downs{};
    bool reported = false;
    std::optional<Time> trafficDeadline;
  };
  static Key key(const QosBinding &);
  bool route(const QosBinding &, const QosChannel *,
             std::span<const unsigned char>, bool host);
  std::function<Time()> clock_;
  std::mutex mutex_;
  std::map<Key, std::weak_ptr<QosChannel>> hosts_;
  std::map<std::string, Job> jobs_;
};
} // namespace CnaService
