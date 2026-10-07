---
title: Security, privacy, rate limiting and TLS
summary: Threat actors, layered input defenses, authorization, secret hygiene, admission limits and residual risks.
order: 18
---

## Threat model

Assume unauthenticated Internet clients, authenticated malicious players, compromised accounts,
replay attackers, malicious relay members, corrupted packets and mistakes in trusted admin input.
The service does not claim to defeat distributed denial of service, malicious host administrators
or compromised operating systems.

## Defense layers

```diagram
Internet peer -> Admission: global/per-address connection and opening-rate caps
Admission -> TLS: handshake deadline, TLS 1.2 minimum
TLS -> HTTP/WebSocket parser: header/body/frame limits
Parser -> Protocol validator: JSON depth/count/type/exact fields
Validator -> Authentication: title-scoped live credential
Authentication -> Authorization: friendship/privacy/session role
Authorization -> SQLite: fixed SQL and transaction constraints
```

One address can hold at most 32 of 256 pre-auth control connections and open 600/minute. Sign-in
and refresh share a stricter per-address budget. Authenticated relay/event hubs have independent
server caps. Many addresses can still create distributed TLS cost; put L4 flood protection ahead
of a public deployment.

## Credential and randomness rules

OpenSSL CSPRNG creates account IDs, tokens, refresh values, tickets and request authority. Passwords
become salted scrypt verifiers. Token/ticket database authority uses hashes. Refresh replay revokes
the family. Error responses never carry SQL, crypto or filesystem details.

No log, metric, diagnostic, report or admin page may contain passwords, verifiers, access/refresh
tokens, relay tickets or private keys. Avoid usernames/gamertags/IP/session IDs in metric labels;
bounded aggregate labels prevent both privacy leakage and attacker-created time series.

## Privacy and blocks

Communication, profile viewing and user content may be `everyone`, `friends` or `blocked`; trade,
purchase and premium are allowed/blocked policy. Bilateral block relationships override social
access. Policy is returned at sign-in and heartbeat and enforced by server operations. Opaque voice
uses cooperative client policy, a documented limitation.

## TLS and proxies

Clients verify chain and hostname. Mount the private key read-only and mode 0600. Renewal requires
restart, so stage and monitor reconnect. The supplied nginx example is raw TCP passthrough: the
service still sees TLS and client source (subject to network topology). An HTTP-terminating proxy
can collapse source addresses and must explicitly support both WebSocket routes; it is not the
recommended default.

## Admin plane

Admin web is a separate process, loopback-first, password/CSRF protected and optionally read-only.
Remote exposure requires explicit opt-in plus TLS; an SSH tunnel is preferred. The CLI and web have
the OS permissions of their process, so local account isolation matters.

Residual risks and evidence boundaries are maintained in repository `docs/security.md` and the
[known limitations chapter](25-limitations-glossary.html).
