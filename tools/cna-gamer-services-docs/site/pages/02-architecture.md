---
title: Architecture, source tour and concurrency
summary: Component responsibilities, request ownership, thread boundaries, lifetime rules and a practical reading order.
order: 2
---

## Component map

| Component | Responsibility | Start reading |
|---|---|---|
| `Main` / `Listener` | CLI, TLS, HTTP, workers, diagnostics, shutdown | `src/Main.cpp`, `src/Listener.cpp` |
| `Protocol` | bounded JSON, envelope and scalar validation, stable errors | `src/Protocol.cpp` |
| `Service` | authentication boundary, dispatch, replay and transactions | `src/Service.cpp` |
| `Store` | SQLite ownership, migrations, prepared statements, crypto helpers | `src/Store.cpp` |
| Domain files | directory, invitations, parties, privacy, avatars, boards | matching `src/*.cpp` |
| Event listener/hub | authenticated hint subscriptions and fan-out | `src/EventListener.cpp` |
| Relay listener/hub | ticket redemption, grants, frames and routing | `src/RelayListener.cpp` |
| Admin | trusted provisioning through shared Store validation | `src/Admin.cpp` |

The protocol implementation map in the repository associates every operation family with tables
and tests. Use symbols rather than memorized line numbers: line numbers drift, ownership does not.

## Request path and lock ownership

```diagram
Network thread -> Beast parser: bounded HTTP body and headers
Beast parser -> Request pool: ordinary operation
Beast parser -> Sign-in pool: password authentication
Request pool -> Service mutex: serialize Store access
Service mutex -> SQLite: transaction and fixed statements
SQLite -> Service mutex: commit or stable error
Service mutex -> Event hub: hints after unlock
Request pool -> Network thread: response bytes
```

Two Asio threads own sockets and coroutine lifetimes. Four request workers run blocking service
work. Two separate sign-in workers contain scrypt CPU and memory cost. `Service::mutex_` serializes
the single Store connection and application caches. Hints are accumulated under the service lock
but delivered after release, preventing event callbacks from extending transaction ownership.

Relay and event hubs have their own synchronization. A redeemed relay releases its pre-auth
admission lease and moves under the account-backed relay cap. Captured references in listener
coroutines remain valid because `listen` owns Service, hubs, pools and acceptors until both network
threads stop and worker pools join.

## Shutdown and crash behavior

SIGINT/SIGTERM stops the I/O context, which stops acceptance and active network work, joins the I/O
thread, then joins service/sign-in pools before local owners destruct. It is bounded graceful
shutdown, not a promise to drain arbitrary slow clients. Transactions use RAII rollback unless
committed, so correctness also holds for SIGKILL and disconnects.

Process-local event/relay channels disappear on any restart. Credentials, directory leases and
memberships persist sufficiently for clients to authenticate new channels. See
[host migration and reconnect](13-host-migration.html) for the behavioral sequence.

## Source reading order

1. Read `protocol/v1.md` and `protocol/relay-v1.md` without code.
2. Read `Protocol.cpp`, then the top of `Service.cpp` through `dispatch`.
3. Read `Store.cpp` construction/migrations and transaction RAII.
4. Follow one small read (`profile.get`) and one durable mutation (`friends.add`).
5. Read directory plus invitations before relay authorization.
6. Read listener coroutines last; their job is transport and lifetime, not domain meaning.
7. Confirm the model in tests, especially atomicity, concurrency lifetime and black-box conformance.

## Architectural invariants

Do not introduce a second SQLite writer process for player traffic, call Service while holding a hub
lock, hold the Service mutex across event delivery, accept a client-supplied relay source, or treat
an event hint as durable truth. Those changes would violate assumptions spread across otherwise
simple components.
