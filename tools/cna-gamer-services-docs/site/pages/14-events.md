---
title: Event channel and polling fallback
summary: Authentication, coalesced topics, reconnect rules, why hints are not truth and which state is intentionally not pushed.
order: 14
---

## Handshake

The client upgrades `/cna/v1/events` over WSS, then sends exactly `v`, `id`, `game` and `token`.
Unknown fields, wrong types, invalid identifiers or a non-live title-scoped access token close the
channel. The welcome returns supported topics.

```json
{"v":1,"id":"events-1","game":"my-game","token":"<64 hex characters>"}
```

The token is shown symbolically because documentation must never normalize copying real
credentials into logs or examples.

## Hint semantics

Topics are `invitations`, `messages`, `friends` and `party`. A successful changing request queues
hints for other affected accounts after its transaction and after the Service lock is released.
The hub coalesces pending topics, so ten messages may produce one “messages changed” notification.

```diagram
Bob -> Server: messages.send(to Alice)
Server -> SQLite: commit durable message
Server -> Alice events: topics=[messages]
Alice -> Server: messages.list
Server -> Alice: authoritative inbox
```

No correctness depends on receiving the hint. Disconnect, process restart, coalescing and network
loss are all expected. CNA continues bounded polling and re-reads the named resource after a hint.

Presence, session directory listings, achievements and leaderboards are intentionally not pushed.
Their normal polling/read boundaries already provide truth, and pushing every mutation would add
fan-out complexity and false ordering guarantees.

## Reconnection

After close, reconnect with a current access token and perform ordinary reads to reconcile. If
authentication fails, refresh/sign in first. There is no durable event cursor and no replay log;
adding one would change hints into a second source of truth.

Monitor `cna_connections{surface="events"}` for leakage and compare with expected online clients.
High reconnect churn with healthy control traffic suggests TLS/proxy idle policy or client update
cadence. The raw TCP proxy timeout must accommodate long-lived WebSockets.
