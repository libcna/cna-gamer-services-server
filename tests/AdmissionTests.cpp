// SPDX-License-Identifier: MIT
#include "Admission.hpp"
#include <iostream>
#include <stdexcept>
#include <atomic>
#include <barrier>
#include <thread>
#include <vector>
using CnaService::Admission;
namespace {int checks=0;void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}}
int main() {
    try {
        const auto now=std::chrono::steady_clock::time_point{}+std::chrono::hours(24);
        Admission history;
        for(int i=0;i<4096;++i){const auto peer="source-"+std::to_string(i);check(history.admit(peer,now),"fill history");history.leave(peer);}
        for(int i=4096;i<8192;++i)check(!history.admit("source-"+std::to_string(i),now),"new address refused at history cap");
        check(history.admit("source-0",now),"known address reconnects at cap");history.leave("source-0");
        Admission control;
        for(int i=0;i<256;++i)check(control.admit("active-"+std::to_string(i),now),"fill control");
        for(int i=0;i<4096;++i)check(!control.admit("refused-"+std::to_string(i),now),"full control refuses");
        for(int i=0;i<256;++i)control.leave("active-"+std::to_string(i));
        for(int i=0;i<3840;++i){const auto peer="fresh-"+std::to_string(i);check(control.admit(peer,now),"global refusal did not spend address slots");control.leave(peer);}
        check(!control.admit("over",now),"exact cap after global refusal");
        check(control.take().first==0,"balanced control leases");
        Admission rate;
        for(int i=0;i<600;++i){check(rate.admit("one",now),"rate budget");rate.leave("one");}
        check(!rate.admit("one",now),"rate cap");
        Admission peer;
        for(int i=0;i<32;++i)check(peer.admit("one",now),"peer capacity");
        check(!peer.admit("one",now),"peer cap");
        for(int i=0;i<32;++i)peer.leave("one");
        check(peer.take().first==0,"balanced peer leases");
        check(!history.admit("expired",now+std::chrono::seconds(59)),"history not expired early");
        check(history.admit("expired",now+std::chrono::minutes(1)),"history expires exactly at window end");history.leave("expired");
        check(rate.admit("one",now+std::chrono::minutes(1)),"rate window resets");rate.leave("one");
        Admission concurrent;
        std::barrier start(8);
        std::atomic<int> admitted=0;
        std::vector<std::jthread> workers;
        for(int worker=0;worker<8;++worker)workers.emplace_back([&,worker] {
            start.arrive_and_wait();
            for(int i=0;i<1024;++i) {
                const auto address=std::to_string(worker)+":"+std::to_string(i);
                if(concurrent.admit(address,now)){++admitted;concurrent.leave(address);}
            }
        });
        workers.clear();
        check(admitted==4096,"concurrent histories stop at the exact cap");
        check(concurrent.take().first==0,"concurrent leases balanced");
        std::cout<<checks<<" admission checks passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
