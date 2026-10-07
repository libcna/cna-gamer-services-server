---
title: Maintainer workflow and safe change checklists
summary: How one developer can change implementation, protocol or schema with focused evidence and minimal maintenance burden.
order: 24
---

## Make a server change safely

1. State the invariant and user-visible behavior in one sentence.
2. Read the normative protocol plus implementation-map row and existing focused tests.
3. Reproduce the problem or write the smallest failing regression test.
4. Implement locally using existing ownership, validation and error idioms.
5. Run the focused target, then `tools/qualify.sh quick`.
6. Run black-box conformance for compatibility; run chaos/security/performance only when relevant.
7. Run `normal` or `full` at the milestone, inspect diff and ccache stats.
8. Update operator/site docs when behavior, configuration or an invariant changed.

Avoid broad refactors around a bug fix. Comments should explain why a lock, bound or transaction
exists. Prefer a fixed direct helper over a general framework used once.

## Change the protocol safely

Protocol v1 and relay v1 are sacred compatibility contracts. First ask whether diagnostics/admin
can solve the need outside player protocol. If extension is necessary, make it optional,
backward-compatible and capability-gated. Update specification, vendored header/constants, golden
vectors, parser/dispatcher, unit tests, black-box conformance and educational docs together.

Never rename/remove operations, reinterpret errors, require formerly optional data, weaken exact
envelopes or change relay framing without a versioned compatibility design. No campaign change may
require a CNA source modification.

## Database migration checklist

Use the detailed steps in [database architecture](16-database.html). Additionally create a realistic
old-schema fixture or upgrade path, verify rollback on injected failure, measure important query
plans and ensure backup/rollback procedure acknowledges forward-only schema.

## Concurrency/lifetime checklist

Name the owning executor/thread, object lifetime and mutex order. Never block network threads with
Store/scrypt. Avoid callbacks under Service/hub locks. Test disconnect and shutdown while work is in
flight, then run TSan on the smallest meaningful subset. A shared pointer is not automatically a
correct ownership explanation.

## Daily and release habits

- Before work: `git status`, confirm branch, read the nearest tests/spec.
- During work: incremental ccache build, focused test, no more than six jobs.
- Before commit: `git diff --check`, inspect every changed file, docs checker, no secrets/generated output.
- Before release: full qualification, sanitizer subset, conformance, backup/restore, chaos/load smoke, explicit CNA skips.
- After release: readiness, build info, errors/refusals/latency, verified off-host backup.

## When to optimize

Create a reproducible workload, profile/measure, change one bottleneck, compare on the same machine,
and re-run correctness. Do not trade clear transactions, validation or security for a small number.
