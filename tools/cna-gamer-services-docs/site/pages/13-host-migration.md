---
title: Host migration, restart and reconnect
summary: Graceful departure, lease expiry, replacement selection and what persistent authority lets clients rebuild after failure.
order: 13
---

## Graceful host departure

```diagram
Host -> Server: sessions.leave(session)
Server -> SQLite: remove host machine
Server -> SQLite: choose eligible remaining machine
Server -> SQLite: update host account/machine, revision and lease
Server -> Joiner: leave result says session continues
Joiner -> Server: sessions.get
Server -> Joiner: snapshot names replacement host
```

Migration must be enabled, at least one eligible machine must remain and session revision must still
be valid. Selection is deterministic from durable membership order; clients do not elect a host by
race. If migration is disabled or no machine remains, the session ends and cascading membership,
invitation and relay-ticket rows disappear.

## Host crash

A crashed game cannot call leave. Its machine lease expires. Pruning observes the expired host,
updates visible revision, chooses a replacement under the same policy, or closes the session. Until
that transition, peers may retain an old snapshot but relay grant revalidation prevents authority
from being invented.

## Service restart

```diagram
Running server -> Operating system: process exits/SIGKILL
Operating system -> Clients: TLS/WSS connections close
Operator -> New server: open same SQLite database and lock
New server -> SQLite: WAL recovery, schema validation
Clients -> New server: auth.ping or refresh
Clients -> New server: sessions.get/touch
Clients -> New server: obtain fresh relay tickets
Clients -> New server: authenticate event and relay WSS
```

Access/refresh authority, directory rows and leases are persistent. Live event subscriptions and
relay socket routes are not. Tickets are short-lived and one-use, so reconnect obtains new tickets
rather than replaying secrets captured before failure.

The chaos suite kills the process with an active two-player session, restarts it, verifies SQLite
integrity and existing access credentials, reads the membership, authenticates a new event channel,
issues new relay tickets and routes a datagram. That proves process-crash recovery on Linux
loopback—not power-loss behavior or public-network timing.

## Debugging migration

Read current session snapshot and revision first. Confirm `allowHostMigration`, host machine expiry,
remaining machine leases and the replacement's members. A relay that reconnects but cannot route
usually indicates stale membership/ticket authority, not an ENet payload problem.
