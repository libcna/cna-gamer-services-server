#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validate a CNA Gamer Services backup and restore it to a new database path."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import sqlite3
import sys

from backup import fsync_path, read_only_uri, verify


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backup", required=True, type=pathlib.Path)
    parser.add_argument("--database", required=True, type=pathlib.Path, help="new destination; existing paths are always refused")
    parser.add_argument("--create-directory", action="store_true", help="create a missing destination directory with mode 0700")
    options = parser.parse_args()

    backup = options.backup.resolve()
    destination = options.database.absolute()
    if not backup.is_file():
        parser.error(f"backup is not a regular file: {backup}")
    if os.path.lexists(destination):
        parser.error(f"destination already exists; move it aside explicitly before restore: {destination}")
    if not destination.parent.exists():
        if not options.create_directory:
            parser.error("destination directory does not exist (use --create-directory explicitly)")
        destination.parent.mkdir(parents=True, mode=0o700)
    temporary = destination.parent / ("." + destination.name + f".restore-{os.getpid()}")
    if os.path.lexists(temporary):
        raise RuntimeError(f"temporary restore path already exists: {temporary}")

    try:
        source = sqlite3.connect(read_only_uri(backup), uri=True, timeout=30)
        try:
            source_integrity, source_version = verify(source)
            target = sqlite3.connect(temporary)
            try:
                source.backup(target, pages=256, sleep=0.05)
                target.commit()
                restored_integrity, restored_version = verify(target)
            finally:
                target.close()
        finally:
            source.close()
        if restored_version != source_version:
            raise RuntimeError("restored schema version differs from the backup")
        os.chmod(temporary, 0o600)
        fsync_path(temporary)
        os.replace(temporary, destination)
        fsync_path(destination.parent)
        print(json.dumps({
            "backup": str(backup), "database": str(destination), "bytes": destination.stat().st_size,
            "schemaVersion": restored_version, "sourceIntegrity": source_integrity,
            "restoredIntegrity": restored_integrity,
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
        print(f"restore failed: {error}", file=sys.stderr)
        raise SystemExit(1)
