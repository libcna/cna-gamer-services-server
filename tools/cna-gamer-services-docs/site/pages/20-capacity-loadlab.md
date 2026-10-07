---
title: Capacity, benchmark methodology and loadlab
summary: Honest interpretation of loopback evidence, comparable measurement practice and realistic workload/soak tooling.
order: 20
---

## What existing numbers mean

The repository keeps curated historical benchmark JSON and narrative, not universal promises. A
committed Release run on an AMD Ryzen 7 PRO 7840U measured roughly 1,210 requests/s steady with 64
closed-loop players; load on the shared host changed results substantially minutes apart. Idle real
players cost far less than saturation clients—normally heartbeat-scale traffic.

Loopback removes public latency, loss, middleboxes and remote TLS effects. Closed-loop clients also
reduce offered load when responses slow. Use the result to compare code revisions on the same
machine/configuration, not to size every deployment.

## Comparable measurement checklist

1. Use `release`, ccache and the same compiler/dependency versions.
2. Record commit plus dirty state; do not compare an unidentified tree.
3. Use the same players, workers, duration, scenarios, packet shape and keep-alive policy.
4. Record CPU/OS/build type and concurrent host load.
5. Warm similarly; run enough duration for stable percentiles.
6. Check correctness/errors, resource growth and server metrics before throughput.
7. Change one hypothesis, rerun on the same machine, then run conformance.
8. Never improve a number by weakening durability, validation or work performed.

## Loadlab quick start

```sh
cmake --preset release && cmake --build --preset release
python3 tools/cna-gamer-services-loadlab/loadlab.py \
  --build build-release --players 24 --workers 6 --seconds 30 \
  --scenarios ordinary,signin-storm,refresh-storm,session-churn,directory-pressure \
  --json /tmp/loadlab.json --html /tmp/loadlab.html
```

Loadlab creates its own title/accounts/certificate/database. It uses at most six worker threads and
only public TLS/WSS plus diagnostics. Results include metadata, exact configuration, request and
success/failure counts, errors, throughput, sampled latency percentiles, connection/relay stats and
process RSS/fd/database snapshots. Latency samples are reservoir-bounded; packets are aggregated.

## Scenario selection

`ordinary` mixes heartbeat, friends, presence, profile, achievements, leaderboards and directory.
`signin-storm` isolates scrypt pressure; `refresh-storm` isolates rotating returning credentials.
`session-churn`, `directory-pressure` and `host-migration` stress online state. `events` opens
authenticated long-lived channels and produces hints. `relay` supports small, large, voice-cadence,
burst, mostly-idle and asymmetric shapes. `reconnect-storm` pays a fresh TLS connection repeatedly.

`soak` keeps one process and samples RSS, high-water RSS, thread/fd count, database size and live
connections every 30 seconds. Run it long enough to expose drift, but keep output aggregated:

```sh
python3 tools/cna-gamer-services-loadlab/loadlab.py --build build-release \
  --players 32 --workers 6 --seconds 3600 --scenarios soak --json /tmp/soak.json
```

Investigate monotonic RSS/fd/table growth by reproducing the smallest scenario before profiling.
