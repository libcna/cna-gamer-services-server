// SPDX-License-Identifier: MIT
#include "RelayFlow.hpp"
namespace CnaService {
RelayHello parseRelayHello(std::string_view bytes) {
    if(bytes.empty()||bytes.size()>MaxRelayHelloBytes)throw Error("LIMIT_EXCEEDED");
    auto hello=parse(bytes);
    if(!hello.is_object()||hello.size()!=4||!hello.contains("v")||!hello["v"].is_number_integer())throw Error("MALFORMED_MESSAGE");
    for(auto it=hello.begin();it!=hello.end();++it)
        if(it.key()!="v"&&it.key()!="id"&&it.key()!="game"&&it.key()!="ticket")throw Error("MALFORMED_MESSAGE");
    if(hello["v"]!=RelayVersion)throw Error("UNSUPPORTED_VERSION");
    RelayHello result{stringField(hello,"id",64),stringField(hello,"game",64),stringField(hello,"ticket",64)};
    if(!identifier(result.id)||!identifier(result.game)||result.ticket.size()!=64)throw Error("INVALID_ARGUMENT");
    for(auto byte:result.ticket)if(!((byte>='0'&&byte<='9')||(byte>='a'&&byte<='f')))throw Error("INVALID_ARGUMENT");
    return result;
}
bool RelayRate::accept(std::size_t bytes,std::chrono::steady_clock::time_point time) {
    if(messages_==0||time-start_>=std::chrono::seconds(1)){start_=time;messages_=0;bytes_=0;}
    if(messages_>=MaxRelayMessagesPerSecond||bytes>MaxRelayBytesPerSecond-bytes_)return false;
    ++messages_;bytes_+=bytes;return true;
}
RelayQueue::Result RelayQueue::push(std::vector<unsigned char> frame) {
    std::lock_guard lock(mutex_);
    if(closed_)return Result::Closed;
    if(frame.empty()||frame.size()>MaxRelayFrameBytes||count_>=MaxRelayQueuedFrames||frame.size()>MaxRelayQueuedBytes-bytes_)return Result::Full;
    const auto size=frame.size();queue_.push_back(std::move(frame));++count_;bytes_+=size;
    if(notified_)return Result::Accepted;
    notified_=true;return Result::Wake;
}
std::optional<std::vector<unsigned char>> RelayQueue::take() {
    std::lock_guard lock(mutex_);
    if(queue_.empty()){notified_=false;return {};}
    auto frame=std::move(queue_.front());queue_.pop_front();return frame;
}
void RelayQueue::complete(std::size_t bytes) {
    std::lock_guard lock(mutex_);
    if(!closed_){--count_;bytes_-=bytes;}
}
void RelayQueue::close() {
    std::lock_guard lock(mutex_);closed_=true;queue_.clear();count_=0;bytes_=0;
}
}
