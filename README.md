# cna-gamer-services-server

An independent C++23 service for CNA, the C++ reimplementation of the XNA 4.0 programming model.
It provides what XNA games reached through their online service: accounts and Guide sign-in, gamer
profiles and pictures, friends and presence, achievements, leaderboards with Ranked arbitration,
messages and player reviews, parties, avatars, the online session directory with invitations and
host migration, an event channel that pushes change hints, and the relay that carries online
NetworkSession traffic.

**It is not Xbox LIVE compatible.** Protocol, accounts, assets and binaries are CNA's own; no Xbox
LIVE wire format, service binary or proprietary asset is used or reproduced. Original code is MIT
([LICENCE](LICENCE)); dependencies keep their own licenses ([THIRD_PARTY.md](THIRD_PARTY.md)).

Specifications: [control protocol v1](protocol/v1.md) (every operation, argument, limit and error)
and [relay protocol v1](protocol/relay-v1.md). CNA vendors their headers, parser and golden vectors
and checks them for drift (`tools/net/check_service_protocol.py` in CNA).

## What it provides

The `hello` operation advertises these capabilities; CNA refuses to use a feature whose capability
is missing.

| Capability | What it covers |
|---|---|
| `identity`, `authentication`, `session-refresh`, `heartbeat` | Title-scoped sign-in (scrypt verifiers), one-hour access tokens, rotating 30-day refresh families with replay revocation, `auth.ping` |
| `friends`, `friend-requests`, `presence`, `presence-status` | Mutual friend requests, per-title rich presence, online/away/busy, joinable and invitation flags |
| `game-defaults`, `gamer-zone` | Account-wide XNA `GameDefaults`; gamer zone; reputation from player reviews |
| `achievements`, `assets`, `files` | Title achievement catalogs with pictures; immutable hash-addressed PNG/GLB assets, read in chunks or as raw bytes (`GET /cna/v1/files/<sha256>`) |
| `leaderboard-reads`, `local-leaderboard-commit`, `leaderboard-epoch-abort`, `ranked-arbitration` | Paged, centered and restricted reads; commits at XNA gameplay boundaries; Ranked rounds reconciled from every machine's report |
| `messages`, `player-reviews` | Guide messages and the player review pane |
| `avatars`, `avatar-catalog-packs` | One validated avatar description and revision per account, negotiated per client: the stored avatar when the client has (or will install) its catalog, else a marked projection; each catalog version described as one installable pack |
| `session-directory`, `session-removal`, `host-migration`, `session-add-members`, `session-invitations`, `join-friend` | PlayerMatch and Ranked directory, `RemoveFromSession`, host migration, `AddLocalGamer`, persistent invitations, joining a friend's or party member's game |
| `parties` | Account-level parties of up to 8 friends: invitations, members with presence and joinable games (XNA `PartySize`, `ShowParty`, `ShowPartySessions`, `SendPartyInvites`) |
| `leaderboard-list`, `title-version` | The title's boards for the Guide's leaderboard page; a title's oldest accepted game version |
| `events` | The account event channel (WSS `/cna/v1/events`): hints `invitations`, `messages`, `friends`, `party`, sent after the request that changed something succeeded; clients re-read and keep polling as the fallback |
| `relay-tickets`, `relay` | One-use tickets and the WSS relay for ENet datagrams (game data and NetworkSession voice alike) |

Push is hints only: the session directory, presence, leaderboards and achievements are read, not
pushed, and a lost hint costs one poll interval. Voice media is not a control-plane feature: the
control protocol has no voice operation, and CNA's NetworkSession voice travels through the relay as
ordinary ENet datagrams the server never inspects.

Not provided: Xbox LIVE compatibility of any kind; account self-registration or password change
over the wire (the administrator provisions accounts; gamer pictures are set by the administrator,
mottos and regions not at all); privacy and block settings; TrueSkill computation; time windows for
the `...Recent` leaderboard keys; direct peer-to-peer connections (every online datagram goes
through the relay); a store (Marketplace), PartnerToken and title update delivery. Linux is the
tested server host; Windows and macOS builds are unvalidated. CNA's
`docs/gamer-services-known-limitations.md` lists every limitation, client and server.

## Build and run

Dependencies: OpenSSL >=3, Boost >=1.74 (headers, Beast), SQLite >=3.38, nlohmann/json >=3.11,
all from the system.

```sh
cmake -S . -B build -G Ninja && cmake --build build --parallel
build/cna-gamer-services-server --database service.sqlite3 --listen 0.0.0.0 --port 47831 \
    --cert certificate.pem --key private-key.pem
# Development only, plain HTTP on a numeric loopback address:
build/cna-gamer-services-server --database service.sqlite3 --insecure-loopback
```

CNA finds the service through `CNA_GAMER_SERVICES_ENDPOINT` (for example
`https://games.example.org:47831/cna/v1`), `CNA_GAME_ID` (a title ID registered below) and, for a
private CA, `CNA_GAMER_SERVICES_CA_BUNDLE`; see CNA's `docs/gamer-services-server.md`.

## Administration

`build/cna-gamer-services-admin <database> <command> ...` works on the same database as a running
server and never prints a credential. Passwords and JSON documents come from stdin.

| Command | Effect |
|---|---|
| `title <id> <name>` | Registers a title; clients name it in every request |
| `title-minimum-version <id> <version>` | The oldest game version the title accepts (`1.2.0`; empty accepts every version): older clients, and clients that state none, get `UPDATE_REQUIRED` (XNA `GameUpdateRequiredException`) |
| `user <username> <gamertag>` | Creates an account; password on stdin (use a secret input, shell tracing off) |
| `achievement <title>` | Adds an achievement from JSON on stdin (`key`, `name`, `description`, `howToEarn`, `score`, optional `picture` hash) |
| `leaderboard <title>` / `seed-leaderboard <title>` | Defines a board / inserts a fixture row, JSON on stdin |
| `asset <title> <mime> <file>` | Imports a PNG (<=512x512, <=512 KiB) or GLB (<=16 MiB); prints its hash |
| `picture <username> <hash>` | Sets a gamer picture |
| `avatar-catalog <directory>` | Imports and fully validates a CNA avatar catalog (`assets/avatars/v1`, `v2`, `v3`) |
| `avatar <username> random [male\|female]`, `clear` or `set` | Gives an account an avatar (`set` reads the description hex from stdin) |
| `game-defaults <username>` | Sets the account's XNA GameDefaults, JSON on stdin |
| `inspect` / `inspect-online <title>` | Counts only: users, titles, sessions, earned; directory, invitations, relay records |
| `revoke-user <username>` | Revokes every credential of an account |
| `expire-access <username>` | Expires access tokens but keeps refresh authority (maintenance tests) |
| `reset-earned <title>` / `reset-online <title>` | Clears a title's earned achievements / its directory sessions, memberships and invitations (accounts, scores and abuse counters stay) |

## Operating it

**Files.** One SQLite database in WAL mode (`service.sqlite3`, `-wal`, `-shm`) plus the TLS key.
Restrict both to the unprivileged service user. Back up with SQLite's online backup
(`sqlite3 service.sqlite3 ".backup backup.sqlite3"`), never by copying the file while it runs.
Opening a database migrates it transactionally to the current schema (20); a newer schema is
refused and nothing is deleted to resolve it. Back up before upgrading the server.

**TLS.** TLS 1.2 or newer with the given chain and key; clients verify chain and hostname. A new
certificate needs a restart. Clients reconnect by themselves, and live online sessions resume their
relay connections (tested by `service_cna_session_restart`).

**Durability.** Commits use `synchronous=NORMAL`: crash-safe, but a power loss may roll back the
last moments of leases, presence and request bookkeeping. Everything a player would notice losing
(credentials and revocations, awards, scores, friends, messages, profile and avatar edits) commits
with `synchronous=FULL`.

**Limits.** 256 control connections, at most 32 from one address; 1024 authenticated relay
machines, counted apart. A connection carries up to 1000 requests, idling at most 5 s between
them; a CNA client holds one only around its bursts, so an address's 32 connections are shared by
the players behind one NAT address rather than owned by 32 of them. Sign-in and refresh: 10 per minute per address. Messages up to 64 KiB. Request IDs that
guard mutations against replay: 1,000,000 per title and 20,000 per account per day (reads,
heartbeats, lease and presence updates record none). Past 32 live sign-ins an account's oldest
refresh family is signed out. The server raises its descriptor limit to what the host allows
(up to 65536) and keeps serving when descriptors run out.

**Monitoring.** One line a minute on stdout, never with a credential, address or request body:

```
stats AUTHENTICATION_FAILED=1 OK=5412 RATE_LIMITED=3 refused=0 control=41 relays=12 events=37
```

It gives response counts per error code since the previous line, connections refused by admission,
open control connections, attached relay machines and open event channels. `INTERNAL_ERROR` means storage failed (disk
full, a locked database); the response to the client never carries details. SIGINT and SIGTERM
stop the server.

**One process.** The service is one process with one SQLite writer and in-memory hubs for relay
channels and event channels. It does not scale across machines: a second node would need shared
relay routing, event delivery between nodes and distributed session ownership, not only a shared
database. A second server process on the same database refuses to start
(`DATABASE_IN_USE`: the first holds an advisory lock on `<database>.lock` until it exits); the admin
tool deliberately takes no lock and works beside a running server. Back up a running server with
SQLite's online backup, which is consistent under WAL: `sqlite3 service.sqlite3 ".backup
service-backup.sqlite3"`. One address may hold 32 control connections and open 600 new ones a
minute (each costs a TLS handshake); put a firewall or proxy in front of the server against floods
from many addresses.

## Before exposing it on the public internet

The server is built to face untrusted clients, but a public deployment still needs these, in order:

1. **An account of its own.** Run it as an unprivileged user that owns only the database directory
   and the TLS key (mode 0600); under systemd, `NoNewPrivileges=yes`, `ProtectSystem=strict`,
   `ReadWritePaths=` the database directory, `Restart=on-failure`.
2. **A public certificate** for the DNS name the games use, with its intermediates in the chain file.
   Clients verify the chain and the hostname against the system trust store (or the CA bundle a
   title ships). A renewed certificate needs a restart: schedule it; clients and relays reconnect.
3. **One open port.** Control requests, the relay and the event channel share the TLS port. The
   server terminates TLS itself: put an L4 (TCP) filter in front, not a TLS-terminating proxy, which
   would also make every player appear to come from the proxy's address and share its per-address
   limits.
4. **Flood protection in front.** Per-address limits stop one address (32 connections, 600 new ones
   and 10 sign-ins a minute); floods from many addresses need a firewall or DDoS filter upstream.
5. **Titles and accounts.** Create each title (`title`), import its achievements, leaderboards and
   avatar catalogs, and set `title-minimum-version` when old game builds must update. Accounts are
   created by the operator (`user`); there is no self-registration and no password reset by mail.
6. **Backups** with SQLite's online backup on a schedule, a restore tried once, and a backup taken
   before every upgrade (migrations only go forward).
7. **Monitoring** of the per-minute statistics line: alert on `INTERNAL_ERROR`, on `refused` rising
   and on `RATE_LIMITED` bursts.
8. **Capacity.** One process, one machine (see below): about 1,300 requests a second on the reference
   machine, while an idle player costs about two a minute.

## Capacity

`tests/service_benchmark.py` provisions a scratch title, starts the server and drives it from one
process per player, each from its own loopback address. Scenarios: `steady` (ordinary
authenticated mix, no think time), `logins` (the same plus eight clients signing in continuously),
`idle` (one address holding 160 idle connections), `replay` (the title's request-ID table filled to
200,000) and `descriptors` (a server limited to 48 descriptors facing 80 idle connections).

```sh
cmake -S . -B build-probe -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-probe
python3 tests/service_benchmark.py build-probe --keep-alive --replay-fill 200000 --json result.json
```

Release build, 64 players, 15 s windows, a 16-core Linux machine shared with other work, loopback,
2026-09-29. "Before" is the server at `a1b2a51`, "after" includes the audit fixes below:

| Scenario | Before | After |
|---|---|---|
| steady | 73 req/s, p50 703 ms, p99 3.6 s | 1,602 req/s, p50 38 ms, p99 67 ms |
| sign-in storm | 42 req/s; everything p50 1.47 s | 1,193 req/s; ordinary p50 49 ms; sign-in p50 373 ms, about 11/s |
| one address, 160 idle connections | 60 of 64 players could not sign in | no failures, 1,440 req/s |
| full request-ID table | 188 req/s at 90,000 IDs (with the fsync fix already in) | 1,964 req/s at 200,000 IDs |
| descriptors exhausted | server exited | keeps serving |

Re-run at `4bc685c` (catalog packs, title versions, parties), same command, with the machine
busier (load average about 4.3 from other work): steady 1,312 req/s, p50 44 ms, p99 73 ms; sign-in
storm 1,024 req/s, sign-in p50 408 ms; 160 idle connections 1,127 req/s; 200,000 request IDs
1,507 req/s; descriptors exhausted, still serving; no errors in any scenario. In the same minutes the
server before this work (`4ba7f95`) measured 1,329 and 992 req/s steady, so the lower figures are
the load, not the new operations.

These are closed-loop saturation figures. A player idling in a menu costs about two requests a
minute (the heartbeat), so the real ceiling depends on what games do between heartbeats. Password
sign-in is expensive by design; returning players renew with refresh tokens, which cost no
derivation.

## Production audit (2026-09-29)

| Finding | Status |
|---|---|
| Every request ran on one of two network threads under one global lock, and sign-in held that lock through its scrypt derivation: one sign-in stalled every request, TLS handshake and relay datagram | Fixed `171b8cf`: worker pools; derivation outside the lock |
| An accept error (`EMFILE`) ended the whole server | Fixed `171b8cf` |
| Two or three fsynced commits per request (5.4 ms each on the reference disk) capped the server near 73 req/s | Fixed `5b71f19`: durability classes, one bookkeeping transaction |
| The 30 s heartbeat spent a title's 100,000 daily request IDs after about 35 always-on players; one account could lock a title for a day; the ID count scanned the whole table on every request | Fixed `5b71f19`: IDs only for mutations, per-account budget, cached count |
| A sign-in burst queued ordinary requests behind scrypt | Fixed `512e0b9`: separate sign-in pool |
| 128 connections shared by HTTP and relays: one host with idle connections locked everyone out, and the server relayed at most 96 machines | Fixed `fb43d9a`: per-address admission, relays counted apart, 1024 relays |
| A TCP and TLS handshake per request | Fixed `dcca1b4` (server) and CNA's reused curl handle |
| A 33rd sign-in within 30 days was refused, locking out players on clients without credential storage | Fixed `541679c`: the oldest family is signed out |
| No operational output at all | Fixed `cb1d6e4`: per-minute statistics line |
| One process, one SQLite writer, in-memory relay and event hubs: no horizontal scaling | Retained |
| Floods from many addresses can still fill the 256 control connections; no handshake rate limit | Retained: needs a firewall or proxy in front |
| The per-account request budget lives in memory and starts over when the server restarts | Retained |
| A new TLS certificate needs a restart (clients and relays reconnect) | Retained |
| Clients poll; there is no push | Fixed `e3e78a8`: an account event channel sends hints, clients still poll as the fallback |
| Players behind one NAT address share its 32 control connections and its 10 sign-ins a minute (a LAN party signing in at once waits) | Retained: per-address limits are the flood protection; keep-alive idles only 5 s |

## Tests

`ctest --test-dir build --output-on-failure` runs the unit suites (service, directory,
invitations, arbitration, avatars, social, relay protocol/authority/flow), the TLS end-to-end test
with separate Python clients and restart persistence, the WSS relay test (Python `websockets`,
pinned in `tests/requirements.txt`) and the benchmark smoke pass.

The tests that drive real CNA processes need CNA's harnesses from a CNA build; each one skips
(exit 77, never a pass) when its harness is not configured:

```sh
CNA=/absolute/path/to/CNA; B=$CNA/cmake-build-debug
CNA_SERVICE_CLIENT_HARNESS=$B/cna_service_client_harness \
CNA_SERVICE_C_API_HARNESS=$B/cna_c_api_service_client \
CNA_SERVICE_DIRECTORY_CLIENT_HARNESS=$B/cna_service_directory_client_harness \
CNA_SERVICE_RELAY_CLIENT_HARNESS=$B/cna_service_relay_client_harness \
CNA_SERVICE_SESSION_CLIENT_HARNESS=$B/cna_service_session_client_harness \
CNA_SERVICE_AVATAR_CLIENT_HARNESS=$B/cna_service_avatar_client_harness \
CNA_AVATAR_CATALOGS=$CNA/modules/gamer-services/assets/avatars \
ctest --test-dir build --output-on-failure
```

They cover Guide sign-in, social flows, pictures and rich presence through the standard XNA API
(`service_tls_e2e`); the directory and public `BeginFind` (`service_cna_directory`); raw and owned
ENet over the relay (`service_cna_relay`, `service_cna_owned_enet`); the public NetworkSession
lifecycle for PlayerMatch and Ranked (`service_cna_session`), invitations (`service_cna_invite`),
service restart mid-session (`service_cna_session_restart`), host migration and host crash
(`service_cna_session_migration`, `service_cna_session_host_crash`), `AddLocalGamer`
(`service_cna_session_add_gamer`) and avatars with an installed catalog pack and a projection (`service_cna_avatars`).

The `_nat` variants and the host-crash and AddLocalGamer tests run each CNA process in its own
rootless user and network namespace behind an outbound-only NAT helper, with identical private
addresses and no inbound ports, so game data can only travel through the relay. They need
unprivileged user namespaces, `unshare`, `ip` and a `slirp4netns` binary, which is executed, never
linked:

```sh
CNA_SERVICE_SLIRP4NETNS=/absolute/slirp4netns \
CNA_SERVICE_SLIRP_LIBRARY_PATH=/optional/unpacked/library/path \
ctest --test-dir build -R '_nat$|host_crash|add_gamer' --output-on-failure
```

Tested with Debian slirp4netns 1.2.1 and libslirp 4.8.0 unpacked outside the repository; their
licenses are in [THIRD_PARTY.md](THIRD_PARTY.md). This is NAT-isolation evidence on one Linux host,
not a measurement of public Internet latency or failover: public deployment is documented (above)
but has not been independently qualified between genuinely remote networks.

### Avatars

Import each catalog CNA ships, then give accounts avatars:

```sh
build/cna-gamer-services-admin service.sqlite3 avatar-catalog $CNA/modules/gamer-services/assets/avatars/v1
build/cna-gamer-services-admin service.sqlite3 avatar-catalog $CNA/modules/gamer-services/assets/avatars/v2
build/cna-gamer-services-admin service.sqlite3 avatar-catalog $CNA/modules/gamer-services/assets/avatars/v3
build/cna-gamer-services-admin service.sqlite3 avatar alice random male
```

The service stores one description and revision per account, never avatar geometry per account.
A CNA client draws the catalogs of its own release locally. When an avatar names a catalog the
client lacks, the client installs that catalog as one pack (`avatars.catalogPack`, then the
manifest and the files it lacks through the file route), validates it whole and activates it at
once; a client that declines updates is given a projection onto a catalog it has.
`service_cna_avatars` proves both with the standard XNA API, and `service_avatar_validation` uses
CNA's catalogs as golden fixtures while refusing malformed models and manifests.
