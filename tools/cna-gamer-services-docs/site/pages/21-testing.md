---
title: Testing, conformance, chaos, sanitizers and malformed input
summary: Layered regression strategy, exact tool commands, skips versus passes and focused adversarial qualification.
order: 21
---

## Test layers

| Layer | Purpose | Typical command |
|---|---|---|
| Unit/component | fast domain, protocol, crypto, DB and concurrency invariants | `tools/qualify.sh quick` |
| Local integration | real TLS/HTTP/WSS/process/restart | `tools/qualify.sh normal` |
| Black-box conformance | public compatibility independent of Store internals | conformance command below |
| Recovery/chaos | crash, busy DB, disconnect, hostile transport, corrupt copy | chaos command below |
| Load/performance | realistic workload and regression evidence | loadlab / performance tier |
| Sanitizers | memory/UB/thread errors | security preset / TSan focused run |
| CNA harness | standard public CNA/XNA behavior | configured external executables; exit 77 otherwise |

## Conformance guide

```sh
python3 tools/cna-gamer-services-conformance/conformance.py \
  --build build-agent --json /tmp/conformance.json
```

It provisions scratch state only through the administrator, then uses verified HTTPS/WSS and never
opens SQLite. Stateful cases cover envelope/version, authentication/isolation, progress/assets,
refresh replay, social/replay, boundaries, session/invitation, relay tickets/framing/routing,
events, host migration and privacy. The default 21-case profile includes a deliberate wait proving
wall-clock relay-ticket expiry; `--smoke` runs 20 cases and skips only that wait. Results include
build/host metadata and a loopback evidence boundary. A failed case is a nonzero exit.

## Chaos guide

```sh
python3 tools/cna-gamer-services-chaos/chaos.py --build build-agent --json /tmp/chaos.json
```

The tool cannot accept a database path. It owns every process it signals and every file it corrupts.
It verifies SIGTERM/SIGKILL recovery, active session/event/relay reconnect, disconnect/retry exactly
once, busy timeout recovery, hostile connections, ownership locking and corrupt-copy refusal.
Process crash is not power-loss evidence.

## Sanitizers and static analysis

```sh
tools/qualify.sh security
cmake --preset tsan && cmake --build --preset tsan
CTEST_PARALLEL_LEVEL=1 ctest --test-dir build-tsan -L unit --output-on-failure
```

ASan/UBSan use persistent ccache trees. Sanitizer builds and tests are deliberately serial because
instrumented C++ translation units have a high peak memory cost. TSan is meaningful for
admission/hubs/lifetime tests but may be expensive and platform-sensitive; investigate findings
instead of broad suppression. Compiler warnings are `-Wall -Wextra -Wpedantic -Werror`. Use
clang-tidy/cppcheck only when installed and focused—mechanical churn is not a security result.

## Malformed/generative testing

Protocol unit tests mutate/truncate golden envelopes, bound JSON arrays/objects/depth/UTF-8 and
exercise numeric/schema edges. Relay golden vectors and flow tests cover binary frame limits.
Avatar validation exercises bounded binary/catalog parsing. Prefer deterministic small corpora and
sanitizer runs; do not write millions of redundant files.

## Interpreting results

An explicit exit-77 CNA harness is “unavailable,” not green. A loopback pass is not NAT/public
Internet evidence. Benchmark smoke asserts correctness/survival, not a speed threshold. Preserve
the exact failing command and seed/fixture before changing code.
