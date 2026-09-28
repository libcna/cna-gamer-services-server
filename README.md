# cna-gamer-services-server

Independent C++23 CNA service inspired by historical XNA functionality. **Not Xbox LIVE compatible** (protocol, accounts, assets, wire or binary). Early implementation: persistent accounts/title-scoped revocable auth, profiles, directed friends/presence and achievement catalog/awards. No matchmaking/relay, refresh credentials, push events, asset download, leaderboards or avatar distribution yet. Do not call this complete or production-hardened.

Dependencies: OpenSSL >=3 (Apache-2.0), Boost >=1.74 (BSL-1.0), SQLite (public domain), nlohmann/json >=3.11 (MIT), all system dependencies. Service code MS-PL as CNA. Linux is the tested host; Windows/macOS TLS client/server builds still need validation.

```sh
CCACHE_DIR=/rv/cnaccache CCACHE_BASEDIR=/rv cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
CCACHE_DIR=/rv/cnaccache CCACHE_BASEDIR=/rv cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
build/cna-gamer-services-admin development.sqlite3 title sample 'Sample title'
# Password comes from stdin, never command arguments. Use a secret input source with shell tracing off.
build/cna-gamer-services-admin development.sqlite3 user alice Alice
# Provide catalog JSON on stdin:
build/cna-gamer-services-admin development.sqlite3 achievement sample < achievement.json
build/cna-gamer-services-server --database development.sqlite3 --listen 127.0.0.1 --port 47831 --cert certificate.pem --key private-key.pem
# Explicit unencrypted loopback testing only:
build/cna-gamer-services-server --database development.sqlite3 --insecure-loopback
```

`inspect` prints user/title/session/earned counts only. `revoke-user <username>` revokes sessions. `reset-earned <title>` resets test achievement data. SQLite migrates transactionally on first open, keeps data on restart and refuses unknown schema versions. Back up the SQLite database using SQLite backup tooling before changing schema; never delete owner data to resolve version errors. Restrict DB/key files and run as an unprivileged service user. TLS tests use generated test CA/cert, never disabled certificate verification. No password/token request logging. See [canonical protocol](protocol/v1.md) for limits and exact operations and CNA's living implementation plan for unfinished acceptance items.
