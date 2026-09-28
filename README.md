# cna-gamer-services-server

Independent C++23 CNA service inspired by historical XNA functionality. **Not Xbox LIVE compatible** (protocol, accounts, assets, wire or binary). Partial implementation: persistent accounts/title-scoped revocable authentication, profiles,
mutual friend requests and presence, achievements, immutable pictures/assets and paged leaderboard
reads, authenticated local gameplay commits, rotating refresh credentials and heartbeat.
The control-only PlayerMatch/Ranked session directory now supports authenticated multi-local
membership, sparse property filtering, host revisions/leases and restart. CNA public online sessions,
relay, platform keychains, push events and
avatar distribution remain unfinished. Do not call this complete or production-hardened.

Dependencies: OpenSSL >=3 (Apache-2.0), Boost >=1.74 (BSL-1.0), SQLite (public domain), nlohmann/json >=3.11 (MIT), all system dependencies. Original service code is MIT. Linux is the tested host; Windows/macOS TLS client/server builds still need validation.

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
boards; gameplay setters are transient and authorized local commits flush at EndGame or explicit early leave. No Ranked,
TrueSkill or Ranked arbitration capability is claimed yet. Schema 4 adds authenticated local game
epochs and atomic/idempotent EndGame/early-leave commits; authenticated abort releases interrupted
epochs. Offline crash cleanup is best effort and expires by the documented quota/lifetime.
SQLite >=3.38 supplies JSON table filtering.

Original server source, protocol and tests use the [MIT license](LICENCE). See
[dependency notices](THIRD_PARTY.md) for the independently licensed transport/database/JSON libraries.


Schema 5 adds hashed rotating access/refresh families (30-day absolute refresh lifetime, one-hour
access tokens), replay-family revocation and authenticated heartbeat. Normal logout revokes its
refresh family; `revoke-user` covers all user families and legacy access. Development
`build/cna-gamer-services-admin service.sqlite3 expire-access alice` expires Alice's access tokens
while preserving refresh authority, for maintenance/reconnect tests. CNA now consumes refresh, pumps heartbeat, and can resume four local slots from private POSIX
user storage. Other client platforms retain ephemeral credentials pending a secure provider. The
server does not supply the endpoint before clients connect. Lost
rotation responses can require fresh Guide sign-in. No credential logging or plaintext bearer
persistence on the server. See the canonical protocol for caps, migration and security semantics.


Schema 6 adds a persistent bounded session directory. Directory membership is control state;
it grants neither ENet connectivity nor Ranked arbitration. Host/member leases are 90 seconds,
renewed separately from account heartbeat. Expired host closes its directory; migration is pending.
See the session-directory protocol section for exact request fields, limits and failures. CTest
includes 99 dedicated assertions and two independent TLS directory clients with four user accounts
for PlayerMatch and Ranked, including server restart/property filtering/leave. This is not proof of
Internet multiplayer; CNA XNA frontend and relay remain active implementation tasks.


Schema 7 adds authenticated persistent invitations, explicit accept/dismiss and invited multi-local
private-slot joins. The capability is control-only, with recipient/title binding, strict limits and
independent persistent abuse counters. CTest includes invitation authorization, atomicity, expiry,
replay, restart and close/recreate quota checks; TLS workers exercise receipt across server restart
and private/public allocation for both directory kinds. Public CNA Guide/InviteAccepted integration,
relay and console Ranked behavioral verification remain unfinished. Migration runs transactionally
on opening a schema-6-or-earlier database; back up the SQLite database before upgrading deployments.

Administration for isolated development state (no credentials printed):

```sh
build/cna-gamer-services-admin service.sqlite3 inspect-online your-title
build/cna-gamer-services-admin service.sqlite3 reset-online your-title
```

`inspect-online` reports JSON counts for directory sessions, members, retained invitations and
sender counters. `reset-online` removes that title's sessions/membership/invitations through FK
cascade; accounts, achievements, leaderboard data and independent invite abuse counters remain.
Neither operation is remotely exposed by the control service.

GS-007c validation checkpoint (2026-09-28): GCC14 clean build with `-Werror`, two compile jobs;
CTest **4/4 passed in 65.71s**: service unit 5,216 assertions, directory 99 assertions, invitations
153 assertions, verified-TLS multi-process/restart suite. The TLS suite runs separate service
clients, both online directory types and recipient-confirmed private-slot joins, and administration
reset checks. This proves service control/membership behavior, not public XNA online sessions
or Internet realtime connectivity. The CNA protocol copy/drift gate is synchronized independently.
