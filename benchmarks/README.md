# Benchmark evidence

Each file here is one run of `tests/service_benchmark.py` with everything needed to repeat it:
server commit, build, compiler, machine, load, the exact command and every scenario's numbers
(throughput, latency percentiles per operation, error codes). One machine, shared with other
work, over loopback: a measurement, not a capacity guarantee.

| File | Server | Steady | Sign-in storm | 160 idle connections | 90,000 request IDs | Descriptors exhausted |
|---|---|---|---|---|---|---|
| [`2026-09-30-824decd.json`](2026-09-30-824decd.json) | `824decd` | 1,210 req/s, p50 48 ms, p99 118 ms | 1,013 req/s, p99 411 ms | 1,161 req/s, no failures | 1,335 req/s | keeps serving |

The same file keeps two comparisons. The server the audit read (`a85a146`) and `824decd`, which
commits each request ID with its change, alternated three times in the same minutes: steady
1,209-1,269 against 1,110-1,265 req/s, sign-in storm 929-1,020 against 821-1,025. The difference
is within the machine's noise. A full run minutes earlier, with other work holding the load
average at 13-17, measured about half: 594 req/s steady.

To add a run:

```sh
cmake -S . -B build-probe -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-probe
python3 tests/service_benchmark.py build-probe --json /tmp/result.json
```

then record the commit, `c++ --version`, `lscpu`, `/proc/loadavg` before and after, and the
results in a new file named `<date>-<commit>.json`.
