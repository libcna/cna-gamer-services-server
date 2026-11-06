// SPDX-License-Identifier: MIT
#pragma once
#include "Admission.hpp"
#include <memory>
#include <string>
#include <utility>
namespace CnaService {
/** @brief Thread-confined owner of one successful preauthentication admission.
 * A suspended connection can outlive the listener's owner during io_context teardown.
 * Retain the admission until release; never borrow a pool that may already be destroyed. */
class ConnectionLease {
public:
    /** @brief Takes responsibility for a previously admitted connection.
     * @param admission Non-null pool whose admit(peer) succeeded. @param peer Admitted address. */
    ConnectionLease(std::shared_ptr<Admission> admission,std::string peer)
        :admission_(std::move(admission)),peer_(std::move(peer)) {}
    /** @brief Releases an admission still held when the connection frame is destroyed. */
    ~ConnectionLease() {release();}
    ConnectionLease(const ConnectionLease&)=delete;
    ConnectionLease& operator=(const ConnectionLease&)=delete;
    /** @brief Releases at most once, including an upgrade followed by frame destruction. */
    void release() {
        auto owner=std::move(admission_);
        if(owner)owner->leave(peer_);
    }
private:
    std::shared_ptr<Admission> admission_;
    std::string peer_;
};
}
