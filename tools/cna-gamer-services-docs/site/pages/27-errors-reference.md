---
title: Complete error reference
summary: Protocol, relay, transport, startup and administration failures with client meaning and operator response.
order: 27
---

The wire error is a compatibility contract, not an exception message. Control responses use a
stable `error` value and never expose SQL, paths, cryptographic material or parser internals. A
client should branch on the code, not English text. Operator-facing startup codes are separate and
appear on stderr before the listener starts.

## Control protocol outcomes

| Code | What it means | Client behavior | Operator investigation |
|---|---|---|---|
| `OK` | The operation completed, or an idempotent repeat reached the same result. | Consume `result`; do not infer that a mutation was new unless its result says so. | None. |
| `MALFORMED_MESSAGE` | JSON, UTF-8, duplicate keys or the outer envelope could not be trusted. | Fix the serializer; never retry identical bytes. | A sustained rate suggests a broken or hostile client. |
| `LIMIT_EXCEEDED` | A hard size/count/resource ceiling was crossed. | Reduce the request or stop creating the bounded resource. | Use the [limits reference](28-limits-reference.html); do not raise limits blindly. |
| `UNSUPPORTED_VERSION` | `v` is not control protocol version 1. | Use a compatible protocol implementation. | Confirm endpoint and client release. |
| `INVALID_ARGUMENT` | A known operation had missing, extra, wrongly typed, malformed or out-of-range data. | Correct the request; an unchanged retry cannot help. | Compare the exact operation schema with the normative specification. |
| `INVALID_STATE` | The input is well formed but the requested transition is no longer legal. | Re-read current state and reconcile. | Common for used invitations, closed gameplay epochs and wrong session phase. |
| `NOT_AUTHORIZED` | The identity is known but privilege, relationship, title or role forbids the action. | Do not sign in repeatedly; refresh policy/state. | Check blocks, privileges, friendship, machine ownership and session role. |
| `NOT_SUPPORTED` | The operation exists but this requested mode is intentionally unsupported. | Choose a supported mode; capability discovery alone does not make every value valid. | Treat as a product boundary, not an outage. |
| `SESSION_FULL` | No legal public/private allocation can fit the complete local group. | Search again or wait; never split an atomic local group silently. | Inspect capacity, private slots and invitation authority. |
| `UNKNOWN_OPERATION` | `op` is syntactically valid but not implemented. | Re-run `hello`; do not probe names. | Usually client/server contract drift. |
| `UNKNOWN_TITLE` | The requested `game` has not been provisioned. | Stop and report configuration. | Create the title or correct `CNA_GAME_ID`. |
| `AUTHENTICATION_FAILED` | Login username/password did not authenticate. | Let the user correct credentials; back off. | Watch rate-limit/refusal trends without logging the password or address. |
| `UNAUTHENTICATED` | Access/refresh/ticket authority is absent, invalid, expired or revoked. | Refresh once when applicable, otherwise sign in. | Refresh replay intentionally revokes descendants; cross-title tokens also fail. |
| `NOT_FOUND` | The scoped object does not exist or is deliberately invisible. | Refresh the containing list; do not guess IDs. | Check title scope, expiry and cascade deletion before assuming loss. |
| `CONFLICT` | A uniqueness/revision/immutability expectation conflicts with stored state. | Re-read, merge if the feature permits it, then create a new intent/request ID. | Typical for stale updates or an immutable catalog version changing. |
| `DUPLICATE_REQUEST` | A request ID was reused by another intent, or its secret/large result cannot be replayed. | Never reuse the ID; generate a new ID only for genuinely new intent. | Inspect client correlation-ID generation. |
| `RATE_LIMITED` | A time-window budget was exhausted. | Back off at least through the documented window. | Find the specific login, request-ID, message, invite, avatar or download budget. |
| `REMOVED_BY_HOST` | A former session member was explicitly removed by host authority. | Surface the removal and discard the old membership/relay state. | This is different from an unknown or naturally expired session. |
| `UPDATE_REQUIRED` | The title requires a newer declared client version. | Direct the player to update; do not omit `titleVersion`. | Inspect `title-minimum-version`. |
| `INTERNAL_ERROR` | The server contained an implementation/storage failure and withheld details. | A same-ID mutation retry is safe only under the documented replay rule. | Check readiness, disk/locks and server logs; take an online backup before repair. |

## Transport and WebSocket outcomes

The control endpoint normally returns HTTP 200 even for protocol errors. A wrong route or method,
an oversized/truncated HTTP request, a failed TLS handshake, or the raw file route may instead use
an HTTP status or close the connection before a JSON response exists. Treat that as transport
failure, not as an invented protocol error.

Event and relay handshakes close with WebSocket policy/protocol codes when the first message is
wrong, late or unauthorized. After relay authentication, invalid binary frames are closed as
protocol errors. The internal test labels below identify the violated invariant:

| Relay validation label | Meaning |
|---|---|
| `RELAY_TRUNCATED` | Fewer than the fixed 24-byte header plus payload. |
| `RELAY_MAGIC` / `RELAY_VERSION` | `CNR` marker or relay version is wrong. |
| `RELAY_MESSAGE_TYPE` | The only v1 datagram message type was not used. |
| `RELAY_RESERVED` | Reserved header bytes were nonzero. |
| `RELAY_MACHINE_ID` | Destination machine ID is not exactly the required binary identity. |
| `RELAY_EMPTY_PAYLOAD` / `RELAY_TOO_LARGE` | Payload is outside 1–4096 bytes. |

These labels are regression-test diagnostics; a public peer gets a bounded close, not a detailed
parser oracle. Unknown destinations are silently dropped so one member cannot enumerate routes.

## Startup and administration failures

`TLS_REQUIRED` and `INSECURE_BIND_REFUSED` prevent accidental plaintext public credentials.
`DIAGNOSTICS_BIND_REFUSED` prevents the unauthenticated diagnostics plane leaving loopback.
`DATABASE_IN_USE` means another server owns runtime authority; `DATABASE_LOCK_FAILED` means the OS
lock itself could not be established. `UNSUPPORTED_DATABASE_VERSION` fails closed when a newer
schema meets an older binary. Never delete a lock or lower `user_version` to silence these errors.

The administrator CLI prints only stable codes such as `INVALID_ARGUMENT`, `NOT_FOUND`,
`UNKNOWN_TITLE` and `LIMIT_EXCEEDED`. The web UI maps validation to 400, CSRF/read-only refusal to
403, failed login to 401, login throttling to 429, database read failure to 503, and a refused C++
admin mutation to 409. It never includes the submitted password in a response or audit record.

## Triage rule

Ask four questions in order: did TLS/HTTP complete, did the envelope validate, did authentication
succeed, and did domain authorization/state permit the operation? This order prevents a common
mistake: treating every `NOT_AUTHORIZED` or `INVALID_STATE` as a credential outage. Use the
[troubleshooting guide](23-troubleshooting.html) for symptom-first diagnosis and the
[operation index](30-operation-index.html) to find implementation/tests.
