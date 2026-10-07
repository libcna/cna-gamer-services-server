#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Create and verify an online SQLite backup; safe while the service is running."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import pathlib
import sqlite3
import sys
import urllib.parse


def read_only_uri(path: pathlib.Path) -> str:
    return "file:" + urllib.parse.quote(str(path.resolve()), safe="/") + "?mode=ro"


def verify(connection: sqlite3.Connection) -> tuple[str, int]:
    integrity = connection.execute("PRAGMA integrity_check").fetchone()[0]
    if integrity != "ok":
        raise RuntimeError(f"integrity_check failed: {integrity}")
    foreign = connection.execute("PRAGMA foreign_key_check").fetchone()
    if foreign is not None:
        raise RuntimeError(f"foreign_key_check failed in table {foreign[0]}")
    version = int(connection.execute("PRAGMA user_version").fetchone()[0])
    return integrity, version


def fsync_path(path: pathlib.Path) -> None:
    descriptor = os.open(path, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def choose_name(database: pathlib.Path, output: pathlib.Path) -> pathlib.Path:
    timestamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    base = output / f"{database.stem}-{timestamp}.sqlite3"
    candidate = base
    suffix = 1
    while candidate.exists():
        candidate = output / f"{base.stem}-{suffix}.sqlite3"
        suffix += 1
    return candidate


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", required=True, type=pathlib.Path)
    parser.add_argument("--output-dir", required=True, type=pathlib.Path)
    parser.add_argument("--retain", type=int, help="after success, keep this many newest matching backups")
    parser.add_argument("--dry-run", action="store_true", help="show the selected destination without writing or pruning")
    options = parser.parse_args()

    database = options.database.resolve()
    if not database.is_file():
        parser.error(f"database is not a regular file: {database}")
    if options.retain is not None and options.retain < 1:
        parser.error("--retain must be at least 1")
    output = options.output_dir.resolve()
    destination = choose_name(database, output)
    if options.dry_run:
        print(json.dumps({"dryRun": True, "database": str(database), "backup": str(destination), "retain": options.retain}))
        return 0

    output.mkdir(parents=True, exist_ok=True, mode=0o700)
    temporary = output / ("." + destination.name + f".tmp-{os.getpid()}")
    if os.path.lexists(temporary):
        raise RuntimeError(f"temporary backup path already exists: {temporary}")
    try:
        source = sqlite3.connect(read_only_uri(database), uri=True, timeout=30)
        target = sqlite3.connect(temporary)
        try:
            # sqlite3_backup takes a consistent snapshot across WAL checkpoints and concurrent
            # commits. It is the supported API; this is never a byte-copy of database/WAL files.
            source.backup(target, pages=256, sleep=0.05)
            target.commit()
            integrity, version = verify(target)
        finally:
            target.close()
            source.close()
        os.chmod(temporary, 0o600)
        fsync_path(temporary)
        os.replace(temporary, destination)
        fsync_path(output)

        pruned: list[str] = []
        if options.retain is not None:
            pattern = f"{database.stem}-*.sqlite3"
            backups = sorted((path for path in output.glob(pattern) if path.is_file()), key=lambda path: path.stat().st_mtime_ns, reverse=True)
            for old in backups[options.retain:]:
                old.unlink()
                pruned.append(str(old))
            if pruned:
                fsync_path(output)
        print(json.dumps({
            "dryRun": False, "backup": str(destination), "bytes": destination.stat().st_size,
            "schemaVersion": version, "integrity": integrity, "pruned": pruned,
        }))
        return 0
    except Exception:
        if os.path.lexists(temporary):
            temporary.unlink()
        raise


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"backup failed: {error}", file=sys.stderr)
        raise SystemExit(1)
