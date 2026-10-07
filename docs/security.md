# Security model and review notes

## Trust boundaries

The TLS control listener is hostile-input territory. Accounts are administrator-provisioned, but
an authenticated player can still be malicious or compromised. The separate diagnostics listener
trusts the local host and is therefore forced to a loopback address. The administrator CLI and
backup tools operate directly on an operator-selected database and must run under administrative
OS permissions.

The service never trusts a client-supplied relay identity. A one-use relay ticket binds title,
directory session, account and machine; the relay replaces the frame's source with that server
grant and revalidates durable membership. Title IDs scope credentials and stored title data.

## Input and resource defenses

- Beast caps HTTP headers at 8 KiB and bodies at the protocol maximum, with ten-second first-read
  and TLS-handshake deadlines and five-second keep-alive idle time.
- JSON parsing caps bytes, nesting, object fields and array entries before request-specific checks.
  Unknown fields and duplicate keys are rejected.
- WebSocket/event and relay handshakes use exact envelopes; binary relay frames have fixed bounds.
- SQL is fixed in the binary and values use prepared-statement bindings. Hash-addressed file access
  accepts only 64 lowercase hexadecimal characters; it never maps a client path to a filesystem.
- Per-address admission, connection and sign-in limits contain one source. They are not DDoS
  protection against many source addresses; deployment needs an upstream L4 control.
- Access tokens, rotating refresh families and relay tickets are generated with OpenSSL's CSPRNG
  and stored as hashes where used as database authority. Refresh replay revokes the family.
- Public errors are stable codes and do not contain database, parser, path or cryptographic detail.

## Secret and privacy handling

Passwords enter the admin CLI on stdin and are immediately converted to scrypt verifiers. Access
tokens, refresh tokens, relay tickets and private keys never enter logs, metrics or diagnostic
responses. Metrics do not label usernames, gamertags, addresses, title IDs, session IDs or token
identifiers. Minute logs deliberately omit peer addresses and request bodies.

The database contains account, friendship, block, message, profile and gameplay state and should
be treated as sensitive. Backup files are mode 0600 but still require off-host encryption and
access controls. Diagnostics expose aggregate service/database state and should remain loopback
only even though they contain no player identifiers.

## Administrative safety

There is no arbitrary-SQL command. Destructive CLI commands name a title/account scope. Restore
refuses to overwrite an existing database; the operator must preserve or explicitly relocate the
old database and its WAL/SHM sidecars first. Backup retention is disabled unless `--retain` is
specified.

## Residual risks and non-goals

- The server is a single machine/process, not a distributed authority or DDoS mitigation layer.
- Cooperative clients enforce voice communication policy; relay payloads are intentionally opaque.
- Certificate renewal requires a process restart.
- Diagnostics has no application authentication because it cannot bind off-loopback. Host access
  control or an authenticated tunnel remains necessary.
- Public-Internet penetration testing and third-party security review were not performed.

Security-sensitive change workflow: first identify the invariant and attacker, add a negative
regression test, make the narrow fix, run unit plus black-box conformance, then update this document
if the operational boundary changed.
