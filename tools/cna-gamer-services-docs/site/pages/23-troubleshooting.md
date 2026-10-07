---
title: Symptom-oriented troubleshooting
summary: Fast evidence-first paths for authentication, TLS, sessions, realtime channels, storage, latency and administration failures.
order: 23
---

## First five checks

Record binary version, `/healthz`, `/readyz`, recent structured statistics and host free disk/fd/RSS.
Confirm the game endpoint/title ID and whether one or all accounts/titles are affected. Do not delete
WAL files, edit `user_version`, reset credentials or restart repeatedly before understanding scope.

## Client cannot sign in

Check TLS first, then title existence/minimum version, username spelling and
`AUTHENTICATION_FAILED` versus `RATE_LIMITED`. Many accounts failing slowly suggests scrypt worker
saturation; only one account failing suggests credentials. `UNAUTHENTICATED` is normally a token
operation, not password rejection.

## Certificate rejected

Verify endpoint hostname equals certificate SAN, chain includes intermediates, certificate time is
valid and CNA uses the intended CA bundle. Connecting by IP to a DNS-only certificate must fail.
Never “fix” production by enabling insecure loopback.

## Refresh suddenly revoked

A consumed refresh token replay revokes its family by design. Look for concurrent credential-store
copies or a lost response followed by old-token retry. Reauthenticate; do not resurrect rows.

## Session cannot be joined

Re-read the snapshot. Check title/kind, local participant count, public/private capacity,
join-in-progress, invitation state/expiry, blocks/avoid review, account already in a title session
and session/machine lease expiry. Search success never guarantees later join.

## Relay connection refused

Issue a fresh ticket. Check one-use replay/expiry, exact title, access family, current machine
membership and participant list. A malformed hello or binary/text mismatch closes deliberately.

## Relay connects but no traffic

Check destination machine is the 16-byte current identity in the same session, both grants remain
valid and client is reading binary frames. Unknown destinations drop silently. Payload/ENet bugs
are above the relay; source identity in received frames should be server-injected.

## Events reconnect repeatedly

Validate the exact four-field hello and live token. Inspect proxy idle timeouts and
`cna_connections{surface="events"}`. Reconnect and poll resources; there is no missed-event replay.

## Database busy or `INTERNAL_ERROR`

Check concurrent admin/import/backup activity, filesystem latency, free space, permissions and
SQLite/WAL errors. The server busy timeout is bounded and later requests can recover. Take an online
backup before invasive analysis. Never copy only the main file while WAL is active.

## Schema too new or database lock failure

Too-new means the binary is older than the database: run the matching build or restore a compatible
backup, never lower the pragma. `DATABASE_IN_USE` means another server owns runtime authority;
identify it. `DATABASE_LOCK_FAILED` means lock path/permissions/platform operation failed.

## Admin web unavailable

Check password file mode 0600, executable/database paths, bind policy and loopback port. Remote bind
needs both explicit opt-in and TLS. A 403 after login is usually CSRF/session expiry or read-only
mode. A 409 is a C++ admin validation refusal, shown without secret input.

## High authentication or request latency

Authentication-only latency points to scrypt CPU/memory or sign-in storms. All-operation latency
points to Service lock/SQLite/host contention. Compare the histogram, operation/outcome counters,
CPU, I/O and loadlab scenario. Benchmark only after correctness and host contention are known.

## Descriptor exhaustion

Inspect process/host limits and current connection surfaces. Admission backs off rather than ending
the server, but upstream distributed slow connections can fill 256 pre-auth slots. Raise limits to
the documented ceiling and add L4 flood control; do not simply remove admission.
