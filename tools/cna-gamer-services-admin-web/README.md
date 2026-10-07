# CNA Gamer Services administrator web

This is a small, server-rendered Python standard-library control plane. It is a separate process
from the public player listener and has no arbitrary-SQL facility or frontend build system. Reads
use fixed, parameterized SQLite queries; mutations invoke the existing C++ administrator CLI so
validation and data semantics stay in one place.

Create a mode-0600 password file without putting the secret in shell history, then bind loopback:

```sh
umask 077
read -rsp 'Admin password: ' ADMIN_PASSWORD; printf '%s\n' "$ADMIN_PASSWORD" > /run/user/$UID/cna-admin-password
unset ADMIN_PASSWORD
python3 tools/cna-gamer-services-admin-web/admin_web.py \
  --database /var/lib/cna-gamer-services/service.sqlite3 \
  --admin build-agent/cna-gamer-services-admin \
  --password-file /run/user/$UID/cna-admin-password
```

Open `http://127.0.0.1:47833/`. Use `--port 0` for an OS-chosen port, and `--read-only` for an
inspection-only session. The password file is rejected if group/other permission bits are set.

The default address must be loopback. Remote binding requires all three of `--allow-remote`,
`--tls-cert` and `--tls-key`; this explicit gate exists because the pages expose private operator
state. Prefer an SSH tunnel to the loopback listener:

```sh
ssh -L 47833:127.0.0.1:47833 server.example.org
```

Security properties:

- constant-time password comparison;
- at most ten failed sign-ins per source per minute, with a bounded 256-source tracker;
- at most sixteen simultaneous request-handler threads;
- random in-memory sessions, one-hour idle expiry, maximum 64 sessions;
- `HttpOnly`, `SameSite=Strict` cookies (`Secure` under TLS);
- per-session CSRF token on every mutation and logout;
- no secret in a URL, log, HTML response or database query;
- strict body/header limits, CSP, no framing, no caching and MIME sniff prevention;
- destructive resets require typing the exact title ID;
- uploaded assets go through a private temporary file and the C++ validator;
- no message bodies, password verifiers, tokens or relay tickets are displayed.

The dashboard covers schema/count health; titles and minimum versions; account search/provisioning,
credential revocation and privileges; achievement and leaderboard catalogs; asset import and gamer
pictures; avatar catalogs/account metadata; active sessions, memberships, invitations and party
state; privacy/social metadata; and explicitly confirmed maintenance resets.

This process has the same OS-level authority as its account. Keep the database, admin executable,
password file and optional TLS key accessible only to that account. Loopback is a boundary, not a
substitute for controlling local logins.
