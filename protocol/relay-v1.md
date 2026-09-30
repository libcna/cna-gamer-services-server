# CNA realtime relay protocol v1

This CNA-owned protocol/accounts/assets are not Xbox LIVE compatible. This document defines
bounded framing, ticket/grant authority and the authenticated WSS endpoint that carries every CNA
online NetworkSession's ENet datagrams. It is tested with real CNA processes for both directory
categories, isolated titles/sessions, separate outbound-NAT namespaces, server restart, host
migration and host crash on one Linux host; public Internet latency and failover are not measured.
Canonical codec: `protocol/include/CnaService/RelayProtocol.hpp`, `src/RelayProtocol.cpp`;
golden corpus `protocol/golden/relay-v1.json`. CNA copies these artifacts exactly and tests drift.

Control remains verified HTTPS POST `/cna/v1`: accounts, directory membership, host revisions,
invitations and ticket issuance. Realtime relay uses a separate verified WSS upgrade endpoint
`/cna/relay/v1`. It forwards ENet UDP datagrams, never XNA objects or REST gameplay payloads.
Tickets/credentials must remain inside TLS messages, never URLs/logs. The client uses a private
loopback UDP bridge with a route for each service-authorized machine. Every datagram goes through
the relay: there is no direct peer path and no NAT traversal (IP/port discovery is not NAT
traversal). WSS/TCP head-of-line latency is the accepted tradeoff, not Xbox networking parity. The
datagrams are opaque to the server: CNA's game data and its NetworkSession voice frames alike.

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

## Authenticated WebSocket transport

Upgrade exactly `/cna/relay/v1`, without query parameters. Production uses verified WSS with TLS
>=1.2. The existing explicit insecure development switch permits unencrypted WS only on a numeric
loopback bind; it must never authenticate Internet clients. No compression is negotiated. Client
certificate/hostname/CA verification is mandatory; credentials are never URL parameters or headers.
First message is text UTF-8 JSON, <=1,024 bytes, within 5s of upgrade, exactly four fields:

```json
{"v":1,"id":"relay-request","game":"your-title","ticket":"<64 lowercase hex digits>"}
```

`id` and `game` obey control v1 identifiers (1..64 ASCII letters/digits/dash/underscore/dot).
Duplicate/unknown fields, noninteger/unknown versions, invalid IDs/UTF-8 and malformed tickets are
refused deterministically. Redeem the one-use ticket and register at most one connection for the
exact authorized title/session/machine. Success is one text response, then only binary datagrams:

```json
{"v":1,"id":"relay-request","error":"OK","result":{"session":"<32 hex>","machine":"<32 hex>","capabilities":["enet-datagrams"],"maxDatagramBytes":4096,"maxQueuedFrames":64}}
```

The server substitutes its own authenticated source machine into each outgoing binary frame.
Unknown, disconnected, same-machine and foreign title/session destinations drop without response;
they never broadcast or reveal another session's membership. Reconnect requires a fresh HTTPS
ticket. Closing releases only that connection's grant, not account or directory membership. A
new ticket cannot replace an existing active channel; its consumed grant is released on refusal.

Policy closes use WebSocket 1008 (bad credentials, duplicate/full registration, queue/rate/authority
failure); malformed envelope/version/type/identifier uses 1002, invalid text UTF-8 uses 1007,
oversize accumulated messages use 1009. Safe empty close reasons contain no input or credentials.
Transport failure/authentication timeout may abort TCP without a close frame. Close handshakes have
a 5s ceiling. Idle timeout is 30s with keepalive ping. Client ping/pong/close frames count toward
ingress limits even before authentication. Grant checks run every 5s; family revocation, title/
membership/host lease loss or grant expiry closes the channel within that interval. Relay does not
renew the separate 90s directory lease. A continuously revoked grant may therefore remain routed
until its next check, at most 5s; no claim of immediate per-datagram account revocation.

Resource policy: 1024 live authenticated relay channels, counted apart from the listener's 256
control connections (at most 32 per source address, which also covers upgrades that have not yet
redeemed a ticket), 512 incoming
datagram/control messages per fixed monotonic one-second window, 1 MiB binary bytes/window.
Application write queues hold <=64 complete frames/263,680 bytes, including the active write.
Capacity is reserved before posting across strands; at most one wake/overflow notification is
pending. Overflow closes the slow recipient. Each accepted socket has its own Asio strand, one
reader and one serialized writer. Pending writes own their buffers until completion/cancellation.
These are fixed per-process caps; the relay hub lives in one server process and does not scale
across machines (README, "One process").

Canonical handshake examples accompany binary golden vectors. Queue/rate/session/title routing
unit tests cover resource boundaries and concurrent producers. Verified-WSS independent-client tests
cover secure forwarding and failure semantics. Multi-process CNA ENet under relay-only NAT
isolation is tested (below); public Internet deployment, latency and failover are not qualified.

## Ticket and connection authority (implemented GS-008a2)

Capability `relay-tickets` covers HTTPS `sessions.relayTicket {session,participants}`. It is
separate from the `relay` capability for this WSS endpoint. `participants` contains 1..4
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

Transport implements TLS-only ticket messages, bounded handshake, one channel per machine,
periodic revocation checks, queue/rate limits and source-authorized forwarding as specified above.
When a machine's relay connection closes, its directory lease is cut to 20 seconds; reconnecting
with a fresh ticket restores it, so a crashed machine leaves its session in about 20 seconds.

## Measured connectivity boundary (GS-008d1)

The first implemented client data path is relay-only: ENet listens on private loopback and each
service-authorized remote machine maps to a stable local UDP bridge port. No peer IP is treated
as proof of Internet reachability, and no direct-connect optimization is claimed. Verified WSS
carries unchanged datagrams to the central relay; that relay injects authenticated source IDs.
The native corpus exercises separate rootless Linux network namespaces with independent outbound
NAT helpers, identical private addresses and no inbound mappings, for both categories/titles and
multi-local identities. Reliable 32KiB application fragmentation and an unreliable channel pass.
This is shared-host NAT-isolation evidence, not public Internet deployment/latency/failover proof.
Every datagram goes through the relay: there is no direct/P2P path. Test prerequisite absence is an
explicit skip, not a passing NAT result. See README for reproduction and test-helper licensing.

## Client game admission boundary (GS-008c3c1)

CNR authorization identifies the sending machine and its title/session membership. The relay
continues forwarding opaque ENet bytes. CNA's game receiver separately verifies exact gamer
claims against authenticated directory membership and completed realtime machine groups. Host
receivers require a game sender from the authenticated source group; clients receive only
host-relayed messages for known admitted remote senders and their own local targets. Validate
logical lengths/options/channels and membership before allocating a game payload copy.
Directory reservation alone is not a completed realtime handshake. State/properties must match
service authority. Host leave broadcasts may name only complete admitted remote groups; arbitrary
unknown/partial leave or end claims cannot replace authority reconciliation. These client rules do not change CNR v1/golden envelope bytes, impose XNA objects
on the server or provide malicious-host anti-cheat. Native tests now refuse a forged existing
sender ID and unknown target while preserving actual fragmented/unreliable ENet exchange in
both localhost and separate outbound-NAT namespaces.

## Owned native session engine (GS-008c3c2)

CNA consumes directory membership and secure relay resources into one owner-thread session.
Host readiness means prepared transport; client readiness additionally requires the exact
service-bound ENet welcome. Directory membership is reconciled before packet admission, while
connected complete groups remain a separate condition. Revisions trigger bounded welcome
recovery; removed authority, transport closure or a ten-second handshake timeout is observed
as one safe failure. Game data retains the existing ENet packet format and the server remains
an opaque datagram forwarder. CNA bounds pending observations and unacknowledged outgoing data
by 128 messages/4MiB, with a separate 64-message/256KiB control budget and a 64-ENet-event pump
budget. These local implementation limits do not change CNR v1 envelope/golden bytes. New native
and NAT acceptance cases exercise the owned engine, separately from hostile raw packet probes.
The standard XNA NetworkSession sits on this engine: create/find/join, invitations, host migration,
AddLocalGamer and reconnection across a service restart are covered by the `service_cna_session*`
tests.
