# CNA Gamer Services chaos laboratory

This tool injects failures only into state it creates in a private temporary directory. It accepts
a build directory, never a database path, and refuses to operate without the expected server/admin
binaries. Production data is not an input.

```sh
python3 tools/cna-gamer-services-chaos/chaos.py --build build-agent --smoke
python3 tools/cna-gamer-services-chaos/chaos.py --build build-agent --json /tmp/chaos.json
```

The current suite exercises:

- SIGTERM and restart after a durable user-visible mutation;
- SIGKILL with an active two-player directory session, followed by credential, session, event and
  relay reconnection checks;
- a client disconnect immediately after sending a mutation, followed by request-ID retry proving
  exactly one visible delivery;
- broken, expired and replayed relay tickets failing closed while ordinary control remains healthy;
- a database writer lock held beyond SQLite's busy timeout, recovery after `INTERNAL_ERROR`, and
  continued service;
- a child-process-only file-size ceiling on the scratch server, contained `INTERNAL_ERROR` and
  recovery after an ordinary restart (not a claim about physical-device behavior);
- truncated/plaintext/oversized hostile connections without process loss;
- a second server attempting to own the same database;
- a deliberately corrupted **copy** being rejected while the original passes integrity checking.

Each case records duration and a concise result in a versioned JSON report. No packet-by-packet log
is retained. Smoke and default currently run the same bounded cases.

## What this evidence means

SIGKILL is a process crash. It does not simulate an OS crash, torn storage write, controller-cache
loss or physical power loss. WAL plus `synchronous=FULL` is reviewed and ordinary process-crash
recovery is tested, but the suite makes no claim about hardware it cannot control. A corrupted
database copy tests fail-closed startup, not successful repair. Restore qualification belongs to
`service_backup_restore`; never experiment on the only damaged copy.

Descriptor exhaustion remains covered by the historical benchmark smoke. Migration atomicity and
request crash points have lower-level regression coverage in `service_atomicity` and Store tests.
Public network interruption, filesystem fault injection and a genuine low-disk mount still require
a dedicated disposable VM/filesystem. The file-ceiling case exercises a failed SQLite write without
filling the shared host and states that narrower evidence explicitly.
