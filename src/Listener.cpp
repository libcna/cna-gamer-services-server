// SPDX-License-Identifier: MIT
#include "CnaService/Listener.hpp"
#include "CnaService/Service.hpp"
#include "RelayListener.hpp"
#include "EventListener.hpp"
#include "CnaService/RelayProtocol.hpp"
#include <boost/beast/websocket.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <algorithm>
#include <sstream>
#include <utility>
#include <map>
#include <mutex>
#include <thread>
#if __has_include(<sys/resource.h>)
#include <sys/resource.h>
#endif
#include <iostream>

namespace CnaService {
namespace net=boost::asio;
namespace beast=boost::beast;
namespace http=beast::http;
using tcp=net::ip::tcp;
namespace {
// Storage work runs on its own threads, and sign-in on two more: a burst of scrypt derivations
// (tens of milliseconds and 32 MiB each) then never queues ordinary requests behind it.
struct Workers {
    net::thread_pool requests{4};
    net::thread_pool signIns{2};
};
void raiseDescriptorLimit() {
#if __has_include(<sys/resource.h>)
    // Every control and relay connection is a descriptor; take what the host allows, up to a
    // generous ceiling. Should the limit still run out, accept backs off instead of failing.
    rlimit limit{};
    if(getrlimit(RLIMIT_NOFILE,&limit)==0) {
        const rlim_t wanted=limit.rlim_max==RLIM_INFINITY?65536:std::min<rlim_t>(limit.rlim_max,65536);
        if(limit.rlim_cur<wanted){limit.rlim_cur=wanted;(void)setrlimit(RLIMIT_NOFILE,&limit);}
    }
#endif
}
bool signIn(std::string_view body) {
    try {const auto request=parse(body);return request.is_object()&&request.contains("op")&&request["op"]=="auth.login";}
    catch(...) {return false;}
}
// Control requests and relay upgrades that have not redeemed a ticket share one pool, of which a
// single address can hold only a slice: one slow or hostile host cannot shut everyone else out. A
// relay that redeemed its ticket leaves the pool; RelayHub bounds those by account-backed grants.
constexpr int MaxControlConnections=256;
constexpr int MaxPeerConnections=32;
// New connections one address may open a minute: every one costs a TLS handshake, so a host that
// connects and drops in a loop is cut off long before it matters, while a NAT full of players on
// keep-alive connections stays far below it.
constexpr int MaxPeerConnectionsPerMinute=600;
class Admission {
public:
    bool admit(const std::string& peer) {
        std::lock_guard lock(mutex_);
        const auto now=std::chrono::steady_clock::now();
        if(rates_.size()>=4096)std::erase_if(rates_,[&](const auto& entry){return now-entry.second.first>=std::chrono::minutes(1);});
        auto& [start,opened]=rates_[peer];
        if(now-start>=std::chrono::minutes(1)){start=now;opened=0;}
        const auto held=peers_.find(peer);
        if(control_>=MaxControlConnections||(held!=peers_.end()&&held->second>=MaxPeerConnections)||opened>=MaxPeerConnectionsPerMinute) {
            ++refused_;return false;
        }
        ++opened;++peers_[peer];++control_;return true;
    }
    /** Open control connections, and refusals since the previous call. */
    std::pair<int,unsigned long long> take() {std::lock_guard lock(mutex_);return {control_,std::exchange(refused_,0)};}
    void leave(const std::string& peer) {
        std::lock_guard lock(mutex_);
        --control_;if(const auto held=peers_.find(peer);held!=peers_.end()&&--held->second<=0)peers_.erase(held);
    }
private:
    std::mutex mutex_;
    std::map<std::string,int> peers_;
    std::map<std::string,std::pair<std::chrono::steady_clock::time_point,int>> rates_;
    int control_=0;
    unsigned long long refused_=0;
};
// One line a minute for the operator: responses per error code, refused connections, open
// control connections and attached relays. Never a credential, address or request body.
net::awaitable<void> report(Service& service,Admission& admission,RelayHub& hub,EventHub& events) {
    net::steady_timer timer(co_await net::this_coro::executor);
    while(true) {
        timer.expires_after(std::chrono::minutes(1));
        co_await timer.async_wait(net::use_awaitable);
        std::ostringstream line;line<<"stats";
        for(const auto& [code,count]:service.takeOutcomes())line<<' '<<code<<'='<<count;
        const auto [control,refused]=admission.take();
        line<<" refused="<<refused<<" control="<<control<<" relays="<<hub.size()<<" events="<<events.size();
        std::cout<<line.str()<<std::endl;
    }
}
class ConnectionLease {
public:
    ConnectionLease(Admission& admission,std::string peer):admission_(admission),peer_(std::move(peer)) {}
    ~ConnectionLease() {release();}
    ConnectionLease(const ConnectionLease&)=delete;
    ConnectionLease& operator=(const ConnectionLease&)=delete;
    void release() {if(held_){held_=false;admission_.leave(peer_);}}
private:
    Admission& admission_;
    std::string peer_;
    bool held_=true;
};
// A client may send further requests on the same connection, saving a TLS handshake each: the
// next one must start within 5 s, and a connection serves at most 1000. Five seconds keeps a
// burst (sign-in, a catalog download) on one connection but frees an idle one long before the
// next heartbeat, so many players behind one NAT address fit within its admission share.
constexpr int MaxRequestsPerConnection=1000;
constexpr std::string_view FilesPath="/cna/v1/files/";
constexpr std::chrono::seconds KeepAliveIdle{5};
template<class Stream>
net::awaitable<bool> exchange(Stream& stream,Service& service,RelayHub& hub,EventHub& events,Workers& workers,const std::string& peer,ConnectionLease& lease) {
    beast::flat_buffer buffer;
    for(int served=0;;++served) {
        beast::get_lowest_layer(stream).expires_after(served==0?std::chrono::seconds(10):KeepAliveIdle);
        http::request_parser<http::string_body> parser;
        parser.body_limit(MaxMessageBytes);parser.header_limit(8192);
        boost::system::error_code ec;
        co_await http::async_read(stream,buffer,parser,net::redirect_error(net::use_awaitable,ec));
        if(ec)co_return false;
        auto request=parser.release();
        if(request.target()==RelayPath&&beast::websocket::is_upgrade(request)) {
            if(buffer.size()!=0)co_return false;
            co_await serveRelay(std::move(stream),std::move(request),service,hub,workers.requests,[&lease]{lease.release();});co_return true;
        }
        if(request.target()==EventsPath&&beast::websocket::is_upgrade(request)) {
            if(buffer.size()!=0)co_return false;
            co_await serveEvents(std::move(stream),std::move(request),service,events,workers.requests,[&lease]{lease.release();});co_return true;
        }
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(10));
        const bool again=request.keep_alive()&&served+1<MaxRequestsPerConnection;
        http::response<http::string_body> response{http::status::ok,11};
        response.set(http::field::content_type,"application/json");response.set(http::field::cache_control,"no-store");
        response.keep_alive(again);
        if(request.method()==http::verb::get&&request.target().starts_with(FilesPath)) {
            // Immutable files by content, as raw bytes: catalog packs travel here, not as hex in JSON.
            std::string_view authorization=request[http::field::authorization];
            const auto token=authorization.starts_with("Bearer ")?authorization.substr(7):std::string_view();
            const std::string game(request["X-CNA-Game"]),path(request.target().substr(FilesPath.size()));
            const auto file=co_await net::co_spawn(workers.requests,[&]()->net::awaitable<Service::File>{co_return service.file(game,token,path);},net::use_awaitable);
            if(file.code=="OK") {
                response.set(http::field::content_type,file.mime);
                response.set(http::field::cache_control,"private, max-age=31536000, immutable");
                response.body()=file.bytes;
            } else {
                response.result(file.code=="UNAUTHENTICATED"?http::status::unauthorized:file.code=="NOT_FOUND"?http::status::not_found:
                    file.code=="RATE_LIMITED"?http::status::too_many_requests:file.code=="INVALID_ARGUMENT"?http::status::bad_request:
                    http::status::internal_server_error);
                response.body()=CnaService::response("",file.code).dump();
            }
        } else if(request.method()!=http::verb::post||request.target()!="/cna/v1") {
            response.result(http::status::not_found);response.body()=CnaService::response("","NOT_FOUND").dump();
        } else {
            // Storage and sign-in derivation block; they run on the worker pools so these network
            // threads keep accepting, handshaking and forwarding relay datagrams meanwhile.
            auto& pool=signIn(request.body())?workers.signIns:workers.requests;
            response.body()=co_await net::co_spawn(pool,[&]()->net::awaitable<std::string>{co_return service.handle(request.body(),peer);},net::use_awaitable);
        }
        response.prepare_payload();
        co_await http::async_write(stream,response,net::redirect_error(net::use_awaitable,ec));
        if(ec||!again)co_return false;
    }
}
net::awaitable<void> connection(tcp::socket socket,std::string peer,Admission& admission,net::ssl::context& tls,Service& service,RelayHub& hub,EventHub& events,Workers& workers,bool insecure) {
    ConnectionLease lease(admission,peer);
    try {
        if(insecure) {
            beast::tcp_stream stream(std::move(socket));co_await exchange(stream,service,hub,events,workers,peer,lease);
        } else {
            beast::ssl_stream<beast::tcp_stream> stream(std::move(socket),tls);
            beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(10));
            co_await stream.async_handshake(net::ssl::stream_base::server,net::use_awaitable);
            if(co_await exchange(stream,service,hub,events,workers,peer,lease))co_return;
            boost::system::error_code ec;
            co_await stream.async_shutdown(net::redirect_error(net::use_awaitable,ec));
        }
    } catch (...) { /* Untrusted transport failures have no credential-bearing diagnostics. */ }
}
net::awaitable<void> accept(tcp::acceptor& acceptor,Admission& admission,net::ssl::context& tls,Service& service,RelayHub& hub,EventHub& events,Workers& workers,bool insecure) {
    net::steady_timer pause(acceptor.get_executor());
    while(true) {
        tcp::socket socket(net::make_strand(acceptor.get_executor()));
        boost::system::error_code ec;
        co_await acceptor.async_accept(socket,net::redirect_error(net::use_awaitable,ec));
        if(ec) {
            // Descriptor exhaustion or a peer that reset before accept must not end the service:
            // back off briefly and keep serving the connections already open.
            if(ec!=net::error::connection_aborted) {
                pause.expires_after(std::chrono::milliseconds(100));co_await pause.async_wait(net::redirect_error(net::use_awaitable,ec));
            }
            continue;
        }
        const auto endpoint=socket.remote_endpoint(ec);
        if(ec)continue;
        auto peer=endpoint.address().to_string();
        if(!admission.admit(peer)) {socket.close(ec);continue;}
        auto executor=socket.get_executor();
        net::co_spawn(executor,connection(std::move(socket),std::move(peer),admission,tls,service,hub,events,workers,insecure),net::detached);
    }
}
}
void listen(const std::string& database,const std::string& address,unsigned short port,
            const std::string& certificate,const std::string& key,bool insecureLoopback) {
    const auto bind=net::ip::make_address(address);
    if(insecureLoopback && !bind.is_loopback())throw Error("INSECURE_BIND_REFUSED");
    if(!insecureLoopback && (certificate.empty()||key.empty()))throw Error("TLS_REQUIRED");
    raiseDescriptorLimit();
    Service service(database);RelayHub hub;EventHub events;
    service.setHintSink([&events](const std::string& user,const std::string& topic){events.notify(user,topic);});
    net::io_context io(2);net::ssl::context tls(net::ssl::context::tls_server);
    if(SSL_CTX_set_min_proto_version(tls.native_handle(),TLS1_2_VERSION)!=1)throw Error("TLS_REQUIRED");
    if(!insecureLoopback) {
        tls.use_certificate_chain_file(certificate);tls.use_private_key_file(key,net::ssl::context::pem);
        if(SSL_CTX_check_private_key(tls.native_handle())!=1)throw Error("TLS_REQUIRED");
    }
    tcp::acceptor acceptor(io,{bind,port});
    net::signal_set signals(io,SIGINT,SIGTERM);signals.async_wait([&](auto,int){io.stop();});
    Workers workers;Admission admission;
    net::co_spawn(io,accept(acceptor,admission,tls,service,hub,events,workers,insecureLoopback),[&](std::exception_ptr error){if(error)io.stop();});
    net::co_spawn(io,report(service,admission,hub,events),net::detached);
    std::cout<<"CNA service listening on "<<address<<":"<<acceptor.local_endpoint().port()<<std::endl;
    std::jthread worker([&]{io.run();});io.run();worker.join();
    workers.requests.join();workers.signIns.join();
}
}
