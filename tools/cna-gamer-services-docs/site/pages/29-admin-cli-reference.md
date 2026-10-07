---
title: Administrator CLI reference
summary: Exact syntax, stdin contracts, output and safety behavior for every C++ administrator command.
order: 29
---

Every invocation has the form `cna-gamer-services-admin DATABASE COMMAND ...`. The CLI opens and
migrates the selected database, uses fixed SQL/Store validators, prints a stable error to stderr and
returns nonzero on refusal. It has no arbitrary-SQL mode. A running server may remain open for
small WAL-safe administrative operations, but schedule large imports/resets and take an online
backup first.

Set reusable paths without embedding secrets:

```sh
ADMIN=build-agent/cna-gamer-services-admin
DB=/var/lib/cna-gamer-services/service.sqlite3
```

## Provisioning and policy

| Syntax | Input/output and notes |
|---|---|
| `$ADMIN "$DB" title ID NAME` | Creates or updates a title display name. `ID` is the `game` value clients use. |
| `$ADMIN "$DB" title-minimum-version ID VERSION` | Sets the minimum dotted version; pass an explicit empty argument to clear it. Older/omitting clients get `UPDATE_REQUIRED`. |
| `$ADMIN "$DB" user USERNAME GAMERTAG` | Reads one password line from stdin and prints the new 32-hex internal user ID. Never put the password in argv. |
| `$ADMIN "$DB" privilege USERNAME NAME VALUE` | `communication`, `profileViewing`, `userContent`: `everyone`/`friends`/`blocked`; `trade`, `purchase`, `premium`: `allowed`/`blocked`. |
| `$ADMIN "$DB" game-defaults USERNAME` | Reads the validated GameDefaults object from stdin and replaces the account-wide value. |

Safe account creation:

```sh
read -rsp 'Initial player password: ' PLAYER_PASSWORD
printf '%s\n' "$PLAYER_PASSWORD" | "$ADMIN" "$DB" user alice Alice
unset PLAYER_PASSWORD
```

## Catalogs and immutable assets

| Syntax | Input/output and notes |
|---|---|
| `$ADMIN "$DB" achievement TITLE` | Reads one achievement JSON document from stdin. Definition keys are immutable once earned semantics depend on them. |
| `$ADMIN "$DB" leaderboard TITLE` | Reads a board definition JSON document from stdin. Key/mode policy cannot silently drift. |
| `$ADMIN "$DB" seed-leaderboard TITLE` | Reads one fixture row JSON. This is administrative/test data, not gameplay trust. |
| `$ADMIN "$DB" asset TITLE MIME FILE` | Validates PNG/GLB, imports immutable bytes and prints the lowercase SHA-256 hash. |
| `$ADMIN "$DB" picture USERNAME HASH` | Assigns an already imported, view-authorized asset as gamer picture. |
| `$ADMIN "$DB" avatar-catalog DIRECTORY` | Reads `catalog.json` and every named file, verifies names/hashes/sizes/models and prints the catalog version. |
| `$ADMIN "$DB" avatar USERNAME random [female\|male]` | Generates from the newest validated catalog. Fails if no catalog exists. |
| `$ADMIN "$DB" avatar USERNAME set` | Reads exactly 2,042 lowercase hex characters from stdin and validates the description/catalog. |
| `$ADMIN "$DB" avatar USERNAME clear` | Removes the account avatar; this is destructive profile maintenance. |

Prefer files and redirection for non-secret JSON so shell quoting remains reviewable:

```sh
"$ADMIN" "$DB" leaderboard my-game < board.json
"$ADMIN" "$DB" achievement my-game < achievement.json
```

## Inspection

| Syntax | Output |
|---|---|
| `$ADMIN "$DB" inspect` | Four newline-separated counts: users, titles, access sessions and earned rows. It prints no secrets. |
| `$ADMIN "$DB" inspect-online TITLE` | JSON counts for directory sessions, members, invitations, sender-limit rows and relay tickets. |

For richer normal inspection use the authenticated [administrator web](04-administration.html),
which uses fixed read-only queries and deliberately hides message bodies, credentials and tickets.

## Credentials and destructive maintenance

| Syntax | Exact scope |
|---|---|
| `$ADMIN "$DB" revoke-user USERNAME` | Deletes all refresh families and access sessions for that account. Existing event/relay authority fails on revalidation. |
| `$ADMIN "$DB" expire-access USERNAME` | Expires current access sessions but deliberately leaves refresh families; primarily useful for maintenance tests. |
| `$ADMIN "$DB" reset-earned TITLE` | Deletes every earned-achievement row for that title. Accounts/catalogs remain. |
| `$ADMIN "$DB" reset-online TITLE` | Deletes title directory sessions; membership/invitation/ticket rows cascade, while accounts/scores and invite abuse counters remain. |

The CLI itself is noninteractive, so a shell invocation is the explicit authorization. The web UI
adds typed title confirmation for the two reset commands. Before either reset: name the title,
capture an online backup, inspect counts, stop affected gameplay if possible, run the command once,
then verify readiness and the intended counts. Never discover a target with a wildcard and feed it
directly into a destructive command.

## Concurrent-access failures

SQLite permits these tools beside the server but still has one writer. A busy failure means the
operation did not gain the writer transaction; do not loop without a bound. The OS ownership lock
prevents a second server, not an administrator process. Catalog import can be CPU/disk heavy and is
best staged off-peak. Backup uses the separate online-backup tool; never copy a live `.sqlite3`
file and ignore its WAL.
