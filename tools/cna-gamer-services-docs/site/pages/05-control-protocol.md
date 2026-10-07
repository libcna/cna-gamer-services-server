---
title: Control protocol, capabilities, errors and limits
summary: The v1 envelope, capability negotiation, compatibility rules, stable error meanings and bounded-input contract.
order: 5
---

## Request and response framing

Every control operation is one JSON object in an HTTP POST to `/cna/v1`. HTTPS provides transport
confidentiality and server identity. The envelope provides protocol version, client correlation,
title scope, operation name, optional access token and operation arguments.

```json
{"v":1,"id":"client-42","game":"my-game","op":"hello","args":{}}
```

```json
{"v":1,"id":"client-42","error":"OK","result":{"version":1}}
```

The server echoes `id` when parsing reaches it. HTTP 200 carries protocol outcomes; transport HTTP
codes are reserved for wrong routes and the raw file endpoint. Unknown envelope fields, duplicate
JSON keys, excessive nesting and wrong types fail before domain work.

## Hello and capability discovery

`hello` needs no title provisioning or credential. Its capability list is the only safe way for a
client to decide whether an optional feature exists. A new server may add a capability without
changing old semantics. Removing a capability or changing an advertised operation breaks the
contract.

Never turn optional response data into required input. Never rename an operation, error or field
to make implementation code prettier. Extend server-only management surfaces before considering a
player protocol extension.

## Error interpretation

| Error | Meaning | Typical client/operator action |
|---|---|---|
| `MALFORMED_MESSAGE` | JSON/envelope cannot be trusted | Client bug or hostile traffic; do not retry unchanged |
| `INVALID_ARGUMENT` | Known operation with invalid typed/bounded value | Correct request |
| `UNSUPPORTED_VERSION` | Control version is not 1 | Use compatible client/server |
| `UNKNOWN_OPERATION` / `UNKNOWN_TITLE` | Unsupported operation / unprovisioned title | Capability/configuration check |
| `UNAUTHENTICATED` | Missing, wrong, expired or revoked authority | Refresh or sign in |
| `AUTHENTICATION_FAILED` | Username/password did not authenticate | User correction; watch attack rate |
| `NOT_AUTHORIZED` | Identity exists but policy/membership forbids action | Do not turn into not-found retries |
| `NOT_FOUND` | Scoped object absent or invisible | Refresh state |
| `CONFLICT` / `INVALID_STATE` | State transition cannot apply now | Re-read truth and reconcile |
| `DUPLICATE_REQUEST` | ID reused incompatibly or secret result cannot replay | Generate a new ID only for a new intent |
| `RATE_LIMITED` / `LIMIT_EXCEEDED` | Time/resource quota or hard bound | Back off or reduce input |
| `UPDATE_REQUIRED` | Title policy rejects this game version | Direct player to update |
| `INTERNAL_ERROR` | Storage/implementation failed without details | Alert operator; safe retry depends on ID semantics |

## Limits as a defense system

The normative spec owns exact field limits. Important outer bounds are 64 KiB control messages,
8 KiB HTTP headers, 16 levels of JSON nesting, 256 generic array items/object fields, 64-character
identifiers, 256 control connections, 32 per address and 600 new connections per address/minute.
Sign-in/refresh share a per-address minute budget. Mutation request IDs have per-account and
per-title daily budgets.

Limits layer rather than replace one another: admission acts before TLS; Beast caps transport;
Protocol caps syntax; each operation caps semantics; authentication and policy then authorize.
