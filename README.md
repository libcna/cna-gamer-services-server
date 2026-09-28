# cna-gamer-services-server

Independent C++23 CNA service inspired by historical XNA functionality. **Not Xbox LIVE compatible** (protocol, accounts, assets, wire or binary). Partial implementation: persistent accounts/title-scoped revocable authentication, profiles,
mutual friend requests and presence, achievements, immutable pictures/assets and paged leaderboard
reads and authenticated local gameplay commits. Matchmaking/relay, refresh credentials, push events and
avatar distribution remain unfinished. Do not call this complete or production-hardened.

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

Original asset provisioning (trusted administrator paths only):
```
build/cna-gamer-services-admin service.sqlite3 asset my-title image/png original-picture.png
# prints content hash; include it as picture in achievement JSON
build/cna-gamer-services-admin service.sqlite3 picture alice <hash>
```
SQLite schema 2 adds immutable assets/title associations and user pictures; schema 1 upgrades
transactionally at open. Assets are PNG (<=512x512/512 KiB) or GLB v2 (<=16 MiB), header checked.
Complete decode/asset-catalog validation remains future work. Network callers only supply hashes
and bounded offsets, never filesystem paths. See protocol/v1.md for ACL/chunk limits.

Cross-repository integration: set `CNA_SERVICE_CLIENT_HARNESS` to the built CNA client harness
and run CTest. The test then includes two real CNA processes, standard Guide sign-in and social
flows, rich presence, revocation, immutable picture streams/cache and server restart persistence.
Without it, the TLS test still uses separate Python clients. Both are service/control tests, not
Internet realtime multiplayer/relay evidence.


Leaderboard catalog/read development (schema 3):

```sh
printf '%s' '{"key":"BestScoreLifeTime","mode":0,"ascending":false,"aggregation":"best","arbitrated":false,"columns":{"Rounds":"int32"}}' | build/cna-gamer-services-admin service.sqlite3 leaderboard my-title
printf '%s' '{"key":"BestScoreLifeTime","mode":0,"gamertag":"Alice","rating":123,"columns":{"Rounds":{"type":"int32","value":3}}}' | build/cna-gamer-services-admin service.sqlite3 seed-leaderboard my-title
```

These administration seeds are fixtures. Online clients read persisted paged, centered or restricted
boards; gameplay setters are transient until session-authorized commits are implemented. No Ranked,
TrueSkill or Ranked arbitration capability is claimed yet. Schema 4 adds authenticated local game
epochs and atomic/idempotent EndGame/early-leave commits; authenticated abort releases interrupted
epochs. Offline crash cleanup is best effort and expires by the documented quota/lifetime.
SQLite >=3.38 supplies JSON table filtering.

Original server source, protocol and tests use the [MIT license](LICENCE). See
[dependency notices](THIRD_PARTY.md) for the independently licensed transport/database/JSON libraries.
