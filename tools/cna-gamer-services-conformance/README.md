# CNA Gamer Services conformance

This is the black-box compatibility suite for control protocol v1 and relay protocol v1. It drives
the real TLS/WSS listener and provisions disposable fixtures only through the administrator CLI. It
does not import server implementation code or inspect its database.

## Run it

Build with ccache and no more than six jobs:

```sh
cmake --preset dev
cmake --build --preset dev
python3 -m pip install -r tests/requirements.txt
python3 tools/cna-gamer-services-conformance/conformance.py \
    --build build-agent \
    --json /tmp/cna-conformance.json
```

The command prints one concise line per scenario and exits nonzero if any case fails. `--json`
writes a stable report containing the commit, dirty state, timestamp, compiler, build type, OS, CPU,
configuration, duration and every case result. Temporary databases, certificates and credentials
are created with owner-only permissions and deleted at exit.

The default profile runs 21 cases, including a deliberate roughly 61-second wait that proves a
relay ticket expires according to the server-advertised lifetime. Add `--smoke` for the 20-case
CI/local profile that skips only that wall-clock wait; ticket expiry is still covered by the
scratch-only chaos suite there.

Use `--keep-work-dir` only to diagnose a failure. The retained directory is printed. It contains
throwaway credentials and must never be pointed at a real service database.

The CTest entry is the same black-box run:

```sh
CTEST_PARALLEL_LEVEL=6 ctest --test-dir build-agent -R service_conformance_smoke --output-on-failure
```

## Current coverage

- hello, v1 selection and critical capability discovery;
- malformed JSON, duplicate keys, unexpected envelope fields, bad identifiers and future versions;
- password authentication, logout, invalid credentials, access-token expiry/recovery and title
  isolation;
- refresh rotation, retired access and replay-family revocation;
- request-outcome replay with an achievement and a message;
- identity/profile reads, gamer zone and account-wide game defaults;
- achievement catalog/award, leaderboard definition/list/read, local commit/abort, asset chunks and
  the raw file route;
- friendship establishment, presence/status, full message lifecycle, reviews and reputation;
- representative string, array and identifier boundaries;
- avatar null reads plus empty-catalog and malformed-description behavior (catalog import and valid
  avatar projection remain covered by focused component/CNA-harness tests);
- session create/find/get/update/join/touch/add-local/remove/leave, invitations, join-friend and host
  migration;
- party invite/decline/accept/leave and block/unblock trust reset;
- a two-machine Ranked round whose matching reports resolve an arbitrated leaderboard;
- relay ticket redemption, one-use replay refusal, wall-clock expiry, authenticated source
  injection, unknown-route drop and malformed binary framing;
- event authentication, hints and unexpected hello fields;
- graceful host migration and privacy/block enforcement;
- an executable coverage guard proving every currently dispatched public control operation is
  exercised. The documentation checker fails if the dispatcher and this declared set drift.

Every current operation is exercised, but this is not a claim that every boundary permutation is.
Focused component tests remain the deeper evidence for catalog binary validation, quota exhaustion,
concurrency and crash points. Real CNA harness tests remain separate: an
unavailable harness is an explicit skip, never a conformance pass.

## Result interpretation

The managed profile is verified loopback TLS/WSS. It demonstrates protocol behavior through a real
network listener but does not establish LAN, NAT, remote-Internet latency, reverse-proxy or failover
behavior. Relay datagrams in this suite are small opaque probes, not an ENet compatibility claim;
the separately configured CNA harness tests provide that evidence when available.
