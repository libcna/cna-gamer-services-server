// SPDX-License-Identifier: MIT
#include "RelayListener.hpp"
#include "CnaService/RelayProtocol.hpp"
namespace CnaService {
bool RelayHub::attach(const RelayGrant& grant,const std::shared_ptr<RelayChannel>& channel) {
    std::lock_guard lock(mutex_);
    std::erase_if(channels_,[](const auto& item){return item.second.expired();});
    Key key{grant.game,grant.session,grant.machine};
    if(channels_.contains(key)||channels_.size()>=MaxRelayConnections)return false;
    channels_.emplace(std::move(key),channel);return true;
}
void RelayHub::detach(const RelayGrant& grant,const RelayChannel* channel) {
    std::lock_guard lock(mutex_);const auto it=channels_.find({grant.game,grant.session,grant.machine});
    if(it!=channels_.end()&&it->second.lock().get()==channel)channels_.erase(it);
}
void RelayHub::route(const RelayGrant& grant,const std::string& destination,std::vector<unsigned char> frame) {
    if(destination==grant.machine)return;
    std::shared_ptr<RelayChannel> target;
    {
        std::lock_guard lock(mutex_);const auto it=channels_.find({grant.game,grant.session,destination});
        if(it!=channels_.end())target=it->second.lock();
    }
    if(target)target->offer(std::move(frame));
}
}
