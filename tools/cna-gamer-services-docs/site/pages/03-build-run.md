---
title: Build, local run and production startup
summary: Reproducible ccache builds, qualification presets, local development, production TLS and the CNA configuration boundary.
order: 3
---

## Dependencies and supported evidence

The build needs CMake 3.25+, a C++23 compiler, Ninja, ccache, OpenSSL 3, SQLite 3.38+, Boost headers,
nlohmann/json and Threads. Linux is tested. macOS and native Windows are not currently validated;
portable branches exist for database locking but that is not equivalent to platform qualification.

## Maintained build commands

```sh
ccache --version
ccache --show-stats
cmake --preset dev
cmake --build --preset dev
tools/qualify.sh quick
```

The presets always set `CMAKE_CXX_COMPILER_LAUNCHER=ccache`. `dev` and `release` cap builds at six
jobs; memory-heavy `asan` and `tsan` builds run serially. `dev` is RelWithDebInfo in `build-agent`,
`release` is optimized, `asan` combines address and undefined behavior sanitizers, and `tsan` is the
thread sanitizer tree. Reuse these trees. Do not clean them
or clear ccache as a routine response to a failure.

Qualification tiers are intentional: `quick` is build plus C++ unit/component tests; `normal` runs
all configured tests; `security` runs the sanitizer unit subset serially; `performance` runs release
benchmark smoke; `full` adds every integration and documentation check. A CNA harness exit 77 is a
skip, never a pass.

## Five-minute local run

```sh
build-agent/cna-gamer-services-admin /tmp/cna-lab.sqlite3 title sample Sample
printf '%s\n' 'a-long-local-password' | \
  build-agent/cna-gamer-services-admin /tmp/cna-lab.sqlite3 user alice Alice
build-agent/cna-gamer-services-server --database /tmp/cna-lab.sqlite3 \
  --listen 127.0.0.1 --port 47831 --insecure-loopback \
  --diagnostics-port 47832
```

`--insecure-loopback` is refused on any non-loopback address. It exists only to remove certificate
friction from a local lab. Never teach a game or deployment to disable certificate validation.

## Production startup

```sh
cna-gamer-services-server --database /var/lib/cna-gamer-services/service.sqlite3 \
  --listen 0.0.0.0 --port 47831 \
  --cert /run/credentials/certificate.pem --key /run/credentials/private-key.pem \
  --diagnostics-listen 127.0.0.1 --diagnostics-port 47832 --log-format json
```

The certificate chain must match the DNS name clients use; TLS 1.2 is the floor. Keep the private
key and database mode 0600 under an unprivileged dedicated account. The diagnostics listener is
plain HTTP because it is forced to loopback. Monitor it locally or through an authenticated tunnel.

## CNA client configuration

At the compatibility boundary a title supplies its service endpoint, title identifier and optional
private CA bundle. Conceptually these are `CNA_GAMER_SERVICES_ENDPOINT`, `CNA_GAME_ID` and
`CNA_GAMER_SERVICES_CA_BUNDLE`. The endpoint includes `/cna/v1`. CNA source is not part of this
repository and must not be modified to accommodate server changes.

Old games continue to work unless the operator explicitly sets a title minimum version. Capability
discovery, not assumptions about server age, gates optional behavior.
