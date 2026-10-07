---
title: Limits and retention reference
summary: One operator-oriented map of transport, account, content, session, realtime and administrative bounds.
order: 28
---

Limits are part of the security and compatibility contract. Values here summarize the normative
`protocol/v1.md`, `protocol/relay-v1.md` and source constants; the
[control contract chapter](05-control-protocol.html) explains how they layer. When changing a bound,
update specification, implementation, boundary tests, conformance where observable, and this page
in one change.

## Transport and syntax

| Surface | Bound | Failure behavior |
|---|---:|---|
| Control request/response body | 65,536 bytes | Oversized/truncated input ends before mutation. |
| Control HTTP headers | 8,192 bytes | Beast rejects the request. |
| File-route headers | 4,096 bytes | Request is rejected before file lookup. |
| JSON nesting | 16 levels | `LIMIT_EXCEEDED`/malformed envelope. |
| JSON array items or object fields | 256 | Rejected before unbounded DOM growth. |
| Identifier (`id`, `game`, `op`) | 1–64 ASCII identifier characters | `INVALID_ARGUMENT`. |
| Gamertag | 1–32 UTF-8 bytes | `INVALID_ARGUMENT`. |
| Password | 8–256 UTF-8 bytes | Login/provisioning refusal. |
| Control connections | 256 total, 32/source | Admission refusal. |
| New connections | 600/source/minute | Admission refusal. |
| Requests on one keep-alive connection | 1,000 | Connection closes. |
| First request / next request / request work | 10 s / 5 s / 10 s | Connection closes on deadline. |

## Credentials, replay and downloads

| Resource | Bound or lifetime | Why it exists |
|---|---:|---|
| Access credential | 1 hour; at most 32 live/account | Bounds stolen-token and table exposure. |
| Refresh family | 30 days, at most 1,024 rotations | Rotation cannot create indefinite authority. |
| Login plus refresh attempts | 10/source/minute | Bounds expensive scrypt and credential probing. |
| Recorded request outcome | 24 hours; stored result at most 16 KiB | Exactly-once retry without permanent growth. |
| Request IDs | 20,000/account/title/day; 1,000,000/title/day | Bounds replay bookkeeping. |
| File download | 1 GiB/title/hour | Bounds bulk egress through authenticated file route. |

Secret-bearing results (`auth.login`, `auth.refresh`, `sessions.relayTicket`) are never persisted for
replay. Repeating their request ID produces `DUPLICATE_REQUEST`, not the original secret.

## Catalogs, progress and social data

| Area | Important bounds |
|---|---|
| Achievements | At most 128/title; score 0–1,000; name 128 bytes; description/how-to 1,024 bytes. |
| PNG assets | At most 512×512 and 512 KiB for ordinary imported images. |
| GLB assets | At most 16 MiB; avatar catalog files have stricter per-kind structural checks. |
| Asset chunk read | 1–12,288 bytes; hash is exactly 64 lowercase hex characters. |
| Leaderboard definitions | At most 32 typed columns; list at most 256 boards. |
| Leaderboard page | `size` 1–100; scalar column payload at most 2 KiB; stream at most 256 bytes. |
| Gameplay epoch | 1–4 participants, 16 open/owner, 24-hour lifetime, at most 128 unique rows/commit. |
| Messages | Text 256 bytes; 1–100 recipients; 200 sends/hour; 100 stored/recipient. |
| Message page | `start` 0–100, `limit` 1–32. |
| Reviews / blocks | 1,024 per reviewer / blocker. |
| Avatar description | Exactly 1,021 bytes (2,042 lowercase hex); one update per two seconds. |
| Avatar batch/catalog list | 1–16 user IDs; 1–64 installed catalog versions. |

The 64 KiB outer response still applies. A valid large leaderboard page can therefore return
`LIMIT_EXCEEDED`; clients should request a smaller page rather than expect truncation.

## Directory, invitations and parties

| Resource | Bound or lifetime |
|---|---:|
| Session capacity | 2–31 gamers; private slots 0..capacity. |
| Local machine group | 1–4 accounts; AddLocalGamer adds 1–3 to an existing owner. |
| Search | `localCount` 1–4, `start` 0–1,024, `limit` 1–32, exactly eight nullable int32 properties. |
| Live sessions | 1,024/title and 16/host/title. |
| Normal directory/machine lease | Clients touch every 30 s; expiry bounded near 90 s. |
| Relay-loss machine grace | 20 s unless a fresh relay reconnect restores the lease. |
| Invitation | 900 s; inbox start 0–64 and limit 1–32. |
| Invite sends | 32/sender/title/hour; 64 live incoming/account; 16,384 rows/title. |
| Party | At most eight members plus outstanding invitations; invitation lasts one hour. |

Capacity checks are atomic for the whole local group. A four-gamer join never partially inserts
two gamers because only two slots remained.

## Event and relay

| Resource | Bound or lifetime |
|---|---:|
| Event hello | At most 1,024 bytes within 10 s. |
| Event connections | Eight/account and 4,096 total; credential revalidated every 60 s. |
| Relay hello/authentication | At most 1,024 bytes within 5 s. |
| Relay datagram | 1–4,096 bytes plus fixed 24-byte header. |
| Relay traffic | 512 frames and 1 MiB per monotonic second. |
| Relay output queue | 64 frames / 263,680 bytes including active write. |
| Relay connections | 1,024 authenticated machines. |
| Relay ticket | 256-bit, one use, at most 60 s; eight outstanding/machine and 8,192/title. |
| Redeemed grant | At most one hour; durable authority rechecked at most every 5 s. |

## Administrative planes

The admin web accepts at most a 17 MiB form so one validated 16 MiB asset plus multipart overhead
fits. It keeps at most 64 one-hour idle sessions, tracks at most 256 login sources, permits ten
failed logins/source/minute, and runs at most 16 handler threads. The diagnostics listener remains
loopback-only and exports bounded labels; it has no player/session identifiers in metric labels.

If a deployment needs a lower Internet-facing ceiling, enforce it at the L4/firewall boundary too.
Do not raise server bounds to solve a client bug: first identify the exact legitimate workload and
add a boundary regression test.
