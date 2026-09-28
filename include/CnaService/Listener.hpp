// SPDX-License-Identifier: MS-PL
#pragma once
#include <string>
namespace CnaService {
/** @brief Runs the bounded HTTP/TLS listener. @param database SQLite path.
 * @param address Numeric bind address. @param port TCP port. @param certificate TLS certificate.
 * @param key TLS private key. @param insecureLoopback Explicit loopback-only development switch. */
void listen(const std::string& database,const std::string& address,unsigned short port,
            const std::string& certificate,const std::string& key,bool insecureLoopback);
}
