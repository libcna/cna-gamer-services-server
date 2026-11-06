// SPDX-License-Identifier: MIT
#include "QosListener.hpp"
#include "CnaService/Service.hpp"
#include "QosHub.hpp"
#include "RelayFlow.hpp"
#include <atomic>
#include <boost/asio.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
namespace CnaService {
namespace net = boost::asio;
namespace beast = boost::beast;
namespace ws = beast::websocket;
namespace {
QosHub &qosHub() {
  static QosHub hub;
  return hub;
}
template <class Stream>
class Connection final
    : public QosChannel,
      public std::enable_shared_from_this<Connection<Stream>> {
public:
  Connection(Stream stream, Service &service, QosHub &hub,
             net::thread_pool &workers, std::function<void()> authenticated)
      : socket_(std::move(stream)), service_(service), hub_(hub),
        workers_(workers), authenticated_(std::move(authenticated)),
        deadline_(socket_.get_executor()), validation_(socket_.get_executor()),
        completion_(socket_.get_executor()) {
    completion_.expires_at(std::chrono::steady_clock::time_point::max());
  }
  ~Connection() override { cleanup(); }
  bool offer(std::vector<unsigned char> frame) override {
    auto self = this->shared_from_this();
    if (!queue_.offer(std::move(frame))) {
      if (!overflow_.exchange(true))
        net::post(socket_.get_executor(),
                  [self] { self->stop(ws::close_code::policy_error); });
      return false;
    }
    if (!notified_.exchange(true))
      net::post(socket_.get_executor(), [self] { self->kick(); });
    return true;
  }
  net::awaitable<void> run(RelayRequest request) {
    ws::close_code refusal = ws::close_code::policy_error;
    try {
      beast::get_lowest_layer(socket_).expires_never();
      socket_.set_option(ws::stream_base::timeout{
          std::chrono::seconds(5), std::chrono::seconds(30), true});
      socket_.read_message_max(MaxRelayHelloBytes);
      co_await socket_.async_accept(request, net::use_awaitable);
      auto self = this->shared_from_this();
      socket_.control_callback([weak = std::weak_ptr<Connection>(self)](
                                   auto, beast::string_view) {
        if (auto live = weak.lock();
            live &&
            (!live->rate_.accept(0, std::chrono::steady_clock::now()) ||
             (live->grant_ && !live->hub_.control(*live->grant_, live.get()))))
          live->stop(ws::close_code::policy_error);
      });
      deadline_.expires_after(std::chrono::seconds(RelayAuthenticationSeconds));
      deadline_.async_wait([self](auto ec) {
        if (!ec)
          self->abort();
      });
      // Beast needs one scratch byte to consume a final empty continuation at
      // the exact message limit. read_message_max still refuses any extra
      // payload before growth.
      beast::flat_buffer input(1201);
      co_await socket_.async_read(input, net::use_awaitable);
      if (closing_)
        throw Error("UNAUTHENTICATED");
      if (!socket_.got_text())
        throw Error("MALFORMED_MESSAGE");
      auto hello = parseRelayHello(beast::buffers_to_string(input.data()));
      input.consume(input.size());
      grant_ = co_await net::co_spawn(
          workers_,
          [&]() -> net::awaitable<QosBinding> {
            co_return service_.redeemQosTicket(hello.game, hello.ticket);
          },
          net::use_awaitable);
      std::fill(hello.ticket.begin(), hello.ticket.end(), '\0');
      hello.ticket.clear();
      if (grant_->hostRole && !hub_.attachHost(*grant_, self))
        throw Error("LIMIT_EXCEEDED");
      attached_ = true;
      if (authenticated_)
        authenticated_();
      const auto welcome = Json{
          {"v", 1},
          {"id", hello.id},
          {"error", "OK"},
          {"result",
           {{"role", grant_->hostRole ? "host" : "searcher"},
            {"probe", grant_->hostRole ? Json(nullptr) : Json(grant_->probe)},
            {"maxFrameBytes", 1200},
            {"maxQueuedFrames",
             16}}}}.dump();
      socket_.text(true);
      co_await socket_.async_write(net::buffer(welcome), net::use_awaitable);
      deadline_.cancel();
      ready_ = true;
      socket_.binary(true);
      socket_.read_message_max(1200);
      if (!grant_->hostRole && !hub_.open(*grant_, self))
        throw Error("NOT_AUTHORIZED");
      kick();
      validateLater();
      while (!closing_) {
        co_await socket_.async_read(input, net::use_awaitable);
        if (closing_)
          break;
        if (socket_.got_text())
          throw RelayError("RELAY_MESSAGE_TYPE");
        if (!rate_.accept(input.size(), std::chrono::steady_clock::now()))
          throw Error("LIMIT_EXCEEDED");
        const auto data = input.data();
        const auto bytes = std::span(
            static_cast<const unsigned char *>(data.data()), data.size());
        (void)parseQosFrame(bytes);
        const bool routed = grant_->hostRole
                                ? hub_.fromHost(*grant_, this, bytes)
                                : hub_.fromSearcher(*grant_, this, bytes);
        if (!routed)
          throw Error("NOT_AUTHORIZED");
        input.consume(input.size());
      }
    } catch (const QosError &) {
      refusal = ws::close_code::protocol_error;
    } catch (const Error &error) {
      if (error.code() == "UNSUPPORTED_VERSION" ||
          error.code() == "MALFORMED_MESSAGE")
        refusal = ws::close_code::protocol_error;
    } catch (...) {
      abort();
      co_return;
    }
    stop(refusal);
    if (!aborted_) {
      boost::system::error_code ec;
      co_await completion_.async_wait(
          net::redirect_error(net::use_awaitable, ec));
    }
  }

private:
  void cleanup() noexcept {
    if (attached_) {
      if (grant_->hostRole)
        hub_.detachHost(*grant_, this);
      else
        hub_.cancel(*grant_, this);
      attached_ = false;
    }
    if (grant_) {
      try {
        service_.releaseQosGrant(*grant_);
      } catch (...) {
      }
      grant_.reset();
    }
  }
  void abort() {
    aborted_ = true;
    closing_ = true;
    cleanup();
    queue_.close();
    deadline_.cancel();
    validation_.cancel();
    completion_.cancel();
    boost::system::error_code ec;
    beast::get_lowest_layer(socket_).socket().cancel(ec);
    beast::get_lowest_layer(socket_).socket().close(ec);
  }
  void stop(ws::close_code code) {
    if (closing_)
      return;
    closing_ = true;
    cleanup();
    queue_.close();
    validation_.cancel();
    deadline_.cancel();
    auto self = this->shared_from_this();
    deadline_.expires_after(std::chrono::seconds(5));
    deadline_.async_wait([self](auto ec) {
      if (!ec)
        self->abort();
    });
    net::co_spawn(socket_.get_executor(), close(self, code), net::detached);
  }
  static net::awaitable<void> close(std::shared_ptr<Connection> self,
                                    ws::close_code code) {
    boost::system::error_code ec;
    co_await self->socket_.async_close(
        ws::close_reason(code), net::redirect_error(net::use_awaitable, ec));
    self->abort();
  }
  void validateLater() {
    auto self = this->shared_from_this();
    validation_.expires_after(std::chrono::milliseconds(250));
    validation_.async_wait([self](auto ec) {
      if (ec || self->closing_)
        return;
      // Validation reads storage: on the worker pool, with its own copy of the
      // grant, and the verdict comes back to this connection's strand.
      net::post(self->workers_, [self, grant = *self->grant_] {
        bool valid = false;
        try {
          valid = self->service_.validateQosGrant(grant);
        } catch (...) {
        }
        net::post(self->socket_.get_executor(), [self, valid] {
          if (self->closing_)
            return;
          self->hub_.expire();
          if (!valid ||
              (!self->grant_->hostRole && !self->hub_.alive(*self->grant_))) {
            self->stop(ws::close_code::policy_error);
            return;
          }
          self->validateLater();
        });
      });
    });
  }
  void kick() {
    if (!ready_ || closing_ || writing_)
      return;
    writing_ = true;
    net::co_spawn(socket_.get_executor(), write(this->shared_from_this()),
                  net::detached);
  }
  static net::awaitable<void> write(std::shared_ptr<Connection> self) {
    while (!self->closing_) {
      auto frame = self->queue_.take();
      if (!frame)
        break;
      boost::system::error_code ec;
      co_await self->socket_.async_write(
          net::buffer(*frame), net::redirect_error(net::use_awaitable, ec));
      self->queue_.complete(frame->size());
      if (ec) {
        self->abort();
        break;
      }
    }
    self->writing_ = false;
    self->notified_ = false;
    if (self->queue_.pending() && !self->notified_.exchange(true))
      self->kick();
  }
  ws::stream<Stream> socket_;
  Service &service_;
  QosHub &hub_;
  net::thread_pool &workers_;
  std::function<void()> authenticated_;
  net::steady_timer deadline_, validation_, completion_;
  std::optional<QosBinding> grant_;
  QosQueue queue_;
  RelayRate rate_;
  std::atomic<bool> overflow_{false}, notified_{false};
  bool ready_ = false, writing_ = false, closing_ = false, attached_ = false,
       aborted_ = false;
};
} // namespace
net::awaitable<void> serveQos(RelayTcp stream, RelayRequest request,
                              Service &service, net::thread_pool &workers,
                              std::function<void()> authenticated) {
  auto connection = std::make_shared<Connection<RelayTcp>>(
      std::move(stream), service, qosHub(), workers, std::move(authenticated));
  co_await connection->run(std::move(request));
}
net::awaitable<void> serveQos(RelayTls stream, RelayRequest request,
                              Service &service, net::thread_pool &workers,
                              std::function<void()> authenticated) {
  auto connection = std::make_shared<Connection<RelayTls>>(
      std::move(stream), service, qosHub(), workers, std::move(authenticated));
  co_await connection->run(std::move(request));
}
} // namespace CnaService
