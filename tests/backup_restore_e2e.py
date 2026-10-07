# SPDX-License-Identifier: MIT
"""Online backup and restore qualification using only disposable state."""

import json
import os
import pathlib
import socket
import ssl
import subprocess
import sys
import tempfile

from transport_e2e import request


def check(value, reason):
    if not value:
        raise AssertionError(reason)


def start(build, database, certificate, key):
    process = subprocess.Popen(
        [str(build / "cna-gamer-services-server"), "--database", str(database), "--listen", "127.0.0.1",
         "--port", "0", "--cert", str(certificate), "--key", str(key)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    line = process.stdout.readline().strip()
    check("listening" in line, "server startup: " + (line or process.stderr.read()))
    return process, "https://localhost:" + line.rsplit(":", 1)[1] + "/cna/v1"


def stop(process):
    process.terminate()
    process.wait(timeout=10)
    process.stdout.close()
    process.stderr.close()


def main():
    build = pathlib.Path(sys.argv[1]).resolve()
    root_repo = pathlib.Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="cna-backup-restore-") as temporary:
        root = pathlib.Path(temporary)
        os.chmod(root, 0o700)
        database = root / "live.sqlite3"
        certificate, key = root / "certificate.pem", root / "private-key.pem"
        generated = subprocess.run(
            ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
             "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost",
             "-keyout", str(key), "-out", str(certificate)],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
        )
        check(generated.returncode == 0, "certificate generation")
        admin = build / "cna-gamer-services-admin"

        def command(*arguments, stdin=None):
            result = subprocess.run([str(admin), str(database), *arguments], input=stdin, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            check(result.returncode == 0, "admin " + " ".join(arguments) + ": " + result.stderr)

        command("title", "backup", "Backup qualification")
        command("user", "alice", "Alice", stdin="alice-backup-password\n")
        command("user", "bob", "Bob", stdin="bob-backup-password\n")
        command("achievement", "backup", stdin=json.dumps({
            "key": "durable", "name": "Durable", "description": "survives restore",
            "howToEarn": "back up", "score": 25,
        }))
        command("leaderboard", "backup", stdin=json.dumps({
            "key": "Score", "mode": 0, "ascending": False, "aggregation": "best",
            "arbitrated": False, "columns": {"Rounds": "int32"},
        }))
        command("seed-leaderboard", "backup", stdin=json.dumps({
            "key": "Score", "mode": 0, "gamertag": "Alice", "rating": 42,
            "columns": {"Rounds": {"type": "int32", "value": 3}},
        }))

        process, url = start(build, database, certificate, key)
        try:
            alice = request(url, str(certificate), "backup", "auth.login", {"username": "alice", "password": "alice-backup-password"})["result"]
            bob = request(url, str(certificate), "backup", "auth.login", {"username": "bob", "password": "bob-backup-password"})["result"]

            def call(operation, arguments, token):
                result = request(url, str(certificate), "backup", operation, arguments, token)
                check(result["error"] == "OK", operation + ": " + result["error"])
                return result["result"]

            call("friends.add", {"gamertag": "Bob"}, alice["token"])
            call("friends.accept", {"gamertag": "Alice"}, bob["token"])
            call("achievements.award", {"key": "durable"}, alice["token"])
            call("profile.setGamerZone", {"gamerZone": "pro"}, alice["token"])
            call("profile.setGameDefaults", {"gameDefaults": {"gameDifficulty": "Hard", "invertYAxis": True}}, alice["token"])
            call("messages.send", {"gamertags": ["Alice"], "text": "restore me"}, bob["token"])

            backup_result = subprocess.run(
                [sys.executable, str(root_repo / "tools/cna-gamer-services-deploy/backup.py"),
                 "--database", str(database), "--output-dir", str(root / "backups")],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            )
            check(backup_result.returncode == 0, "online backup: " + backup_result.stderr)
            backup = pathlib.Path(json.loads(backup_result.stdout)["backup"])
            check(backup.is_file(), "backup was not published")
        finally:
            stop(process)

        restored = root / "restored.sqlite3"
        restore_result = subprocess.run(
            [sys.executable, str(root_repo / "tools/cna-gamer-services-deploy/restore.py"),
             "--backup", str(backup), "--database", str(restored)],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        check(restore_result.returncode == 0, "restore: " + restore_result.stderr)
        restored_report = json.loads(restore_result.stdout)
        check(restored_report["schemaVersion"] == 23 and restored_report["restoredIntegrity"] == "ok", "restore validation")

        process, url = start(build, restored, certificate, key)
        try:
            def restored_call(operation, arguments, token):
                return request(url, str(certificate), "backup", operation, arguments, token)

            check(restored_call("auth.ping", {}, alice["token"])["error"] == "OK", "access credential survived")
            profile = restored_call("profile.get", {"gamertag": "Alice"}, alice["token"])["result"]
            check(profile["gamerZone"] == "pro" and profile["gamerScore"] == 25, "profile/achievement survived")
            defaults = restored_call("profile.gameDefaults", {}, alice["token"])["result"]["gameDefaults"]
            check(defaults == {"gameDifficulty": "Hard", "invertYAxis": True}, "game defaults survived")
            friends = restored_call("friends.list", {}, alice["token"])["result"]["friends"]
            check(len(friends) == 1 and friends[0]["accepted"], "friendship survived")
            messages = restored_call("messages.list", {"start": 0, "limit": 10}, alice["token"])["result"]
            check(messages["total"] == 1 and messages["messages"][0]["text"] == "restore me", "message survived")
            board = restored_call("leaderboards.read", {"key": "Score", "mode": 0, "start": 0, "size": 10}, alice["token"])["result"]
            check(board["total"] == 1 and board["entries"][0]["rating"] == 42, "leaderboard survived")
            rotated = restored_call("auth.refresh", {"refreshToken": alice["refreshToken"]}, None)
            check(rotated["error"] == "OK", "refresh credential survived")
        finally:
            stop(process)
        print("Online backup/restore preserved credentials, profile, achievement, friendship, message and leaderboard state")


if __name__ == "__main__":
    main()
