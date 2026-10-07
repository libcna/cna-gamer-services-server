---
title: Labs 1–4 — build, provision and negotiate
summary: Copy-paste-safe foundation labs using one disposable database and an insecure loopback-only listener.
order: 31
---

These labs turn the mental model into a running process. They intentionally use plain HTTP only on
numeric loopback; production must use TLS. Work from the repository root and never substitute a
production path.

## Lab workspace

```sh
LAB=$(mktemp -d /tmp/cna-learning.XXXXXX)
chmod 700 "$LAB"
DB=$LAB/service.sqlite3
ADMIN=build-agent/cna-gamer-services-admin
SERVER=build-agent/cna-gamer-services-server
```

Keep the terminal open: those variables identify disposable state. At the end, stop the server,
inspect the path, and remove only the exact directory printed by `printf '%s\n' "$LAB"`.

## Lab 1 — build and qualify

```sh
ccache --version
cmake --preset dev
cmake --build --preset dev
tools/qualify.sh quick
ccache --show-stats
```

Confirm the preset says `CMAKE_CXX_COMPILER_LAUNCHER=ccache`, ordinary Ninja builds never exceed six
jobs (sanitizer builds use one), and the unit label reports nineteen component executables. A later
build should show cache hits or no work;
do not clean the tree just to obtain a prettier statistic.

## Lab 2 — create a title

```sh
"$ADMIN" "$DB" title learning 'Learning title'
"$ADMIN" "$DB" title-minimum-version learning ''
"$ADMIN" "$DB" inspect
```

The second command explicitly permits all client versions. `inspect` prints counts rather than
rows or secrets. Explain why the title ID—not display name—is the stable protocol scope.

## Lab 3 — create two accounts

```sh
read -rsp 'Alice password: ' ALICE_PASSWORD; printf '\n'
printf '%s\n' "$ALICE_PASSWORD" | "$ADMIN" "$DB" user alice Alice
read -rsp 'Bob password: ' BOB_PASSWORD; printf '\n'
printf '%s\n' "$BOB_PASSWORD" | "$ADMIN" "$DB" user bob Bob
```

The CLI prints internal user IDs. Record them in the lab notes but do not confuse them with access
tokens. Passwords travel on stdin and become salted scrypt verifiers; neither value belongs in a
command argument, URL or log.

## Lab 4 — start and negotiate

In a second terminal, using the printed `$LAB` path:

```sh
build-agent/cna-gamer-services-server \
  --database "$LAB/service.sqlite3" \
  --listen 127.0.0.1 --port 47831 --insecure-loopback \
  --diagnostics-listen 127.0.0.1 --diagnostics-port 47832
```

From the first terminal:

```sh
printf '%s' '{"v":1,"id":"lab-hello-1","game":"learning","op":"hello","args":{}}' |
  curl --silent --show-error --fail-with-body \
    -H 'Content-Type: application/json' --data-binary @- http://127.0.0.1:47831/cna/v1
curl --fail http://127.0.0.1:47832/readyz
```

Verify response version 1, the echoed ID and advertised capabilities. Then change `v` to 2 and
observe `UNSUPPORTED_VERSION`; add an unexpected outer key and observe `MALFORMED_MESSAGE`. These
are protocol results carried by HTTP 200, not HTTP routing failures.

Finally run the independent suite, which creates its own database and TLS certificate:

```sh
python3 tools/cna-gamer-services-conformance/conformance.py \
  --build build-agent --json "$LAB/conformance.json"
python3 - "$LAB/conformance.json" <<'PY'
import json, sys
report = json.load(open(sys.argv[1]))
print(report["summary"])
PY
```

The report must say loopback evidence. It does not certify a reverse proxy, LAN, public Internet or
real CNA harness. Continue with [social and progress labs](32-labs-social-progress.html).
