---
title: Introduction and five-minute mental model
summary: What the service is, why it exists, and the smallest accurate model of its control, event, relay and storage planes.
order: 1
---

## What this server is

CNA Gamer Services Server is an independent C++23 implementation of the online service boundary
used by CNA's XNA-compatible API. A game sees familiar concepts—signed-in gamers, friends,
achievements, leaderboards, parties and `NetworkSession`—while this process supplies the account,
directory and relay authority. It is not Xbox LIVE compatible and does not use Xbox LIVE accounts,
wire formats, binaries or assets.

The server deliberately stays one process and one SQLite database. That is enough for one operator
to reason about ownership, transactions, backups and incidents. It is not a horizontally scalable
service mesh, matchmaking oracle, store, TrueSkill service or peer-to-peer NAT traversal system.

## Five-minute mental model

```diagram
CNA game -> TLS listener: HTTPS control request
TLS listener -> Service: validated operation
Service -> SQLite: serialized transaction
SQLite -> Service: durable result
Service -> CNA game: stable response/error
CNA game -> Event WSS: authenticate access token
Service -> Event WSS: coalesced change hint
CNA game -> Relay WSS: redeem one-use ticket
Relay WSS -> CNA game: opaque authorized datagram
```

There are three network uses of one public TLS port. `POST /cna/v1` performs stateful control
operations. `/cna/v1/events` is a WebSocket carrying hints such as “messages changed”; the client
still reads truth with control calls. `/cna/relay/v1` is a WebSocket carrying bounded opaque game
datagrams after a ticket proves current session membership.

SQLite is the persistent source of truth for credentials, social state, progress, definitions and
directory leases. Event subscriptions and live relay routes are memory-only. After restart, clients
authenticate new channels against persistent authority; they do not expect socket identity to
survive.

## Relationship to CNA and XNA concepts

CNA is the client-side runtime and remains an independent read-only compatibility peer for this
project. Games configure an endpoint and title ID; no server change may demand a coordinated CNA
change. The control and relay v1 specifications plus golden vectors are the contract.

XNA names explain the user model, not the network implementation. `SignedInGamer` maps to a
title-scoped access credential. `GamerServicesDispatcher.Update` drives heartbeat and event
processing. `NetworkSession` directory state is control-plane data; its game and voice datagrams
are opaque relay data.

## The invariants to remember first

- One server process owns runtime authority for one database.
- Every request and credential is title-scoped; cross-title access fails.
- Persistent mutations and replay outcomes commit together.
- Refresh credentials rotate; replay revokes the whole family.
- Directory membership authorizes relay routing; a claimed source never does.
- Events are hints, reads are truth.
- The administrator provisions identity; clients cannot self-register.
- Diagnostics and administration are separate management planes.

Continue with [architecture and source tour](02-architecture.html), then read
[protocol foundations](05-control-protocol.html) before changing behavior.
