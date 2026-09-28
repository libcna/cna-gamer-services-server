// SPDX-License-Identifier: MIT
#pragma once
#include <string>
namespace CnaService {
/** @brief Server-owned redeemed connection authority, not a network authentication token. */
struct RelayGrant {
    /** @brief Hashed one-use ticket record; never the original bearer credential. */
    std::string ticketHash;
    /** @brief Bound title, session, machine and owner identity. */
    std::string game, session, machine, owner;
};
}
