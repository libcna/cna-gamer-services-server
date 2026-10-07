---
title: Known limitations, glossary and FAQ
summary: Honest non-goals, evidence gaps, concise definitions and answers to the questions a new owner will ask first.
order: 25
---

## Known limitations

- One process, one machine and one SQLite writer; no horizontal scaling or cross-node relay/event authority.
- No Xbox LIVE compatibility, self-registration, email password reset, Marketplace, PartnerToken or update delivery.
- No TrueSkill computation or exact recent-time-window leaderboard semantics.
- All online game/voice datagrams relay; no direct peer-to-peer path.
- Opaque voice privacy is cooperatively enforced by clients, not inspected centrally.
- Distributed floods from many addresses require upstream protection.
- Certificate rotation requires restart.
- Per-account request budget is process memory and restarts reset it; durable per-title IDs remain.
- Linux loopback/NAT harness evidence is not public-Internet, macOS or native Windows validation.
- External CNA harnesses/catalog fixtures are explicit skips when unavailable.

## Glossary

| Term | Meaning here |
|---|---|
| Access token | Random one-hour title-scoped request authority, stored hashed server-side |
| Refresh family | Rotating 30-day credential lineage; replay revokes all descendants |
| Title | Provisioned game namespace isolating credentials/content/progress/session state |
| Directory session | Persistent advertised PlayerMatch/Ranked membership and policy |
| Machine | One network participant containing one to four local gamer accounts |
| Member | An account occupying a session ordinal/slot on a machine |
| Host migration | Transactional selection of a remaining machine after host departure/expiry |
| Party | Account-global friend group, separate from a game session |
| Event hint | Non-durable prompt to re-read authoritative state |
| Relay ticket | Short-lived one-use proof binding credentials to current machine/session |
| Relay grant | Server-owned in-memory authority after ticket redemption |
| Recorded request | Mutation whose ID/outcome commits with its state for replay safety |
| Gameplay round | Boundary grouping leaderboard reports/commits |
| Projection | Valid avatar mapped to a catalog version a client can render |
| Readiness | Exact expected schema plus usable foreign-key-enabled SQLite connection |

## FAQ

### Why SQLite?

It matches the intended single-machine authority, offers strong transactions/WAL/online backup and
keeps operations comprehensible. Replacing it would not solve in-memory relay/event distribution.

### Can two servers share the database?

No. The second is locked out. Shared storage would still split rate limits, socket routes and
session authority.

### Why are events only hints?

It makes reconnect/loss simple: the database/read operation remains one source of truth. A durable
event log would add ordering, cursor retention and another correctness boundary.

### Why did refresh replay sign out a valid-looking token?

The old refresh value proved the family was copied or response state was lost. Revoking descendants
is safer than allowing two rotating owners.

### Can diagnostics be public with a firewall password?

There is no diagnostics application password. It is forced loopback; use a collector or SSH tunnel.

### Is the admin web a replacement for the CLI?

No. It is a safer ordinary UI that delegates mutations to the CLI. The CLI remains the scriptable,
minimal recovery tool.

### What should I read next?

Follow the [learning paths and labs](26-learning-labs.html), then use the protocol specification as
normative detail while tracing tests/source.
