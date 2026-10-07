# CNA Gamer Services load lab

`loadlab.py` is the canonical realistic load/soak driver. It provisions disposable state, starts
the real TLS server with private diagnostics, and drives only public HTTPS/WSS interfaces. It never
opens the database and never accepts a production database path.

```sh
python3 tools/cna-gamer-services-loadlab/loadlab.py \
  --build build-release --players 24 --workers 6 --seconds 30 \
  --scenarios ordinary,signin-storm,refresh-storm,session-churn,directory-pressure \
  --json /tmp/loadlab.json --html /tmp/loadlab.html

# Short correctness-oriented CI/local check:
python3 tools/cna-gamer-services-loadlab/loadlab.py --build build-agent --smoke

# A bounded soak (aggregate output only):
python3 tools/cna-gamer-services-loadlab/loadlab.py --build build-release \
  --players 32 --workers 6 --seconds 3600 --scenarios soak --json /tmp/soak.json
```

Scenarios:

- `ordinary`: heartbeat, social/profile/achievement/leaderboard reads, presence and directory mix.
- `signin-storm`: repeated password authentication from rotating loopback source addresses.
- `refresh-storm`: returning-player refresh rotation, distinct from password derivation.
- `session-churn`: repeated create/touch/leave cycles.
- `directory-pressure`: live advertised sessions plus repeated searches.
- `reconnect-storm`: ordinary requests with a fresh TLS connection each time.
- `host-migration`: two-player create/join/host-leave/replacement/last-leave cycles.
- `events`: long-lived authenticated event channels receiving friend/message hints.
- `relay`: authenticated relay ring with selectable packet shape (`small`, `large`, `voice`,
  `burst`, `idle`, `asymmetric`). Up to 31 players share one test session.
- `soak`: the ordinary mix plus periodic process/database/connection snapshots; intended for long
  runs that reveal RSS, descriptor, database-size or latency drift.

The tool uses threads for blocking clients, caps `--workers` at six, retains at most 50,000 latency
samples per scenario, and writes no per-request or per-packet log. Reports contain commit/dirty
state, timestamp, compiler/build/OS/CPU metadata, exact configuration, successes, errors,
throughput, latency percentiles, connection/relay statistics and sampled server resources.

Results are loopback saturation evidence. Compare revisions on the same idle machine with identical
arguments; do not interpret them as public-Internet latency. No universal pass/fail performance
threshold is embedded. Smoke fails only for setup failure, server exit, transport failure,
`INTERNAL_ERROR`, or a scenario that performs no successful work.

The older `tests/service_benchmark.py` remains useful historical evidence and descriptor/replay
coverage. Loadlab does not silently redefine those committed measurements.
