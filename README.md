# cna-gamer-services-server

Independent C++23 CNA service inspired by historical XNA functionality. **Not Xbox LIVE compatible** (protocol, accounts, assets, wire or binary). Partial implementation: persistent accounts/title-scoped revocable authentication, profiles,
mutual friend requests and presence, achievements, immutable pictures/assets and paged leaderboard
reads, authenticated local gameplay commits, rotating refresh credentials and heartbeat.
The control-only PlayerMatch/Ranked session directory now supports authenticated multi-local
membership, sparse property filtering, host revisions/leases and restart. CNA public online sessions,
CNA ENet relay integration, platform keychains, push events and
avatar distribution remain unfinished. The separate server WSS endpoint now forwards authenticated
bounded datagrams; that alone does not prove Internet multiplayer. Do not call this complete or production-hardened.

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
sender counters and relay ticket/grant records. `reset-online` removes that title's sessions/membership/invitations through FK
cascade; accounts, achievements, leaderboard data and independent invite abuse counters remain.
Neither operation is remotely exposed by the control service.

GS-007c validation checkpoint (2026-09-28): GCC14 clean build with `-Werror`, two compile jobs;
CTest **4/4 passed in 65.71s**: service unit 5,216 assertions, directory 99 assertions, invitations
153 assertions, verified-TLS multi-process/restart suite. The TLS suite runs separate service
clients, both online directory types and recipient-confirmed private-slot joins, and administration
reset checks. This proves service control/membership behavior, not public XNA online sessions
or Internet realtime connectivity. The CNA protocol copy/drift gate is synchronized independently.

GS-007d real CNA control probe: set `CNA_SERVICE_DIRECTORY_CLIENT_HARNESS` to CNA's
`cmake-build-debug/cna_service_directory_client_harness`; `service_cna_directory` explicitly skips
with return 77 when absent. It runs two independent CNA processes with standard Guide sign-in and
four separate accounts, both directory categories/two titles, real server restart and recipient
confirmation, property filtering, ordinary/invited private membership, retries/departure, deliberately
expired owner and secondary access credentials, secondary-only revocation at Dispatcher.Update,
and the existing multi-local leaderboard scope refresh path. Directory operations use CNA's
private typed backend; this is not a standard-API online NetworkSession acceptance game.

Full measured integration gate (2026-09-28): `CNA_SERVICE_CLIENT_HARNESS=<CNA C++ probe>
CNA_SERVICE_C_API_HARNESS=<pure C probe> CNA_SERVICE_DIRECTORY_CLIENT_HARNESS=<directory probe>
ctest --test-dir build --output-on-failure`: **5/5 passed in 75.37s**. Unit 5,216 assertions;
directory 99; invitation 153; general TLS E2E 59.24s; two-CNA control E2E 6.85s
(31 join/16 host checks per directory category). No server production code changed in this step.

GS-008a1 adds the canonical independent binary realtime envelope, specification and golden vectors:
[relay protocol](protocol/relay-v1.md). Bounded zero-copy parsing covers ENet's 4,096-byte maximum
MTU and rejects unknown version/type/reserved bytes, empty/oversize payloads and invalid machines.
Server relay protocol test passes **10,027 assertions**, including 10,000 deterministic mutations.
Service/directory/invitation/relay unit gate **4/4 passed in 7.52s**, clean `-Werror` build.
CNA independently runs the same golden corpus and a mutation suite and checks exact source drift.
This slice does not implement a WebSocket endpoint, authentication tickets or forwarding, does not
advertise relay capability and does not prove Internet multiplayer. Those are immediate next tasks.

GS-008a2 schema 8 adds SHA-256-only one-use 60s relay ticket records and server-owned connection
grants for the exact title/session/machine/local group. All local users must supply live credentials;
only the machine owner may request authority. Grants bind revocable refresh families so normal
access-token rotation is safe, and revalidation rejects family expiry/revocation, privilege loss,
leave/host expiry and grant expiry. Prepared queries/transactional consumption, release and
per-machine/title caps are covered by **60 authority assertions**. Back up before schema upgrade.

Full integration with configured native C++/C/directory CNA probes: **7/7 CTest passed in 74.10s**:
5,217 service assertions; 99 directory; 153 invitations; 10,027 relay framing; 60 authority;
general TLS 57.18s; two-CNA control/ticket issuance 6.53s (32 join/17 host checks per kind).
Clean `-Werror` build, no skipped test. Capability `relay-tickets` means issuance only; WSS forwarding
and Internet realtime connectivity are still pending and `relay` is not advertised. See
[relay authority contract](protocol/relay-v1.md).

GS-008b server relay uses a separate verified-WSS `/cna/relay/v1` endpoint, encrypted one-use
credential hello, exact local-group authority, server-injected source IDs and title/session-only
routing. Per-socket strands serialize reads/writes; queue reservations precede executor posting,
including in-flight writes. Authentication/idle/grant/rate/resource limits and safe close semantics
are in [the canonical relay specification](protocol/relay-v1.md). Only explicit numeric-loopback
development may use plain WS. Capability `relay` identifies this server endpoint; CNA's private
WSS/ENet bridge and public online NetworkSession integration remain unfinished.

Independent WSS test client requires Python `websockets==15.0.1` (BSD-3-Clause, tests only);
`tests/requirements.txt` pins it. The runtime server does not use Python. Test configuration:

```sh
ctest --test-dir build -R '^service_relay_(protocol|authorization|flow|wss)$' --output-on-failure
```

Tests include bounded/concurrent queue producers, rate windows, exact hello/golden validation,
verified CA/hostname refusal, UTF-8/version/type/size errors, two multi-local machine groups in both
session categories/titles, bidirectional binary forwarding, maximum fragmented datagrams, foreign
session/title isolation, one-use/duplicate registration, repeated reconnect, slow-recipient queue
overflow, directory lease loss and secondary-account revocation. They do not yet run CNA ENet
under isolated routing/NAT or unblock a standard-API online XNA sample. The matching-build validation checkpoint below records results; prior GS-008a counts are history.

GS-008b matching-build checkpoint (2026-09-28): clean GCC14 `-Werror` incremental build;
configured native C++/C/directory CNA probes and CTest **9/9 passed in 120.48s**, no skipped tests.
Service 5,217 assertions; directory 99; invitation 153; relay framing 10,027; authority 60;
flow/queue/rate/hello/routing 725. General TLS/restart/CNA probe suite 56.96s; two-CNA control
probe 6.47s (32 join/17 host checks per category); WSS 259 checks/47.36s. No CNA ENet or
Internet multiplayer proof in this checkpoint. Logs: `build/relay-wss-build.log` and
`build/relay-wss-final-matching.log`. The exact-limit empty continuation buffer fix and the
slow-consumer fixture deadline correction are retained by meaningful wire tests.
