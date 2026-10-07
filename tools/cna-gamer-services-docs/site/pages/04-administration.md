---
title: Provisioning and administration
summary: Safe title/player setup, every administrator CLI family, the browser control plane and its security model.
order: 4
---

## Provision a title and player

```sh
ADMIN=build-agent/cna-gamer-services-admin
DB=/tmp/cna-learning.sqlite3
$ADMIN "$DB" title my-game 'My Game'
read -rsp 'Initial password: ' PASSWORD
printf '%s\n' "$PASSWORD" | $ADMIN "$DB" user alice Alice
unset PASSWORD
```

Title and gamertag identifiers use the protocol identifier alphabet. Passwords go through stdin so
they do not enter shell history or a process argument list. The CLI prints a new internal account
ID, never a credential or verifier.

## CLI command map

| Family | Commands | Safety notes |
|---|---|---|
| Identity | `title`, `title-minimum-version`, `user` | Provisioning only; no client self-registration |
| Catalogs | `achievement`, `leaderboard`, `asset`, `avatar-catalog` | JSON/stdin or validated file inputs |
| Fixtures | `seed-leaderboard` | Development/administrative data, not gameplay trust |
| Profile | `picture`, `avatar`, `game-defaults`, `privilege` | Reuses Store validation and fixed values |
| Inspect | `inspect`, `inspect-online` | Counts, not secrets |
| Credentials | `revoke-user`, `expire-access` | All families vs access tokens only |
| Reset | `reset-earned`, `reset-online` | Destructive and explicitly title-scoped |

JSON examples should be composed in a file or here-document with shell tracing disabled. The
protocol specification defines exact schemas; the admin calls the same Store validators.

## Administrator web interface

The repository's admin web is a separate standard-library process. It reads fixed queries and
invokes the C++ CLI for mutation. It has no arbitrary SQL and no public player route.

```sh
umask 077
read -rsp 'Admin web password: ' P; printf '%s\n' "$P" > /tmp/cna-admin-password; unset P
python3 tools/cna-gamer-services-admin-web/admin_web.py \
  --database "$DB" --admin "$ADMIN" --password-file /tmp/cna-admin-password
```

It shows dashboard counts, titles, searchable accounts, privileges, achievement/leaderboard
catalogs, assets, avatar metadata/catalogs, sessions/invitations/parties and privacy/social metadata.
It provisions accounts, imports validated data, revokes credentials and runs confirmed resets.
Message bodies, password material, tokens and tickets are deliberately absent.

Loopback is the default. Remote binding needs an explicit flag and TLS chain/key; an SSH tunnel to
loopback is preferable. Sessions are random and memory-only, expire after one hour, and carry a
CSRF token. Cookies are HttpOnly and SameSite Strict. `--read-only` makes every mutation return 403.

## Concurrent server and admin access

The server owns runtime authority but SQLite WAL permits bounded administrative access. The admin
tool deliberately does not take the server ownership lock. Writes still contend with the one
SQLite writer and can return a busy/internal failure rather than waiting forever. Schedule large
catalog imports and destructive resets; never bypass the tools with ad-hoc SQL during normal
operation.
