// SPDX-License-Identifier: MIT
#include "CnaService/Listener.hpp"
#include "CnaService/Service.hpp"
#include "Admission.hpp"
#include "RelayListener.hpp"
#include "EventListener.hpp"
#include "CnaService/RelayProtocol.hpp"
#include <boost/beast/websocket.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <algorithm>
#include <chrono>
#include <sstream>
#include <utility>
#include <map>
#include <mutex>
#include <optional>
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
// One line a minute for the operator: responses per error code, refused connections, open
// control connections and attached relays. Never a credential, address or request body.
net::awaitable<void> report(Service& service,Admission& admission,RelayHub& hub,EventHub& events,bool jsonLogging) {
    net::steady_timer timer(co_await net::this_coro::executor);
    while(true) {
        timer.expires_after(std::chrono::minutes(1));
        co_await timer.async_wait(net::use_awaitable);
        const auto outcomes=service.takeOutcomes();
        const auto [control,refused]=admission.take();
        if(jsonLogging) {
            std::cout<<Json{{"timestamp",now()},{"level","info"},{"event","statistics"},
                {"outcomes",outcomes},{"refusedConnections",refused},{"controlConnections",control},
                {"relayConnections",hub.size()},{"eventConnections",events.size()}}.dump()<<std::endl;
        } else {
            std::ostringstream line;line<<"stats";
            for(const auto& [code,count]:outcomes)line<<' '<<code<<'='<<count;
            line<<" refused="<<refused<<" control="<<control<<" relays="<<hub.size()<<" events="<<events.size();
            std::cout<<line.str()<<std::endl;
        }
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
                response.body()=file.bytes;
            } else {
                response.result(file.code=="UNAUTHENTICATED"?http::status::unauthorized:file.code=="NOT_AUTHORIZED"?http::status::forbidden:file.code=="NOT_FOUND"?http::status::not_found:
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
std::string labelValue(std::string_view value) {
    std::string escaped;escaped.reserve(value.size());
    for(const char character:value) {
        if(character=='\\'||character=='\"')escaped.push_back('\\');
        if(character=='\n'){escaped+="\\n";continue;}
        escaped.push_back(character);
    }
    return escaped;
}
std::string prometheus(const Service::Metrics& metrics,const Admission::Snapshot& admission,
                       const Admission::Snapshot& diagnostics,std::size_t relays,std::size_t events,
                       std::chrono::steady_clock::time_point started) {
    const auto uptime=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-started).count();
    std::ostringstream out;
    out<<"# HELP cna_build_info Build information.\n# TYPE cna_build_info gauge\ncna_build_info{version=\""<<CNA_SERVER_VERSION<<"\"} 1\n"
       <<"# HELP cna_up Whether the process is running.\n# TYPE cna_up gauge\ncna_up 1\n"
       <<"# HELP cna_ready Whether storage is usable at the expected schema.\n# TYPE cna_ready gauge\ncna_ready "<<(metrics.ready?1:0)<<'\n'
       <<"# HELP cna_uptime_seconds Process uptime.\n# TYPE cna_uptime_seconds gauge\ncna_uptime_seconds "<<uptime<<'\n'
       <<"# HELP cna_schema_version SQLite schema version.\n# TYPE cna_schema_version gauge\ncna_schema_version "<<metrics.schemaVersion<<'\n'
       <<"# HELP cna_database_bytes Allocated SQLite database bytes.\n# TYPE cna_database_bytes gauge\ncna_database_bytes "<<metrics.databaseBytes<<'\n'
       <<"# HELP cna_titles Provisioned titles.\n# TYPE cna_titles gauge\ncna_titles "<<metrics.titles<<'\n'
       <<"# HELP cna_accounts Provisioned accounts.\n# TYPE cna_accounts gauge\ncna_accounts "<<metrics.accounts<<'\n'
       <<"# HELP cna_access_sessions Active access credentials.\n# TYPE cna_access_sessions gauge\ncna_access_sessions "<<metrics.activeAccessSessions<<'\n'
       <<"# HELP cna_directory_sessions Active multiplayer directory sessions.\n# TYPE cna_directory_sessions gauge\ncna_directory_sessions "<<metrics.activeDirectorySessions<<'\n'
       <<"# HELP cna_pending_invitations Unexpired pending invitations.\n# TYPE cna_pending_invitations gauge\ncna_pending_invitations "<<metrics.pendingInvitations<<'\n'
       <<"# HELP cna_connections Current connections by surface.\n# TYPE cna_connections gauge\n"
       <<"cna_connections{surface=\"control\"} "<<admission.connections<<'\n'
       <<"cna_connections{surface=\"relay\"} "<<relays<<'\n'
       <<"cna_connections{surface=\"events\"} "<<events<<'\n'
       <<"cna_connections{surface=\"diagnostics\"} "<<diagnostics.connections<<'\n'
       <<"# HELP cna_refused_connections_total Connections refused since process start.\n# TYPE cna_refused_connections_total counter\n"
       <<"cna_refused_connections_total{surface=\"control\"} "<<admission.refused<<'\n'
       <<"cna_refused_connections_total{surface=\"diagnostics\"} "<<diagnostics.refused<<'\n'
       <<"# HELP cna_responses_total Service responses by bounded outcome.\n# TYPE cna_responses_total counter\n";
    for(const auto& [outcome,count]:metrics.outcomes)out<<"cna_responses_total{outcome=\""<<labelValue(outcome)<<"\"} "<<count<<'\n';
    out<<"# HELP cna_operations_total Service requests by bounded operation.\n# TYPE cna_operations_total counter\n";
    for(const auto& [operation,count]:metrics.operations)out<<"cna_operations_total{operation=\""<<labelValue(operation)<<"\"} "<<count<<'\n';
    constexpr std::array<std::string_view,8> LatencyLabels{"0.001","0.005","0.01","0.025","0.05","0.1","0.25","+Inf"};
    out<<"# HELP cna_request_duration_seconds Service request duration.\n# TYPE cna_request_duration_seconds histogram\n";
    for(std::size_t index=0;index<LatencyLabels.size();++index)
        out<<"cna_request_duration_seconds_bucket{le=\""<<LatencyLabels[index]<<"\"} "<<metrics.requestLatencyBuckets[index]<<'\n';
    out<<"cna_request_duration_seconds_sum "<<static_cast<double>(metrics.requestLatencyMicroseconds)/1000000.0<<'\n'
       <<"cna_request_duration_seconds_count "<<metrics.requestLatencyBuckets.back()<<'\n';
    return out.str();
}
net::awaitable<void> diagnosticsConnection(tcp::socket socket,std::string peer,Admission& admission,
        Admission& controlAdmission,
        Service& service,RelayHub& hub,EventHub& events,Workers& workers,
        std::chrono::steady_clock::time_point started) {
    ConnectionLease lease(admission,std::move(peer));
    try {
        beast::tcp_stream stream(std::move(socket));stream.expires_after(std::chrono::seconds(5));
        beast::flat_buffer buffer;http::request_parser<http::string_body> parser;
        parser.body_limit(0);parser.header_limit(4096);
        co_await http::async_read(stream,buffer,parser,net::use_awaitable);
        const auto request=parser.release();
        http::response<http::string_body> response{http::status::ok,11};
        response.set(http::field::cache_control,"no-store");
        response.set("X-Content-Type-Options","nosniff");
        response.keep_alive(false);
        if(request.method()!=http::verb::get) {
            response.result(http::status::method_not_allowed);response.set(http::field::allow,"GET");
            response.set(http::field::content_type,"application/json");response.body()=Json{{"error","METHOD_NOT_ALLOWED"}}.dump();
        } else if(request.target()=="/healthz") {
            response.set(http::field::content_type,"application/json");response.body()=Json{{"status","ok"}}.dump();
        } else if(request.target()=="/readyz"||request.target()=="/metrics") {
            const auto metrics=co_await net::co_spawn(workers.requests,[&service]()->net::awaitable<Service::Metrics>{co_return service.metrics();},net::use_awaitable);
            if(request.target()=="/readyz") {
                if(!metrics.ready)response.result(http::status::service_unavailable);
                response.set(http::field::content_type,"application/json");
                response.body()=Json{{"status",metrics.ready?"ready":"not-ready"},{"schemaVersion",metrics.schemaVersion},
                    {"expectedSchemaVersion",SchemaVersion}}.dump();
            } else {
                response.set(http::field::content_type,"text/plain; version=0.0.4; charset=utf-8");
                response.body()=prometheus(metrics,controlAdmission.snapshot(),admission.snapshot(),hub.size(),events.size(),started);
            }
        } else {
            response.result(http::status::not_found);response.set(http::field::content_type,"application/json");
            response.body()=Json{{"error","NOT_FOUND"}}.dump();
        }
        response.prepare_payload();
        co_await http::async_write(stream,response,net::use_awaitable);
    } catch(...) { /* The diagnostics listener exposes no request data in logs. */ }
}
net::awaitable<void> diagnosticsAccept(tcp::acceptor& acceptor,Admission& admission,
        Admission& controlAdmission,Service& service,
        RelayHub& hub,EventHub& events,Workers& workers,std::chrono::steady_clock::time_point started) {
    net::steady_timer pause(acceptor.get_executor());
    while(true) {
        tcp::socket socket(net::make_strand(acceptor.get_executor()));boost::system::error_code ec;
        co_await acceptor.async_accept(socket,net::redirect_error(net::use_awaitable,ec));
        if(ec) {
            pause.expires_after(std::chrono::milliseconds(100));
            co_await pause.async_wait(net::redirect_error(net::use_awaitable,ec));continue;
        }
        const auto endpoint=socket.remote_endpoint(ec);if(ec)continue;
        auto peer=endpoint.address().to_string();
        if(!admission.admit(peer)){socket.close(ec);continue;}
        auto executor=socket.get_executor();
        net::co_spawn(executor,diagnosticsConnection(std::move(socket),std::move(peer),admission,
            controlAdmission,service,hub,events,workers,started),net::detached);
    }
}
}
void listen(const std::string& database,const std::string& address,unsigned short port,
            const std::string& certificate,const std::string& key,bool insecureLoopback,
            bool diagnosticsEnabled,const std::string& diagnosticsAddress,
            unsigned short diagnosticsPort,bool jsonLogging) {
    const auto bind=net::ip::make_address(address);
    if(insecureLoopback && !bind.is_loopback())throw Error("INSECURE_BIND_REFUSED");
    if(!insecureLoopback && (certificate.empty()||key.empty()))throw Error("TLS_REQUIRED");
    std::optional<net::ip::address> diagnosticsBind;
    if(diagnosticsEnabled) {
        diagnosticsBind=net::ip::make_address(diagnosticsAddress);
        if(!diagnosticsBind->is_loopback())throw Error("DIAGNOSTICS_BIND_REFUSED");
    }
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
    std::optional<tcp::acceptor> diagnosticsAcceptor;
    if(diagnosticsEnabled)diagnosticsAcceptor.emplace(io,tcp::endpoint{*diagnosticsBind,diagnosticsPort});
    net::signal_set signals(io,SIGINT,SIGTERM);signals.async_wait([&](auto,int){io.stop();});
    Workers workers;Admission admission,diagnosticsAdmission;
    const auto started=std::chrono::steady_clock::now();
    net::co_spawn(io,accept(acceptor,admission,tls,service,hub,events,workers,insecureLoopback),[&](std::exception_ptr error){if(error)io.stop();});
    if(diagnosticsAcceptor)net::co_spawn(io,diagnosticsAccept(*diagnosticsAcceptor,diagnosticsAdmission,
        admission,service,hub,events,workers,started),net::detached);
    net::co_spawn(io,report(service,admission,hub,events,jsonLogging),net::detached);
    if(jsonLogging)std::cout<<Json{{"timestamp",now()},{"level","info"},{"event","service_listening"},
        {"address",address},{"port",acceptor.local_endpoint().port()},{"tls",!insecureLoopback},
        {"version",CNA_SERVER_VERSION}}.dump()<<std::endl;
    else std::cout<<"CNA service listening on "<<address<<":"<<acceptor.local_endpoint().port()<<std::endl;
    if(diagnosticsAcceptor) {
        if(jsonLogging)std::cout<<Json{{"timestamp",now()},{"level","info"},{"event","diagnostics_listening"},
            {"address",diagnosticsAddress},{"port",diagnosticsAcceptor->local_endpoint().port()}}.dump()<<std::endl;
        else std::cout<<"CNA diagnostics listening on "<<diagnosticsAddress<<":"<<diagnosticsAcceptor->local_endpoint().port()<<std::endl;
    }
    std::jthread worker([&]{io.run();});io.run();worker.join();
    workers.requests.join();workers.signIns.join();
}
}
