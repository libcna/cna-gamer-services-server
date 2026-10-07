// SPDX-License-Identifier: MIT
// The account event channel: after a hello naming the title and an access token, the server sends
// a text frame {"v":1,"topics":[...]} whenever another account changed something the client shows
// (invitations, messages, friends, party). Hints only: the client reads the change through the
// ordinary requests, so a lost hint costs nothing but the poll interval.
#include "EventListener.hpp"
#include "CnaService/Protocol.hpp"
#include "CnaService/Service.hpp"
#include <boost/asio.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
namespace CnaService {
namespace net=boost::asio;
namespace beast=boost::beast;
namespace ws=beast::websocket;
namespace {
constexpr std::size_t MaxChannelsPerAccount=8,MaxChannels=4096,MaxHelloBytes=1024;
constexpr auto HelloSeconds=std::chrono::seconds(10);
constexpr auto RevalidateSeconds=std::chrono::seconds(60);
template<class Stream>
class Connection final:public EventChannel,public std::enable_shared_from_this<Connection<Stream>> {
public:
    Connection(Stream stream,Service& service,EventHub& hub,net::thread_pool& workers,std::function<void()> authenticated)
        :socket_(std::move(stream)),service_(service),hub_(hub),workers_(workers),authenticated_(std::move(authenticated)),
         deadline_(socket_.get_executor()),validation_(socket_.get_executor()) {}
    ~Connection() override {if(attached_)hub_.detach(user_,this);}
    void offer(const std::string& topic) override {
        bool wake=false;
        {std::lock_guard lock(mutex_);wake=pending_.empty()&&!closing_;pending_.insert(topic);}
        if(wake)net::post(socket_.get_executor(),[self=this->shared_from_this()]{self->kick();});
    }
    net::awaitable<void> run(RelayRequest request) {
        try {
            beast::get_lowest_layer(socket_).expires_never();
            socket_.set_option(ws::stream_base::timeout{std::chrono::seconds(5),std::chrono::seconds(30),true});
            socket_.read_message_max(MaxHelloBytes);
            co_await socket_.async_accept(request,net::use_awaitable);
            auto self=this->shared_from_this();
            deadline_.expires_after(HelloSeconds);
            deadline_.async_wait([self](auto ec){if(!ec)self->abort();});
            beast::flat_buffer input;
            co_await socket_.async_read(input,net::use_awaitable);
            if(!socket_.got_text())throw Error("MALFORMED_MESSAGE");
            const auto hello=parse(beast::buffers_to_string(input.data()));input.consume(input.size());
            // The event credential envelope is deliberately exact, as the relay hello and control
            // envelope are. Ignoring an extra field risks accidentally assigning meaning to it in
            // one client while the server authenticates a different message.
            if(!hello.is_object()||hello.size()!=4||!hello.contains("v")||!hello["v"].is_number_integer())
                throw Error("MALFORMED_MESSAGE");
            for(const auto& [key,value]:hello.items()) {
                (void)value;
                if(key!="v"&&key!="id"&&key!="game"&&key!="token")throw Error("MALFORMED_MESSAGE");
            }
            if(hello["v"]!=ProtocolVersion)throw Error("UNSUPPORTED_VERSION");
            const auto id=stringField(hello,"id",64);
            game_=stringField(hello,"game",64);token_=stringField(hello,"token",128);
            if(!identifier(id)||!identifier(game_)||token_.size()!=64)throw Error("INVALID_ARGUMENT");
            user_=co_await net::co_spawn(workers_,[&]()->net::awaitable<std::string>{co_return service_.eventAccount(game_,token_);},net::use_awaitable);
            if(!hub_.attach(user_,self))throw Error("LIMIT_EXCEEDED");
            attached_=true;
            if(authenticated_)authenticated_();
            deadline_.cancel();
            const auto welcome=Json{{"v",1},{"id",id},{"error","OK"},{"result",{{"topics",Json::array({"invitations","messages","friends","party"})}}}}.dump();
            socket_.text(true);
            co_await socket_.async_write(net::buffer(welcome),net::use_awaitable);
            ready_=true;kick();revalidateLater();
            // The client sends nothing more; anything it does send ends the channel.
            co_await socket_.async_read(input,net::use_awaitable);
        } catch(...) {}
        abort();
    }
private:
    void abort() {
        {std::lock_guard lock(mutex_);closing_=true;}
        deadline_.cancel();validation_.cancel();
        if(attached_){hub_.detach(user_,this);attached_=false;}
        boost::system::error_code ec;beast::get_lowest_layer(socket_).socket().close(ec);
    }
    // A token that expired or was signed out ends the channel; the client reconnects with its new one.
    void revalidateLater() {
        auto self=this->shared_from_this();validation_.expires_after(RevalidateSeconds);
        validation_.async_wait([self](auto ec){
            if(ec)return;
            net::post(self->workers_,[self]{
                bool valid=false;
                try{valid=self->service_.eventAccount(self->game_,self->token_)==self->user_;}catch(...){}
                net::post(self->socket_.get_executor(),[self,valid]{if(valid)self->revalidateLater();else self->abort();});
            });
        });
    }
    void kick() {
        if(!ready_||writing_)return;
        writing_=true;net::co_spawn(socket_.get_executor(),write(this->shared_from_this()),net::detached);
    }
    static net::awaitable<void> write(std::shared_ptr<Connection> self) {
        while(true) {
            std::set<std::string> topics;
            {std::lock_guard lock(self->mutex_);if(self->closing_||self->pending_.empty())break;topics.swap(self->pending_);}
            const auto frame=Json{{"v",1},{"topics",Json(topics)}}.dump();
            boost::system::error_code ec;
            co_await self->socket_.async_write(net::buffer(frame),net::redirect_error(net::use_awaitable,ec));
            if(ec){self->abort();break;}
        }
        self->writing_=false;
    }
    ws::stream<Stream> socket_;
    Service& service_;EventHub& hub_;net::thread_pool& workers_;
    std::function<void()> authenticated_;
    net::steady_timer deadline_,validation_;
    std::mutex mutex_;
    std::set<std::string> pending_;
    std::string game_,token_,user_;
    bool ready_=false,writing_=false,closing_=false,attached_=false;
};
}
bool EventHub::attach(const std::string& user,const std::shared_ptr<EventChannel>& channel) {
    std::lock_guard lock(mutex_);
    std::erase_if(channels_,[](const auto& entry){return entry.second.expired();});
    if(channels_.size()>=MaxChannels||channels_.count(user)>=MaxChannelsPerAccount)return false;
    channels_.emplace(user,channel);return true;
}
void EventHub::detach(const std::string& user,const EventChannel* channel) {
    std::lock_guard lock(mutex_);
    const auto [first,last]=channels_.equal_range(user);
    for(auto it=first;it!=last;++it)if(auto live=it->second.lock();!live||live.get()==channel){channels_.erase(it);return;}
}
void EventHub::notify(const std::string& user,const std::string& topic) {
    std::vector<std::shared_ptr<EventChannel>> live;
    {
        std::lock_guard lock(mutex_);
        const auto [first,last]=channels_.equal_range(user);
        for(auto it=first;it!=last;++it)if(auto channel=it->second.lock())live.push_back(std::move(channel));
    }
    for(auto& channel:live)channel->offer(topic);
}
std::size_t EventHub::size() {
    std::lock_guard lock(mutex_);
    return static_cast<std::size_t>(std::count_if(channels_.begin(),channels_.end(),[](const auto& entry){return !entry.second.expired();}));
}
net::awaitable<void> serveEvents(RelayTcp stream,RelayRequest request,Service& service,EventHub& hub,net::thread_pool& workers,
                                 std::function<void()> authenticated) {
    auto connection=std::make_shared<Connection<RelayTcp>>(std::move(stream),service,hub,workers,std::move(authenticated));
    co_await connection->run(std::move(request));
}
net::awaitable<void> serveEvents(RelayTls stream,RelayRequest request,Service& service,EventHub& hub,net::thread_pool& workers,
                                 std::function<void()> authenticated) {
    auto connection=std::make_shared<Connection<RelayTls>>(std::move(stream),service,hub,workers,std::move(authenticated));
    co_await connection->run(std::move(request));
}
}
