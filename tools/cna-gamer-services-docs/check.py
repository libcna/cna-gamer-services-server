#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Cheap documentation drift checks with no package-manager dependency."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import subprocess
import sys
import tempfile
import urllib.parse


LINK = re.compile(r"(?<!!)\[[^\]]*\]\(([^)]+)\)")
JSON_FENCE = re.compile(r"```json\s*\n(.*?)\n```", re.DOTALL | re.IGNORECASE)


def markdown_files(root: pathlib.Path) -> list[pathlib.Path]:
    ignored = {"build", "build-agent", "build-release", "build-asan", "build-tsan", ".git"}
    return sorted(path for path in root.rglob("*.md") if not any(part in ignored or part.startswith("build-") for part in path.parts))


def check_links(root: pathlib.Path, documents: list[pathlib.Path]) -> list[str]:
    failures: list[str] = []
    for document in documents:
        text = document.read_text(encoding="utf-8")
        for raw in LINK.findall(text):
            target = raw.strip().split(maxsplit=1)[0].strip("<>")
            if not target or target.startswith(("#", "http://", "https://", "mailto:")):
                continue
            path_text = urllib.parse.unquote(target.split("#", 1)[0])
            if "cna-gamer-services-docs/site/pages" in document.as_posix() and path_text.endswith(".html"):
                if path_text == "index.html":
                    continue
                generated_source = document.parent / (pathlib.Path(path_text).stem + ".md")
                if not generated_source.exists():
                    failures.append(f"{document.relative_to(root)}: missing generated-page source: {target}")
                continue
            resolved = (document.parent / path_text).resolve()
            try:
                resolved.relative_to(root)
            except ValueError:
                failures.append(f"{document.relative_to(root)}: link escapes repository: {target}")
                continue
            if not resolved.exists():
                failures.append(f"{document.relative_to(root)}: missing link target: {target}")
    return failures


def check_json_examples(root: pathlib.Path, documents: list[pathlib.Path]) -> list[str]:
    failures: list[str] = []
    for document in documents:
        for index, example in enumerate(JSON_FENCE.findall(document.read_text(encoding="utf-8")), 1):
            try:
                json.loads(example)
            except json.JSONDecodeError as error:
                failures.append(f"{document.relative_to(root)}: JSON fence {index}: {error.msg} at line {error.lineno}")
    return failures


def check_schema(root: pathlib.Path) -> list[str]:
    failures: list[str] = []
    store = (root / "include/CnaService/Store.hpp").read_text(encoding="utf-8")
    match = re.search(r"SchemaVersion\s*=\s*(\d+)", store)
    if not match:
        return ["include/CnaService/Store.hpp: SchemaVersion was not found"]
    declared = int(match.group(1))
    migrations = sorted((root / "migrations").glob("[0-9][0-9][0-9]_*.sql"))
    highest = max((int(path.name[:3]) for path in migrations), default=0)
    if declared != highest:
        failures.append(f"schema drift: Store declares {declared}, highest migration is {highest}")
    for migration in migrations:
        number = int(migration.name[:3])
        versions = re.findall(r"PRAGMA\s+user_version\s*=\s*(\d+)", migration.read_text(encoding="utf-8"), re.IGNORECASE)
        if versions != [str(number)]:
            failures.append(f"{migration.relative_to(root)}: expected exactly PRAGMA user_version={number}")
    readme = (root / "README.md").read_text(encoding="utf-8")
    if f"current schema ({declared})" not in readme:
        failures.append(f"README.md: current schema text does not name {declared}")
    return failures


def check_required_artifacts(root: pathlib.Path) -> list[str]:
    required = [
        "docs/ai-final-infrastructure-campaign.md",
        "docs/architecture.md",
        "docs/operations.md",
        "docs/security.md",
        "HANDOFF.md",
        "docs/protocol-implementation-map.md",
        "protocol/v1.md",
        "protocol/relay-v1.md",
        "protocol/golden/v1.json",
        "protocol/golden/relay-v1.json",
        "tools/qualify.sh",
        "tools/cna-gamer-services-conformance/README.md",
        "tools/cna-gamer-services-loadlab/README.md",
        "tools/cna-gamer-services-loadlab/loadlab.py",
        "tools/cna-gamer-services-chaos/README.md",
        "tools/cna-gamer-services-chaos/chaos.py",
        "tools/cna-gamer-services-admin-web/README.md",
        "tools/cna-gamer-services-admin-web/admin_web.py",
        "tools/cna-gamer-services-docs/site/README.md",
        "tools/cna-gamer-services-docs/site/build.py",
        "tools/cna-gamer-services-deploy/README.md",
        "tools/cna-gamer-services-deploy/backup.py",
        "tools/cna-gamer-services-deploy/restore.py",
        "tools/cna-gamer-services-deploy/systemd/cna-gamer-services.service",
        "tools/cna-gamer-services-deploy/container/Dockerfile",
        "tools/cna-gamer-services-deploy/container/compose.yaml",
        "tools/cna-gamer-services-deploy/nginx-stream.conf",
        ".dockerignore",
    ]
    return [f"missing required documentation artifact: {path}" for path in required if not (root / path).exists()]


def check_conformance_operations(root: pathlib.Path) -> list[str]:
    service = (root / "src/Service.cpp").read_text(encoding="utf-8")
    dispatcher_line = next((line for line in service.splitlines() if "operations{" in line), "")
    dispatched = set(re.findall(r'"([a-z][A-Za-z0-9_.-]+)"', dispatcher_line))
    conformance = (root / "tools/cna-gamer-services-conformance/conformance.py").read_text(encoding="utf-8")
    declaration = re.search(r'PUBLIC_OPERATIONS\s*=\s*frozenset\(\s*"""(.*?)"""\.split\(\)', conformance, re.DOTALL)
    if not dispatched:
        return ["src/Service.cpp: public operation dispatcher set was not found"]
    if not declaration:
        return ["conformance: PUBLIC_OPERATIONS declaration was not found"]
    covered = set(declaration.group(1).split())
    failures: list[str] = []
    if missing := sorted(dispatched - covered):
        failures.append("conformance: dispatched operations missing from coverage guard: " + ", ".join(missing))
    if stale := sorted(covered - dispatched):
        failures.append("conformance: stale operations in coverage guard: " + ", ".join(stale))
    return failures


def check_site(root: pathlib.Path) -> list[str]:
    failures: list[str] = []
    source = root / "tools/cna-gamer-services-docs/site/pages"
    pages = sorted(source.glob("*.md"))
    if len(pages) < 25:
        failures.append(f"documentation site: expected at least 25 teaching pages, found {len(pages)}")
    combined = "\n".join(path.read_text(encoding="utf-8") for path in pages).lower()
    required_topics = [
        "five-minute mental model", "administrator web", "refresh replay", "ranked arbitration",
        "host migration", "relay ticket", "binary frame", "database migration", "disaster runbook",
        "metric reference", "logging reference", "benchmark methodology", "loadlab", "conformance guide",
        "chaos guide", "troubleshooting", "maintainer workflow", "known limitations", "glossary",
        "learning path", "labs 13–17",
    ]
    for topic in required_topics:
        if topic not in combined:
            failures.append(f"documentation site: required topic marker is missing: {topic}")
    with tempfile.TemporaryDirectory(prefix="cna-docs-check-") as temporary:
        output = pathlib.Path(temporary)
        result = subprocess.run(
            [sys.executable, str(root / "tools/cna-gamer-services-docs/site/build.py"), "--output", str(output)],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        if result.returncode != 0:
            return failures + ["documentation site build failed: " + (result.stderr.strip() or result.stdout.strip())]
        html_files = sorted(output.glob("*.html"))
        if len(html_files) != len(pages) + 1:
            failures.append(f"documentation site: built {len(html_files)} HTML files for {len(pages)} pages")
        reference = re.compile(r'(?:href|src)="([^"]+)"')
        for document in html_files:
            for target in reference.findall(document.read_text(encoding="utf-8")):
                if target.startswith(("#", "http://", "https://", "mailto:")):
                    continue
                path_text = urllib.parse.unquote(target.split("#", 1)[0])
                if not (document.parent / path_text).exists():
                    failures.append(f"generated {document.name}: missing local target: {target}")
    listener = (root / "src/Listener.cpp").read_text(encoding="utf-8")
    exported = set(re.findall(r"cna_[a-z_]+", listener))
    exported = {re.sub(r"_(?:bucket|sum|count)$", "", name) for name in exported}
    observability = (root / "docs/operations.md").read_text(encoding="utf-8") + combined
    missing_metrics = sorted(name for name in exported if name not in observability)
    if missing_metrics:
        failures.append("documentation site: undocumented exported metrics: " + ", ".join(missing_metrics))
    admin_source = (root / "src/Admin.cpp").read_text(encoding="utf-8")
    commands = set(re.findall(r'command=="([^"]+)"', admin_source))
    admin_docs = (root / "README.md").read_text(encoding="utf-8") + combined
    missing_commands = sorted(command for command in commands if f"`{command}" not in admin_docs)
    if missing_commands:
        failures.append("documentation site: undocumented admin commands: " + ", ".join(missing_commands))
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", nargs="?", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[2])
    parser.add_argument("--strict", action="store_true", help="reserved for checks that may initially be warnings")
    options = parser.parse_args()
    root = options.root.resolve()
    documents = markdown_files(root)
    failures = check_links(root, documents)
    failures += check_json_examples(root, documents)
    failures += check_schema(root)
    failures += check_required_artifacts(root)
    failures += check_conformance_operations(root)
    failures += check_site(root)
    if failures:
        for failure in failures:
            print("FAIL", failure)
        print(f"Documentation checks: {len(failures)} failure(s), {len(documents)} Markdown files inspected")
        return 1
    print(f"Documentation checks passed: {len(documents)} Markdown files, links, JSON examples and schema references")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
