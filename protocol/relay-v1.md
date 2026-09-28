# CNA realtime relay protocol v1

This CNA-owned protocol/accounts/assets are not Xbox LIVE compatible. This document defines
bounded relay framing only at GS-008a1. The WSS endpoint, authentication tickets and forwarding
are the next slices; no relay capability or Internet connectivity is currently implemented.
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
