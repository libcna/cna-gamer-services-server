# Architecture

This page is the source-oriented mental model for maintainers. The protocol specifications remain
normative; this page explains where the implementation lives and which ownership rules matter.

## Process model

```text
                         one cna-gamer-services-server process

 clients ── TCP/TLS ──> Listener.cpp ── HTTP POST /cna/v1 ──> worker pools
                              │                                  │
                              ├── WSS /cna/v1/events ──> EventHub│
                              └── WSS /cna/relay/v1 ──> RelayHub │
                                                                 v
                                                   Service (one mutex)
                                                                 │
                                                   Store / one SQLite handle
                                                                 │
                                                         WAL database
```

`src/Main.cpp` validates process arguments and acquires `DatabaseLock` for the lifetime of the
server. `src/Listener.cpp` owns the Asio context, TLS policy, admission control and worker pools.
The listener has two network threads. Four request workers execute storage-bound work and two
separate workers absorb expensive login scrypt derivations. The latter derivation deliberately
releases `Service::mutex_`; all SQLite work remains serialized.

The administrator executable opens the same database without the server lock. SQLite serialization
and prepared statements make supported concurrent administrative operations safe; it is not a
second runtime authority and does not own relay or event state.

## Control request path

1. `Listener.cpp` bounds headers and bodies before application parsing.
2. `Protocol.cpp::parse` rejects invalid JSON, duplicate keys, deep nesting and oversized
   containers; `validateRequest` validates the exact v1 envelope.
3. `Service::handle` converts every public failure to a stable protocol code and records aggregate
   outcome counts without logging credentials or bodies.
4. `Service::dispatch` negotiates the title/version, authenticates the access token, starts the
   request transaction and applies replay/outcome semantics.
5. Domain modules implement the operation: `Authentication.cpp`, `Leaderboards.cpp`,
   `SessionDirectory.cpp`, `Invitations.cpp`, `Parties.cpp`, `Privacy.cpp`, `Avatars.cpp` and the
   smaller operations in `Service.cpp`.
6. A successful transaction commits before event hints are offered. Event delivery can therefore
   be lost without making the hint disagree with committed state.

The single service mutex is intentional. It gives one obvious ordering boundary around a single
SQLite writer and the in-memory request/rate caches. Do not remove it piecemeal: doing so changes
transaction, statement and cache ownership simultaneously.

## Persistence and durability

`Store.cpp` opens SQLite in FULLMUTEX mode, enables foreign keys and WAL, reads `user_version`, and
applies each numbered migration in its own `BEGIN IMMEDIATE` transaction. A database newer than
`Store::SchemaVersion` is refused; it is never reset or downgraded.

`Store::Transaction` uses an outer `BEGIN IMMEDIATE` and nested savepoints. This is why domain
helpers can preserve their own all-or-nothing rule while the request ID, mutation and outcome still
commit together.

`Service.cpp` classifies operations before opening the request transaction:

- replaceable leases, presence, directory state and request bookkeeping use
  `synchronous=NORMAL`;
- credentials/revocations, achievements, leaderboards, social data, profiles and avatars use
  `synchronous=FULL`.

This is an explicit product guarantee, not a performance tuning accident. Changes to the
`Ephemeral` set need a durability review and a crash/restart test.

## Authentication and replay protection

`Authentication.cpp` stores scrypt verifiers and hashes of access/refresh credentials, never bearer
plaintext. A refresh family rotates atomically. Reuse of an old refresh credential revokes that
family and its access sessions.

For replay-sensitive mutations, `Service::dispatch` stores the request ID, account, operation,
outcome and a bounded non-secret result in the same transaction as the mutation. A crash before
commit preserves neither; a crash after commit preserves both. Secret-bearing results are never
stored, so repeating login, refresh or relay-ticket issuance returns `DUPLICATE_REQUEST`.

## Directory, invitations and arbitration

`SessionDirectory.cpp` owns persistent logical sessions, machines, members, leases and host
migration. Directory membership is not network connectivity. A relay grant is issued only for the
complete current local-machine group.

Invitations are persistent, title-bound references to a live directory session. Acceptance does not
reserve capacity. `sessions.joinInvited` validates and consumes the invitation in the membership
transaction.

Ranked Lobby-to-Playing transitions snapshot the exact accounts and machines into an arbitration
round. Rounds intentionally do not reference the live directory, allowing legitimate reports after
members leave or the directory closes. `Leaderboards.cpp` resolves only strict-majority identical
rows and applies the board's existing aggregation policy.

## Realtime relay

`RelayAuthorization.cpp` issues hashed, one-use, 60-second tickets and turns a redeemed ticket into a
server-owned grant. A grant binds title, session, machine, owner and every local account's refresh
family. It is revalidated every five seconds.

`RelayListener.cpp` owns each WebSocket and its strand. `RelayFlow` bounds ingress windows and the
cross-strand write queue. `RelayHub` keys routes by `(title, session, machine)`. A client frame names
only its destination; the server encodes the authenticated source into the forwarded frame.
Disconnecting removes only the grant and shortens the directory-machine lease to the documented
reconnect grace period.

## Event hints

`EventListener.cpp` authenticates an access token, registers up to eight channels per account and
coalesces pending topics. `EventHub` is process memory. There is no sequence number, persistence or
replay because hints carry no state; clients must retain polling/read fallback.

## Concurrency ownership

| Component | Ownership rule |
|---|---|
| `Store` and SQLite handle | Accessed only while `Service::mutex_` is held, except the login scrypt computation, which performs no SQLite work |
| Request/rate/catalog/download caches | Protected by `Service::mutex_` |
| Outcome counters | Separate `outcomesMutex_` because the reporter drains them independently |
| `RelayHub` and `EventHub` maps | Each has its own mutex; callbacks retain shared channel ownership outside the lock |
| Relay socket state | Its Asio strand; cross-strand offers first reserve capacity in `RelayQueue` |
| Event pending topics | Per-connection mutex; writes run on the socket executor |
| Admission history/counts | `Admission` mutex |

## Invariants to preserve

- Exactly one runtime server owns a database; admin access is not runtime authority.
- A title never observes another title's credentials, directory, leaderboard or relay routes.
- Persistent state is authoritative; events and live hub membership are derived/transient.
- Request replay records and their mutations commit together.
- Plaintext passwords, access tokens, refresh credentials and relay tickets never enter logs,
  metrics or persistent result records.
- Relay source identity is server-injected and cannot be supplied by a client.
- Session membership, relay authority and realtime handshake completion are distinct states.
- Ranked round authority is a historical snapshot and may outlive the directory.
- Schema migrations only move forward and preserve an unsupported newer database untouched.

The most useful executable evidence is in `tests/AtomicityTests.cpp`, `tests/DirectoryTests.cpp`,
`tests/RelayAuthorizationTests.cpp`, `tests/RelayFlowTests.cpp`, `tests/PrivacyTests.cpp`,
`tests/ConcurrencyLifetimeTests.cpp` and the Python TLS/WSS end-to-end tests.
