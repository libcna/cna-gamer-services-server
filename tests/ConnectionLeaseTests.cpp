// SPDX-License-Identifier: MIT
// Oracle: each admitted connection releases once, and may finish after listener owner teardown.
// The integrator ASan trace reproduces that order through io_context coroutine destruction.
#include "ConnectionLease.hpp"
#include <boost/asio.hpp>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
using CnaService::Admission;
using CnaService::ConnectionLease;
namespace net=boost::asio;
namespace {
int checks=0;
void check(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}
void leaseOutlivesOwner() {
    auto owner=std::make_shared<Admission>();
    check(owner->admit("retained-parent"),"acquire connection");
    auto lease=std::make_unique<ConnectionLease>(owner,"retained-parent");
    std::weak_ptr<Admission> weak=owner;
    owner.reset(); // listen() owner disappears before outstanding async connection cleanup.
    lease.reset(); // Before the fix ASan must observe Admission::leave using freed memory.
    check(weak.expired(),"final lease destruction releases admission storage");
}
void releasesExactlyOnce() {
    auto owner=std::make_shared<Admission>();
    check(owner->admit("shared-address")&&owner->admit("shared-address"),"two connections behind same address");
    auto first=std::make_unique<ConnectionLease>(owner,"shared-address");
    auto last=std::make_unique<ConnectionLease>(owner,"shared-address");
    check(owner->snapshot().connections==2,"two live admissions");
    first->release();check(owner->snapshot().connections==1,"upgrade releases only its own admission");
    first->release();first.reset();check(owner->snapshot().connections==1,"explicit release and destruction cannot double release");
    std::weak_ptr<Admission> weak=owner;owner.reset();
    last->release();last->release();last.reset();
    check(weak.expired(),"no admission owner leak after explicit release");
}
struct Observation {bool started=false;int destroyed=0;};
struct ObserveDestruction {
    std::shared_ptr<Observation> observation;
    ~ObserveDestruction(){++observation->destroyed;}
};
net::awaitable<void> pending(std::unique_ptr<ConnectionLease> lease,std::shared_ptr<Observation> observation) {
    ObserveDestruction destroyed{observation};observation->started=true;
    net::steady_timer timer(co_await net::this_coro::executor);
    timer.expires_after(std::chrono::hours(1));
    co_await timer.async_wait(net::use_awaitable);
    lease->release(); // Normal completion is also safe; this test destroys the pending frame.
}
void stoppedIoDestroysConnectionFrames() {
    auto owner=std::make_shared<Admission>();
    check(owner->admit("control"),"control admission");
    auto lease=std::make_unique<ConnectionLease>(owner,"control");
    const auto observation=std::make_shared<Observation>();
    auto io=std::make_unique<net::io_context>();
    net::co_spawn(*io,pending(std::move(lease),observation),net::detached);
    io->poll();check(observation->started&&observation->destroyed==0,"real coroutine suspended on timer without a socket");
    std::weak_ptr<Admission> weak=owner;owner.reset();
    io->stop();io.reset(); // Same destruction path as suspended Listener::connection on shutdown.
    check(observation->destroyed==1,"io_context destroys outstanding connection frame exactly once");
    check(weak.expired(),"abandoned coroutine releases final admission storage");
}
}
int main(int argc,char** argv) {
    try {
        int ran=0;
        for(const auto& [name,test]:std::initializer_list<std::pair<const char*,void(*)()>>{
                {"owner",leaseOutlivesOwner},{"once",releasesExactlyOnce},{"coroutine",stoppedIoDestroysConnectionFrames}})
            if(argc==1||(argc==2&&std::string(argv[1])==name)){test();++ran;std::cout<<name<<" passed\n";}
        check(ran>0,"unknown test case");std::cout<<checks<<" connection lease checks passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<"connection lease: "<<e.what()<<'\n';return 1;}
}
