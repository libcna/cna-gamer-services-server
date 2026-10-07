# Operations guide

This guide is the short operator reference. The player protocol remains on the configured TLS
listener; health and metrics are an independent, opt-in management surface.

## Startup

Production requires a numeric bind address, certificate chain, private key and writable database
directory. The key and database should be readable only by the dedicated service account.

```sh
build-agent/cna-gamer-services-server \
  --database /var/lib/cna-gamer-services/service.sqlite3 \
  --listen 0.0.0.0 --port 47831 \
  --cert /run/credentials/certificate.pem \
  --key /run/credentials/private-key.pem \
  --diagnostics-listen 127.0.0.1 --diagnostics-port 47832 \
  --log-format json
```

`--insecure-loopback` is only for local development and is refused on a non-loopback control
address. `--diagnostics-listen` is likewise refused unless the address is loopback. Run
`cna-gamer-services-server --help` for the complete stable command line and `--version` to identify
the binary.

The first stdout record confirms the actual control port (important when port `0` is used). A
second record confirms diagnostics when enabled. JSON logs contain `timestamp`, `level`, `event`
and event-specific bounded fields. Human logs retain the compact startup and minute-stat format.
No mode logs passwords, access/refresh credentials, relay tickets, request bodies or client IPs.

## Health and metrics

The diagnostics listener is deliberately plain HTTP on loopback. Reach it locally, through a
monitoring sidecar, or through an authenticated SSH tunnel; never publish it directly.

```sh
curl --fail http://127.0.0.1:47832/healthz
curl --fail http://127.0.0.1:47832/readyz
curl --fail http://127.0.0.1:47832/metrics
```

- `/healthz` proves the network process can answer without touching user state.
- `/readyz` queries SQLite and is ready only when foreign keys are enabled and the open database
  has exactly the schema this binary expects. It does not mutate state.
- `/metrics` is Prometheus text. It performs the same bounded database snapshot as readiness.

The metrics are cumulative until process restart except gauges and uptime. Labels are restricted to
fixed surfaces, server-generated outcomes and the known operation set; unknown client operation
names collapse to `unknown` and malformed messages to `invalid`.

| Metric | Type | Meaning |
|---|---|---|
| `cna_build_info{version}` | gauge | Running build version; value is always 1 |
| `cna_up` / `cna_ready` | gauge | Process liveness / database readiness |
| `cna_uptime_seconds` | gauge | Monotonic process uptime |
| `cna_schema_version` | gauge | Open SQLite `user_version` |
| `cna_database_bytes` | gauge | SQLite page count multiplied by page size; WAL bytes excluded |
| `cna_titles`, `cna_accounts` | gauge | Provisioned durable objects |
| `cna_access_sessions` | gauge | Unexpired access credentials |
| `cna_directory_sessions` | gauge | Unexpired multiplayer directory sessions |
| `cna_pending_invitations` | gauge | Unexpired invitations still pending |
| `cna_connections{surface}` | gauge | Control, relay, event and diagnostics connections |
| `cna_refused_connections_total{surface}` | counter | Admission refusals since startup |
| `cna_responses_total{outcome}` | counter | Service results, including file retrieval |
| `cna_operations_total{operation}` | counter | Known control operations plus `files.read`, `unknown`, `invalid` |
| `cna_request_duration_seconds` | histogram | End-to-end service/file work, fixed 1–250 ms buckets plus infinity |

Useful first alerts are readiness below 1, any increase in `INTERNAL_ERROR`, sustained admission
refusals, and latency moving materially relative to the same machine's own baseline. A database
size increase is a capacity clue, not intrinsically an error.

## Shutdown, backup and recovery

SIGINT or SIGTERM stops acceptance and the Asio loop, joins service workers and then closes the
database. Crash recovery is still a required safety property; do not depend on a graceful stop for
correctness. Use the SQLite online-backup tools and tested restore procedure in
[the deployment tools](../tools/cna-gamer-services-deploy/README.md). The stressed recovery
procedure is [disaster-recovery.md](disaster-recovery.md).

## Evidence boundary

Linux loopback is tested. The checked-in systemd and container files are examples that must be
adapted to host paths and certificate automation. The L4 proxy example is syntax-reviewed but was
not exercised against a remote public network in this campaign. No loopback measurement is a
public-Internet capacity or latency claim.
