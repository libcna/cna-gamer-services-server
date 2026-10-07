---
title: Labs 13–17 — observe, back up, restore, load and break safely
summary: Repeatable operator exercises with bounded reports and disposable state, ending in explicit evidence statements.
order: 34
---

Every command here either reads a local endpoint or creates its own scratch state. For manual
backup/restore, use only the disposable `$DB` from the foundation lab.

## Lab 13 — diagnose from bounded telemetry

```sh
curl --fail http://127.0.0.1:47832/healthz
curl --fail http://127.0.0.1:47832/readyz
curl --fail http://127.0.0.1:47832/metrics > /tmp/cna-metrics-before.txt
```

Perform one successful `hello`, one authenticated read and one invalid request, then scrape again.
Locate the operation counter, outcome counter and latency histogram changes. Confirm labels contain
no gamertag, title, session, token or address. Restart with `--log-format json`, parse each line with
Python's `json` module and identify startup versus minute-stat records. Readiness must fail for a
wrong schema/foreign-key state; health alone deliberately does not prove storage readiness.

## Lab 14 — online backup while serving

```sh
mkdir -m 700 "$LAB/backups"
python3 tools/cna-gamer-services-deploy/backup.py \
  --database "$DB" --output-dir "$LAB/backups" --dry-run
python3 tools/cna-gamer-services-deploy/backup.py \
  --database "$DB" --output-dir "$LAB/backups"
ls -l "$LAB/backups"
```

The real run uses SQLite's backup API, checks integrity and foreign keys, fsyncs, sets mode 0600 and
publishes with atomic rename. Keep serving traffic during it. A plain filesystem copy of the live
main file is not an equivalent test because committed pages may still live in WAL.

## Lab 15 — restore without overwriting evidence

Stop the scratch server. Select the timestamped backup explicitly and restore to a path that does
not exist:

```sh
BACKUP=$(find "$LAB/backups" -maxdepth 1 -name '*.sqlite3' -type f | sort | tail -n 1)
python3 tools/cna-gamer-services-deploy/restore.py \
  --backup "$BACKUP" --database "$LAB/restored/service.sqlite3" --create-directory --dry-run
python3 tools/cna-gamer-services-deploy/restore.py \
  --backup "$BACKUP" --database "$LAB/restored/service.sqlite3" --create-directory
```

Start a server on the restored database and repeat profile, earned and leaderboard reads. The
automated stronger proof also verifies access/refresh and social data:

```sh
CTEST_PARALLEL_LEVEL=1 ctest --test-dir build-agent \
  -R '^service_backup_restore$' --output-on-failure
```

## Lab 16 — compare bounded load, not anecdotes

```sh
cmake --preset release
cmake --build --preset release
python3 tools/cna-gamer-services-loadlab/loadlab.py \
  --build build-release --players 16 --workers 6 --seconds 10 \
  --scenarios ordinary,signin-storm,refresh-storm,session-churn,directory-pressure \
  --json /tmp/cna-load.json --html /tmp/cna-load.html
```

Record commit/dirty state, compiler, build type, CPU/OS, exact workload, successes/failures,
throughput, percentiles and ending resources. Compare only the same machine/configuration. A closed
loopback run removes Internet latency and therefore cannot establish public capacity.

## Lab 17 — controlled failure and evidence language

```sh
python3 tools/cna-gamer-services-chaos/chaos.py \
  --build build-agent --smoke --json /tmp/cna-chaos.json
```

For every case answer: what was killed/corrupted, who owned it, what state was expected durable,
what postcondition proved recovery, and what real failure was not modeled? Verify the tool accepts
no database argument and uses a generated private directory. Its current scope proves process
SIGTERM/SIGKILL recovery, durable reconnect, exactly-once disconnect retry, busy timeout recovery,
hostile transport containment, single-owner refusal and corrupt-copy rejection.

Finish by writing a one-paragraph handoff: “On this Linux host and commit, these loopback/scratch
checks passed; CNA harness/public Internet/OS crash/power loss were not exercised.” Honest scope is
part of the lab, not a disclaimer added later.
