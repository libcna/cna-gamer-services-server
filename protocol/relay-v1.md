# CNA realtime relay protocol v1

This CNA-owned protocol/accounts/assets are not Xbox LIVE compatible. This document defines
bounded framing and relay ticket/grant authority through GS-008a2. The WSS endpoint and forwarding
are the next slices; no relay data capability or Internet connectivity is currently implemented.
Canonical codec: `protocol/include/CnaService/RelayProtocol.hpp`, `src/RelayProtocol.cpp`;
golden corpus `protocol/golden/relay-v1.json`. CNA copies these artifacts exactly and tests drift.

Control remains verified HTTPS POST `/cna/v1`: accounts, directory membership, host revisions,
invitations and upcoming tickets. Realtime relay uses a separate verified WSS upgrade endpoint
`/cna/relay/v1`. It forwards ENet UDP datagrams, never XNA objects or REST gameplay payloads.
Tickets/credentials must remain inside TLS messages, never URLs/logs. The client uses a private
loopback UDP bridge with a route for each service-authorized machine; relay is mandatory first,
and direct-peer connectivity is a later optimization. IP/port discovery is not NAT traversal.
WSS/TCP head-of-line latency is an explicit first transport tradeoff, not Xbox networking parity.

## Binary datagram message

Exactly one WebSocket binary message contains one envelope plus one ENet UDP datagram. TCP and
WebSocket fragmentation may split that message; implementations must bound accumulated message
length before buffer growth. No compression/extensions or text datagrams are negotiated.

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 3 | ASCII `CNR` magic |
| 3 | 1 | unsigned version 1 |
| 4 | 1 | message type 1 (datagram) |
| 5 | 3 | reserved; all zero |
| 8 | 16 | nonzero opaque service machine ID |
| 24 | 1..4096 | complete encapsulated ENet UDP datagram |

Maximum message 4,120 bytes; queue ceiling 64 messages/263,680 bytes per connection. The
4,096-byte payload ceiling covers ENet's maximum MTU; ENet fragments larger game packets before
this layer. No untrusted datagram length field, endpoint or filesystem path is used. Machine ID
strings in control messages are precisely 32 lowercase hex digits; all-zero/broadcast IDs are
forbidden. Client-sent ID is the **destination**. Server-sent ID is the **authenticated source**,
injected by the server from the connection grant. The sender cannot supply a source identity.
Routing may only select an active authorized machine in the sender's session/title, never an
arbitrary IP/port. A dropped or unauthorized destination must never be reinterpreted as broadcast.

Pure parsing performs checks before payload allocation and returns a borrowed view. Rejections,
in precedence order: `RELAY_TRUNCATED` (<24), `RELAY_TOO_LARGE` (>4120), `RELAY_MAGIC`,
`RELAY_VERSION`, `RELAY_MESSAGE_TYPE`, `RELAY_RESERVED`, `RELAY_EMPTY_PAYLOAD`, `RELAY_MACHINE_ID`.
Transport will close/refuse malformed messages without echoing bytes or credentials. Golden vectors
exercise each code; boundary tests cover empty/maximum/oversize and deterministic 10,000 mutations.

## Pending authenticated transport requirements

One-use hashed 60s tickets must bind title/session/machine and all local users' revocable authority.
Normal access refresh must preserve an established machine grant; logout/revocation/leave/host
expiry must invalidate it. Maintain one connection per machine, handshake deadlines, periodic grant
checks, per-connection read/write serialization, bounded queues/backpressure and rate limits.
Reconnect needs a fresh ticket. Advertise relay only after the endpoint actually forwards data.
Test genuine multi-process ENet under relay-only isolation/firewall restrictions before any Internet
multiplayer claim; localhost membership tests alone do not meet that criterion.

## Ticket and connection authority (implemented GS-008a2)

Capability `relay-tickets` covers HTTPS `sessions.relayTicket {session,participants}`. It is
separate from a working `relay` capability (not yet advertised). `participants` contains 1..4
current authenticated title-bound access credentials matching the machine's complete directory
group; actor must own that machine. Secondary/nonmembers/partial/foreign/duplicate/revoked
groups fail before issuance. Result exactly `{ticket,session,machine,expires,serverTime,relayVersion,
maxDatagramBytes}`: random 256-bit bearer ticket, <=60s lifetime, protocol 1/4,096-byte ceiling.
Never log/persist plaintext tickets; server SQLite schema 8 stores only SHA-256 ticket hashes.

Encrypted connection redemption requires title plus ticket, checks all group members and consumes
it exactly once transactionally. Wrong title, unknown/expired/used/malformed credentials fail
UNAUTHENTICATED. The resulting server-owned grant is not accepted as a network credential.
Grant authority binds all local accounts' revocable refresh families, exact session/machine/owner
and directory membership. Ordinary access rotation preserves it; family revocation/expiry, online
privilege loss, group departure or host lease expiration invalidates it. One-hour grant ceiling
requires reconnect with fresh ticket; grant release deletes only the exact disconnected grant.

Unconsumed tickets may survive server restart until their short expiry; used tickets cannot be
replayed. Open data connections do not survive a server process restart and need fresh authority.
Limits: 8 outstanding ticket/grant records per machine, 8,192 per title; expired unused/grants are
pruned before issuance/redemption/validation. Disconnect release prevents repeated normal reconnect
from accumulating grants; abandoned used records expire within an hour. Existing host/machine
90s directory leases remain authoritative and a ticket does not extend them. Migrations are
transactional; schema 7 and earlier preserve accounts/catalog/data, newer schema is refused.

Transport still must implement TLS-only ticket messages, bounded handshake, one channel per
machine, periodic revocation checks, queue/rate limits and source-authorized forwarding. Issuing
a ticket alone is not a realtime connection or proof of Internet multiplayer.
