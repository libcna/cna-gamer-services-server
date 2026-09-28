// SPDX-License-Identifier: MIT
#include "CnaService/Listener.hpp"
#include "CnaService/Service.hpp"
#include "RelayListener.hpp"
#include "CnaService/RelayProtocol.hpp"
#include <boost/beast/websocket.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <atomic>
#include <thread>
#include <iostream>

namespace CnaService {
namespace net=boost::asio;
namespace beast=boost::beast;
namespace http=beast::http;
using tcp=net::ip::tcp;
namespace {
std::atomic<int> connections{0};
struct ConnectionLease { ~ConnectionLease(){--connections;} };
template<class Stream>
net::awaitable<bool> exchange(Stream& stream,Service& service,RelayHub& hub,const std::string& peer) {
    beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(10));
    beast::flat_buffer buffer;
    http::request_parser<http::string_body> parser;
    parser.body_limit(MaxMessageBytes);parser.header_limit(8192);
    boost::system::error_code ec;
    co_await http::async_read(stream,buffer,parser,net::redirect_error(net::use_awaitable,ec));
    if(ec)co_return false;
    auto request=parser.release();
    if(request.target()==RelayPath&&beast::websocket::is_upgrade(request)) {
        if(buffer.size()!=0)co_return false;
        co_await serveRelay(std::move(stream),std::move(request),service,hub);co_return true;
    }
    http::response<http::string_body> response{http::status::ok,11};
    response.set(http::field::content_type,"application/json");response.set(http::field::cache_control,"no-store");
    response.keep_alive(false);
    if(request.method()!=http::verb::post||request.target()!="/cna/v1") {
        response.result(http::status::not_found);response.body()=CnaService::response("","NOT_FOUND").dump();
    } else response.body()=service.handle(request.body(),peer);
    response.prepare_payload();
    co_await http::async_write(stream,response,net::redirect_error(net::use_awaitable,ec));
    co_return false;
}
net::awaitable<void> connection(tcp::socket socket,net::ssl::context& tls,Service& service,RelayHub& hub,bool insecure) {
    ConnectionLease lease;
    try {
        const auto peer=socket.remote_endpoint().address().to_string();
        if(insecure) {
            beast::tcp_stream stream(std::move(socket));co_await exchange(stream,service,hub,peer);
        } else {
            beast::ssl_stream<beast::tcp_stream> stream(std::move(socket),tls);
            beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(10));
            co_await stream.async_handshake(net::ssl::stream_base::server,net::use_awaitable);
            if(co_await exchange(stream,service,hub,peer))co_return;
            boost::system::error_code ec;
            co_await stream.async_shutdown(net::redirect_error(net::use_awaitable,ec));
        }
    } catch (...) { /* Untrusted transport failures have no credential-bearing diagnostics. */ }
}
net::awaitable<void> accept(tcp::acceptor& acceptor,net::ssl::context& tls,Service& service,RelayHub& hub,bool insecure) {
    while(true) {
        tcp::socket socket(net::make_strand(acceptor.get_executor()));
        co_await acceptor.async_accept(socket,net::use_awaitable);
        if(connections.fetch_add(1)>=128) { --connections;socket.close();continue; }
        auto executor=socket.get_executor();
        net::co_spawn(executor,connection(std::move(socket),tls,service,hub,insecure),net::detached);
    }
}
}
void listen(const std::string& database,const std::string& address,unsigned short port,
            const std::string& certificate,const std::string& key,bool insecureLoopback) {
    const auto bind=net::ip::make_address(address);
    if(insecureLoopback && !bind.is_loopback())throw Error("INSECURE_BIND_REFUSED");
    if(!insecureLoopback && (certificate.empty()||key.empty()))throw Error("TLS_REQUIRED");
    Service service(database);RelayHub hub;
    net::io_context io(2);net::ssl::context tls(net::ssl::context::tls_server);
    if(SSL_CTX_set_min_proto_version(tls.native_handle(),TLS1_2_VERSION)!=1)throw Error("TLS_REQUIRED");
    if(!insecureLoopback) {
        tls.use_certificate_chain_file(certificate);tls.use_private_key_file(key,net::ssl::context::pem);
        if(SSL_CTX_check_private_key(tls.native_handle())!=1)throw Error("TLS_REQUIRED");
    }
    tcp::acceptor acceptor(io,{bind,port});
    net::signal_set signals(io,SIGINT,SIGTERM);signals.async_wait([&](auto,int){io.stop();});
    net::co_spawn(io,accept(acceptor,tls,service,hub,insecureLoopback),[&](std::exception_ptr error){if(error)io.stop();});
    std::cout<<"CNA service listening on "<<address<<":"<<acceptor.local_endpoint().port()<<std::endl;
    std::jthread worker([&]{io.run();});io.run();worker.join();
}
}
