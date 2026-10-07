// SPDX-License-Identifier: MIT
#pragma once
#include <string>
namespace CnaService {
/** @brief Runs the bounded HTTP/TLS listener. @param database SQLite path.
 * @param address Numeric bind address. @param port TCP port. @param certificate TLS certificate.
 * @param key TLS private key. @param insecureLoopback Explicit loopback-only development switch.
 * @param diagnosticsEnabled Whether to expose local health and metrics. @param diagnosticsAddress
 * Numeric loopback address. @param diagnosticsPort Local HTTP port. @param jsonLogging Emit JSON
 * operational logs instead of compact human-readable lines. */
void listen(const std::string& database,const std::string& address,unsigned short port,
            const std::string& certificate,const std::string& key,bool insecureLoopback,
            bool diagnosticsEnabled,const std::string& diagnosticsAddress,
            unsigned short diagnosticsPort,bool jsonLogging);
}
