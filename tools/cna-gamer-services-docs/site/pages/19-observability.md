---
title: Observability, metrics, logging and incident workflow
summary: Liveness/readiness meaning, every exported metric, bounded structured logs and a repeatable diagnosis sequence.
order: 19
---

## Separate diagnostics plane

Enable `--diagnostics-port 47832`. The server refuses a non-loopback diagnostics bind. It is plain
HTTP because local-only reachability is the security boundary; use a local collector or
authenticated tunnel, never a public firewall opening.

`/healthz` proves the process can accept and answer without database work. `/readyz` reads SQLite
and is ready only when the open schema equals this binary's `SchemaVersion` and foreign keys are
enabled. Neither mutates user state. `/metrics` returns Prometheus text and performs the same
bounded storage snapshot.

## Metric reference

| Metric | Type and labels | Interpretation |
|---|---|---|
| `cna_build_info` | gauge `{version}` | Identity of running binary |
| `cna_up`, `cna_ready` | gauges | Process answer / usable expected-schema database |
| `cna_uptime_seconds` | gauge | Monotonic process age |
| `cna_schema_version` | gauge | SQLite `user_version` |
| `cna_database_bytes` | gauge | Allocated DB pages; WAL excluded |
| `cna_titles`, `cna_accounts` | gauges | Provisioned persistent objects |
| `cna_access_sessions` | gauge | Unexpired access credentials |
| `cna_directory_sessions` | gauge | Unexpired multiplayer sessions |
| `cna_pending_invitations` | gauge | Pending and unexpired |
| `cna_connections` | gauge `{surface}` | control, relay, events, diagnostics |
| `cna_refused_connections_total` | counter `{surface}` | Admission refusals since start |
| `cna_responses_total` | counter `{outcome}` | Stable protocol/file outcomes |
| `cna_operations_total` | counter `{operation}` | Known operations plus `files.read`, `invalid`, `unknown` |
| `cna_request_duration_seconds` | histogram | Service/file latency at 1, 5, 10, 25, 50, 100, 250 ms and infinity |

Operation labels never accept an arbitrary client string: an unsupported name collapses to
`unknown`; a request that cannot establish an operation is `invalid`. There are no player/title/IP
labels. Counters reset on restart; gauges reflect the scrape.

## Logging reference

Default human startup retains `CNA service listening on ADDRESS:PORT`, followed by a diagnostics
line when enabled. Once a minute `stats` gives interval outcomes, refusals and current control,
relay and event connections.

`--log-format json` emits one JSON object per line. Startup includes `timestamp`, `level`, `event`,
`address`, `port`, TLS state and version. Statistics includes timestamp/level/event, an `outcomes`
object, refused/open control, relay and event counts. Startup failures include a stable code. No
mode logs request body, address, token, password or ticket.

## Alerting starters

- `cna_ready != 1` for more than the startup window.
- Any sustained increase in `INTERNAL_ERROR`.
- Refusals rising outside a known load test.
- p95/p99 latency deviating from the same host's baseline.
- Database bytes or durable table counts growing contrary to workload.
- Event/relay connections increasing without falling after clients depart.

## Incident workflow

1. Record wall time, version/build info, uptime and readiness; avoid restarting before evidence unless required for safety.
2. Identify affected surface: sign-in, ordinary control, event, relay, storage or all.
3. Compare outcomes and latency with connection/refusal counters and host CPU/RSS/fd/disk state.
4. Reproduce one safe request with a test account; never request a player's credential or dump payloads.
5. Check database free space, lock/busy symptoms and schema. Take an online backup before repair.
6. Use protocol IDs from client-side debug context only where authorized; server logs intentionally omit them.
7. Apply the narrowest recovery, then run readiness, conformance smoke and the affected focused test.
8. Preserve a concise timeline, cause, invariant/test added and evidence boundary.
