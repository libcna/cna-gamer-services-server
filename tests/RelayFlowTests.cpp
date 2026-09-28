// SPDX-License-Identifier: MIT
#include "RelayFlow.hpp"
#include "RelayListener.hpp"
#include <iostream>
#include <thread>
#include <atomic>
#include <fstream>
using namespace CnaService;
namespace {
int assertions=0;
void check(bool value){++assertions;if(!value)throw std::runtime_error("Relay flow assertion");}
template<class Work> void refused(Work work,const std::string& code) {
    try {work();check(false);}catch(const Error& error){check(error.code()==code);}
}
struct Channel:RelayChannel {
    std::vector<std::vector<unsigned char>> frames;
    void offer(std::vector<unsigned char> frame) override {frames.push_back(std::move(frame));}
};
}
int main() {
    try {
        std::ifstream input(CNA_RELAY_VECTORS);check(input.good());Json corpus;input>>corpus;
        for(const auto& vector:corpus["handshakes"]) {
            const auto text=vector["message"].get<std::string>(),error=vector["error"].get<std::string>();
            if(error=="OK")check(parseRelayHello(text).game=="sample");
            else refused([&]{(void)parseRelayHello(text);},error);
        }
        auto hello=Json{{"v",1},{"id","test"},{"game","one"},{"ticket",std::string(64,'a')}};
        check(parseRelayHello(hello.dump()).id=="test");
        for(const auto& key:{"id","game","ticket","v"}){auto invalid=hello;invalid.erase(key);refused([&]{(void)parseRelayHello(invalid.dump());},"MALFORMED_MESSAGE");}
        for(const auto& key:{"id","game","ticket"}){auto invalid=hello;invalid.erase(key);invalid["unknown"]="x";refused([&]{(void)parseRelayHello(invalid.dump());},"MALFORMED_MESSAGE");}
        auto invalid=hello;invalid["extra"]=true;refused([&]{(void)parseRelayHello(invalid.dump());},"MALFORMED_MESSAGE");
        invalid=hello;invalid["v"]=2;refused([&]{(void)parseRelayHello(invalid.dump());},"UNSUPPORTED_VERSION");
        invalid["v"]=1.0;refused([&]{(void)parseRelayHello(invalid.dump());},"MALFORMED_MESSAGE");
        for(const auto& key:{"id","game","ticket"}){invalid=hello;invalid[key]="../secret";refused([&]{(void)parseRelayHello(invalid.dump());},"INVALID_ARGUMENT");}
        invalid=hello;invalid["ticket"]=std::string(64,'A');refused([&]{(void)parseRelayHello(invalid.dump());},"INVALID_ARGUMENT");
        refused([&]{(void)parseRelayHello("{\"v\":1,\"v\":1}");},"MALFORMED_MESSAGE");
        refused([&]{(void)parseRelayHello(std::string(1025,'x'));},"LIMIT_EXCEEDED");
        RelayRate rate;const auto start=std::chrono::steady_clock::time_point{};
        for(std::size_t i=0;i<MaxRelayMessagesPerSecond;++i)check(rate.accept(1,start));
        check(!rate.accept(0,start));check(!rate.accept(0,start+std::chrono::milliseconds(999)));
        check(rate.accept(MaxRelayBytesPerSecond,start+std::chrono::seconds(1)));check(!rate.accept(1,start+std::chrono::seconds(1)));
        check(rate.accept(1,start+std::chrono::seconds(2)));
        RelayQueue queue;
        for(std::size_t i=0;i<MaxRelayQueuedFrames;++i)check(queue.push(std::vector<unsigned char>(MaxRelayFrameBytes,1))==(i==0?RelayQueue::Result::Wake:RelayQueue::Result::Accepted));
        check(queue.push({1})==RelayQueue::Result::Full);auto active=queue.take();check(active.has_value());
        check(queue.push({1})==RelayQueue::Result::Full);queue.complete(active->size());
        check(queue.push({1})==RelayQueue::Result::Accepted);
        while(auto frame=queue.take())queue.complete(frame->size());
        check(queue.push({1})==RelayQueue::Result::Wake);queue.close();check(!queue.take());check(queue.push({1})==RelayQueue::Result::Closed);
        queue.complete(1); // Cancelled active write cannot underflow closed accounting.
        RelayQueue concurrent;std::atomic<int> accepted{0},wakes{0};
        std::vector<std::jthread> producers;
        for(int thread=0;thread<4;++thread)producers.emplace_back([&]{for(int i=0;i<1000;++i){auto result=concurrent.push({1});if(result==RelayQueue::Result::Wake)++wakes;if(result==RelayQueue::Result::Wake||result==RelayQueue::Result::Accepted)++accepted;}});
        producers.clear();check(accepted==64);check(wakes==1);
        RelayHub hub;auto sender=std::make_shared<Channel>(),recipient=std::make_shared<Channel>(),foreign=std::make_shared<Channel>();
        RelayGrant grant{"hash","one","session","sender","owner"},target=grant;target.machine="target";
        check(hub.attach(grant,sender));check(hub.attach(target,recipient));check(!hub.attach(target,foreign));
        auto other=target;other.game="two";check(hub.attach(other,foreign));
        hub.route(grant,"target",{1,2});check(recipient->frames.size()==1);check(foreign->frames.empty());
        hub.route(grant,"sender",{3});check(sender->frames.empty());
        other=grant;other.session="elsewhere";hub.route(other,"target",{4});check(recipient->frames.size()==1);
        hub.route(grant,"missing",{5});check(recipient->frames.size()==1);
        hub.detach(target,foreign.get());hub.route(grant,"target",{6});check(recipient->frames.size()==2);
        hub.detach(target,recipient.get());hub.route(grant,"target",{7});check(recipient->frames.size()==2);check(hub.attach(target,foreign));
        RelayHub bounded;std::vector<std::shared_ptr<Channel>> live;
        for(std::size_t i=0;i<MaxRelayConnections;++i){auto item=std::make_shared<Channel>();other=grant;other.machine=std::to_string(i);check(bounded.attach(other,item));live.push_back(std::move(item));}
        other.machine="over";check(!bounded.attach(other,sender));live.pop_back();check(bounded.attach(other,sender));
        std::cout<<"Relay flow assertions: "<<assertions<<'\n';return 0;
    }catch(...){std::cerr<<"Relay flow test failed after "<<assertions<<" assertions\n";return 1;}
}
