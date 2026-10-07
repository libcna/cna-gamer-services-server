// SPDX-License-Identifier: MIT
#include "Admission.hpp"
namespace CnaService {
// Control requests and relay upgrades that have not redeemed a ticket share one pool, of which a
// single address can hold only a slice: one slow or hostile host cannot shut everyone else out. A
// relay that redeemed its ticket leaves the pool; RelayHub bounds those by account-backed grants.
constexpr int MaxControlConnections=256;
constexpr int MaxPeerConnections=32;
// New connections one address may open a minute: every one costs a TLS handshake, so a host that
// connects and drops in a loop is cut off long before it matters, while a NAT full of players on
// keep-alive connections stays far below it.
constexpr int MaxPeerConnectionsPerMinute=600;
bool Admission::admit(const std::string& peer, std::chrono::steady_clock::time_point now) {
        std::lock_guard lock(mutex_);
        const auto held=peers_.find(peer);
        if(control_>=MaxControlConnections||(held!=peers_.end()&&held->second>=MaxPeerConnections)) {
            ++refused_;++refusedTotal_;return false;
        }
        if(rates_.size()>=4096)std::erase_if(rates_,[&](const auto& entry){return now-entry.second.first>=std::chrono::minutes(1);});
        if(rates_.size()>=4096&&!rates_.contains(peer)) {++refused_;++refusedTotal_;return false;}
        auto& [start,opened]=rates_[peer];
        if(now-start>=std::chrono::minutes(1)){start=now;opened=0;}
        if(opened>=MaxPeerConnectionsPerMinute) {
            ++refused_;++refusedTotal_;return false;
        }
        ++opened;++peers_[peer];++control_;return true;
    }
std::pair<int,unsigned long long> Admission::take() {std::lock_guard lock(mutex_);return {control_,std::exchange(refused_,0)};}
Admission::Snapshot Admission::snapshot() {std::lock_guard lock(mutex_);return {control_,refusedTotal_};}
void Admission::leave(const std::string& peer) {
        std::lock_guard lock(mutex_);
        --control_;if(const auto held=peers_.find(peer);held!=peers_.end()&&--held->second<=0)peers_.erase(held);
    }
}
