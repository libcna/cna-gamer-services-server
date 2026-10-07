---
title: Relay tickets, binary framing and authorization
summary: One-use capability lifecycle, authenticated machine identity, opaque NetworkSession traffic and routing isolation.
order: 15
---

## Why a relay exists

CNA NetworkSession game and voice traffic uses ENet datagrams. The server does not interpret their
game payloads; it supplies a reachable authenticated route when direct peer-to-peer connectivity is
not assumed. Every online datagram goes through this relay in the current architecture.

## Ticket lifecycle

```diagram
Game -> Control: sessions.relayTicket(session, participants)
Control -> SQLite: verify credentials, membership and machine
Control -> Game: random short-lived one-use ticket
Game -> Relay WSS: upgrade /cna/relay/v1
Game -> Relay WSS: v, id, game, ticket
Relay WSS -> SQLite: atomically redeem ticket
Relay WSS -> Game: authenticated machine welcome
Relay WSS -> Relay hub: attach route under grant
```

The ticket binds title, session, machine owner, local participants and their refresh families. Its
database form is hashed. Redemption is atomic and only once; replay closes with policy violation.
A ticket expiring or being used does not revoke the underlying directory membership—obtain a new
one after reconnect.

## Binary frame

Relay v1 frames begin with the fixed `CNR` magic, version/type/reserved bytes, a 16-byte destination
machine ID, then bounded opaque payload. On delivery the relay replaces that destination field with
the authenticated **source** machine. A client cannot forge another source by choosing bytes.

```text
43 4e 52 01 | 01 | 00 00 00 | <16-byte machine> | <opaque ENet datagram>
 C  N  R v1   data  reserved       route identity        payload
```

Unknown destinations are dropped rather than becoming an information oracle. Malformed/truncated
binary frames close with protocol error. Text after authentication and unsupported frame types are
invalid. Session/title boundaries are checked both at ticket issuance and grant revalidation.

## Authority and privacy

The relay hub caps authenticated machines separately from pre-auth control admission. It releases a
grant on disconnect and removes duplicate/old ownership according to hub rules. A compromised
member can send game payload to current peers; it cannot route to another title/session or claim a
different source machine.

Voice-like packets are opaque. Cooperative CNA clients apply privilege/block policy around capture
and playback. The relay cannot inspect semantics or provide content moderation.

## Load and diagnosis

Loadlab supports small, large, voice-cadence, burst, idle and asymmetric shapes, recording
connections, datagrams and bytes without packet logs. If connection fails, check ticket expiry,
one-use replay, live access family and current machine membership. If connection succeeds but data
does not arrive, check destination machine identity/session membership before debugging payload.
