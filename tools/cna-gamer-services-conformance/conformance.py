#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Black-box CNA Gamer Services v1 conformance runner.

The runner provisions only scratch state through the public administrator executable, launches the
real TLS server, and then uses HTTPS/WSS like an independent client. It never imports server code or
opens the SQLite database. Results are both human-readable and machine-readable.
"""

from __future__ import annotations

import argparse
import asyncio
import datetime as dt
import http.client
import json
import os
import pathlib
import platform
import ssl
import subprocess
import sys
import tempfile
import time
import uuid

import websockets


TOOL_VERSION = 1
TITLE = "conformance"
PASSWORDS = {
    "alice": "alice-conformance-password",
    "bob": "bob-conformance-password",
    "carol": "carol-conformance-password",
    "dave": "dave-conformance-password",
}
PUBLIC_OPERATIONS = frozenset(
    """hello auth.login auth.logout auth.refresh auth.ping gamer.lookup profile.get
    profile.gameDefaults profile.setGameDefaults profile.setGamerZone friends.list friends.add
    friends.remove friends.accept presence.set presence.status achievements.list achievements.award
    assets.read leaderboards.read leaderboards.definition leaderboards.list leaderboards.game.begin
    leaderboards.game.commit leaderboards.game.abort sessions.relayTicket sessions.create sessions.find
    sessions.get sessions.touch sessions.update sessions.join sessions.joinInvited sessions.leave
    sessions.remove sessions.addMembers invites.send invites.list invites.get invites.accept
    invites.dismiss invites.joinFriend parties.get parties.invite parties.accept parties.decline
    parties.leave messages.send messages.list messages.read messages.delete reviews.submit avatars.get
    avatars.set avatars.catalog avatars.catalogPack privacy.block privacy.unblock privacy.list""".split()
)


class Failure(RuntimeError):
    """One stable, concise conformance assertion failure."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)


class ManagedServer:
    """Scratch database, certificate and real server process."""

    def __init__(self, build: pathlib.Path, keep: bool) -> None:
        self.build = build.resolve()
        self.keep = keep
        self.temp: tempfile.TemporaryDirectory[str] | None = None
        self.root: pathlib.Path | None = None
        self.database: pathlib.Path | None = None
        self.certificate: pathlib.Path | None = None
        self.key: pathlib.Path | None = None
        self.asset: bytes = b""
        self.asset_hash = ""
        self.process: subprocess.Popen[str] | None = None
        self.url = ""

    @property
    def admin(self) -> pathlib.Path:
        return self.build / "cna-gamer-services-admin"

    @property
    def executable(self) -> pathlib.Path:
        return self.build / "cna-gamer-services-server"

    def command(self, *arguments: str, stdin: str | None = None, capture: bool = False) -> str:
        require(self.database is not None, "scratch database was not initialized")
        result = subprocess.run(
            [str(self.admin), str(self.database), *arguments],
            input=stdin,
            text=True,
            check=False,
            stdout=subprocess.PIPE if capture else subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        require(result.returncode == 0, f"admin {' '.join(arguments[:2])} failed: {result.stderr.strip()}")
        return result.stdout.strip() if capture else ""

    def start(self) -> None:
        require(self.admin.is_file() and self.executable.is_file(), "build does not contain server and admin executables")
        if self.keep:
            self.root = pathlib.Path(tempfile.mkdtemp(prefix="cna-conformance-"))
        else:
            self.temp = tempfile.TemporaryDirectory(prefix="cna-conformance-")
            self.root = pathlib.Path(self.temp.name)
        os.chmod(self.root, 0o700)
        self.database = self.root / "state.sqlite3"
        self.certificate = self.root / "certificate.pem"
        self.key = self.root / "private-key.pem"
        certificate = subprocess.run(
            [
                "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost",
                "-keyout", str(self.key), "-out", str(self.certificate),
            ],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )
        require(certificate.returncode == 0, f"scratch certificate generation failed: {certificate.stderr.strip()}")

        self.command("title", TITLE, "Conformance scratch title")
        self.command("title", "other-title", "Isolation scratch title")
        for username, password in PASSWORDS.items():
            self.command("user", username, username.title(), stdin=password + "\n")
        achievement = {
            "key": "first-step", "name": "First step", "description": "Conformance fixture",
            "howToEarn": "Run the suite", "score": 10,
        }
        self.command("achievement", TITLE, stdin=json.dumps(achievement))
        board = {
            "key": "Score", "mode": 0, "ascending": False, "aggregation": "best",
            "arbitrated": False, "columns": {"Rounds": "int32"},
        }
        self.command("leaderboard", TITLE, stdin=json.dumps(board))
        ranked_board = {
            "key": "RankedScore", "mode": 0, "ascending": False, "aggregation": "latest",
            "arbitrated": True, "columns": {},
        }
        self.command("leaderboard", TITLE, stdin=json.dumps(ranked_board))
        seeded = {
            "key": "Score", "mode": 0, "gamertag": "Alice", "rating": 10,
            "columns": {"Rounds": {"type": "int32", "value": 1}},
        }
        self.command("seed-leaderboard", TITLE, stdin=json.dumps(seeded))

        # A complete one-pixel PNG. The file route must return byte-for-byte content.
        self.asset = bytes.fromhex(
            "89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c489"
            "0000000d49444154789c6360f8cff0000004010100c9fe92ef0000000049454e44ae426082"
        )
        asset_path = self.root / "pixel.png"
        asset_path.write_bytes(self.asset)
        self.asset_hash = self.command("asset", TITLE, "image/png", str(asset_path), capture=True)

        self.process = subprocess.Popen(
            [
                str(self.executable), "--database", str(self.database), "--listen", "127.0.0.1",
                "--port", "0", "--cert", str(self.certificate), "--key", str(self.key),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        require(self.process.stdout is not None, "server stdout was not captured")
        line = self.process.stdout.readline().strip()
        if "listening" not in line:
            error = self.process.stderr.read().strip() if self.process.stderr else ""
            raise Failure(f"server did not start: {line or error or 'no diagnostic'}")
        port = int(line.rsplit(":", 1)[1])
        self.url = f"https://localhost:{port}/cna/v1"

    def stop(self) -> None:
        if self.process is not None:
            if self.process.poll() is None:
                self.process.terminate()
                try:
                    self.process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait(timeout=5)
            if self.process.stdout:
                self.process.stdout.close()
            if self.process.stderr:
                self.process.stderr.close()
        if self.temp is not None:
            self.temp.cleanup()


class Client:
    """Small public-protocol client with deterministic request correlation."""

    def __init__(self, server: ManagedServer) -> None:
        require(server.certificate is not None, "certificate is unavailable")
        self.server = server
        self.context = ssl.create_default_context(cafile=str(server.certificate))
        self.counter = 0
        self.prefix = "conformance-" + uuid.uuid4().hex[:12]
        self.operations: set[str] = set()

    def request_id(self) -> str:
        self.counter += 1
        return f"{self.prefix}-{self.counter}"

    def raw(self, body: str) -> tuple[int, dict]:
        port = int(self.server.url.split(":")[2].split("/")[0])
        connection = http.client.HTTPSConnection("localhost", port, context=self.context, timeout=10)
        try:
            connection.request("POST", "/cna/v1", body=body.encode("utf-8"), headers={"Content-Type": "application/json"})
            response = connection.getresponse()
            payload = response.read()
            return response.status, json.loads(payload)
        finally:
            connection.close()

    def call(
        self,
        operation: str,
        arguments: dict | None = None,
        token: str | None = None,
        *,
        request_id: str | None = None,
        game: str = TITLE,
        version: int = 1,
    ) -> dict:
        self.operations.add(operation)
        envelope = {
            "v": version,
            "id": request_id or self.request_id(),
            "game": game,
            "op": operation,
            "args": arguments or {},
        }
        if token is not None:
            envelope["token"] = token
        status, result = self.raw(json.dumps(envelope, separators=(",", ":")))
        require(status == 200, f"{operation} returned HTTP {status}")
        require(result.get("id") == envelope["id"], f"{operation} did not preserve the request ID")
        return result

    def expect(self, operation: str, error: str, arguments: dict | None = None, token: str | None = None, **kwargs) -> dict:
        result = self.call(operation, arguments, token, **kwargs)
        require(result.get("error") == error, f"{operation}: expected {error}, got {result.get('error')}")
        return result

    def file(self, token: str, digest: str) -> tuple[int, str, bytes]:
        port = int(self.server.url.split(":")[2].split("/")[0])
        connection = http.client.HTTPSConnection("localhost", port, context=self.context, timeout=10)
        try:
            connection.request(
                "GET", f"/cna/v1/files/{digest}",
                headers={"Authorization": f"Bearer {token}", "X-CNA-Game": TITLE},
            )
            response = connection.getresponse()
            return response.status, response.getheader("Content-Type", ""), response.read()
        finally:
            connection.close()


class Suite:
    def __init__(self, server: ManagedServer, include_slow: bool) -> None:
        self.server = server
        self.include_slow = include_slow
        self.client = Client(server)
        self.credentials: dict[str, dict] = {}
        self.session = ""
        self.host_machine = ""
        self.remote_machine = ""
        self.cases: list[dict] = []

    def record(self, name: str, function) -> None:
        started = time.perf_counter()
        try:
            function()
            status, detail = "passed", ""
        except Exception as error:  # each case becomes a compact machine-readable failure
            status, detail = "failed", f"{type(error).__name__}: {error}"
        self.cases.append({"name": name, "status": status, "durationMs": round((time.perf_counter() - started) * 1000, 2), "detail": detail})

    def hello(self) -> None:
        result = self.client.expect("hello", "OK")["result"]
        required = {
            "identity", "authentication", "session-refresh", "heartbeat", "friends", "presence",
            "achievements", "assets", "files", "leaderboard-reads", "local-leaderboard-commit",
            "session-directory", "session-invitations", "host-migration", "parties", "privacy",
            "events", "relay-tickets", "relay", "request-outcomes",
        }
        require(result.get("version") == 1, "hello did not select v1")
        require(result.get("maxMessageBytes") == 65536, "hello message bound drifted")
        missing = sorted(required - set(result.get("capabilities", [])))
        require(not missing, "hello is missing capabilities: " + ", ".join(missing))

    def envelope_validation(self) -> None:
        malformed = ["{", "[]", '{"v":1,"v":1}', '{"v":1,"id":"x","game":"conformance","op":"hello","args":{},"extra":1}']
        for body in malformed:
            status, result = self.client.raw(body)
            require(status == 200 and result.get("error") == "MALFORMED_MESSAGE", f"malformed envelope was accepted: {body[:32]}")
        status, result = self.client.raw(
            json.dumps({"v": 2, "id": "future-version", "game": TITLE, "op": "hello", "args": {}})
        )
        require(status == 200 and result.get("error") == "UNSUPPORTED_VERSION", "future protocol version was not refused")
        status, result = self.client.raw('{"v":1,"id":"../bad","game":"conformance","op":"hello","args":{}}')
        require(status == 200 and result.get("error") == "INVALID_ARGUMENT", "invalid identifier was accepted")

    def authentication(self) -> None:
        self.client.expect("auth.login", "AUTHENTICATION_FAILED", {"username": "alice", "password": "wrong-password"})
        self.client.expect("profile.get", "UNAUTHENTICATED", {"gamertag": "Alice"}, "0" * 64)
        for username, password in PASSWORDS.items():
            login = self.client.expect("auth.login", "OK", {"username": username, "password": password})["result"]
            require(len(login.get("token", "")) == 64 and len(login.get("refreshToken", "")) == 64, "credential shape drifted")
            self.credentials[username] = login
        ping = self.client.expect("auth.ping", "OK", {}, self.credentials["alice"]["token"])["result"]
        require("serverTime" in ping and "privileges" in ping and "blocked" in ping, "heartbeat policy snapshot is incomplete")
        self.client.expect("auth.ping", "UNAUTHENTICATED", {}, self.credentials["alice"]["token"], game="other-title")

    def identity_and_preferences(self) -> None:
        alice = self.credentials["alice"]
        bob = self.credentials["bob"]
        lookup = self.client.expect(
            "gamer.lookup", "OK", {"gamertag": "Alice"}, bob["token"]
        )["result"]
        require(lookup["userId"] == alice["identity"]["userId"], "gamer lookup changed identity")
        self.client.expect("gamer.lookup", "NOT_FOUND", {"gamertag": "Missing"}, alice["token"])

        profile = self.client.expect(
            "profile.setGamerZone", "OK", {"gamerZone": "pro"}, alice["token"]
        )["result"]
        require(profile["gamerZone"] == "pro", "gamer zone did not persist")
        defaults = {"gameDifficulty": "Hard", "invertYAxis": True, "primaryColor": "#123abc"}
        written = self.client.expect(
            "profile.setGameDefaults", "OK", {"gameDefaults": defaults}, alice["token"]
        )["result"]
        require(written["gameDefaults"] == defaults, "game defaults write changed values")
        read = self.client.expect("profile.gameDefaults", "OK", {}, alice["token"])["result"]
        require(read["gameDefaults"] == defaults, "game defaults read did not round-trip")
        public = self.client.expect(
            "profile.get", "OK", {"gamertag": "Alice"}, bob["token"]
        )["result"]
        require(public["gamerZone"] == "pro", "profile omitted the selected gamer zone")

    def resources_and_progress(self) -> None:
        token = self.credentials["alice"]["token"]
        catalog = self.client.expect("achievements.list", "OK", {}, token)["result"]["achievements"]
        require(len(catalog) == 1 and catalog[0]["key"] == "first-step", "achievement catalog mismatch")
        request_id = self.client.request_id()
        award = self.client.expect("achievements.award", "OK", {"key": "first-step"}, token, request_id=request_id)
        require(award["result"]["awarded"] is True, "first award was not new")
        replay = self.client.expect("achievements.award", "OK", {"key": "first-step"}, token, request_id=request_id)
        require(replay == award, "request-outcome replay did not preserve the award response")
        later = self.client.expect("achievements.award", "OK", {"key": "first-step"}, token)
        require(later["result"]["awarded"] is False, "new award request was not idempotent")

        boards = self.client.expect("leaderboards.list", "OK", {}, token)["result"]["boards"]
        require({board["key"] for board in boards} == {"Score", "RankedScore"}, "leaderboard catalog mismatch")
        definition = self.client.expect("leaderboards.definition", "OK", {"key": "Score", "mode": 0}, token)["result"]
        require(definition["columns"] == {"Rounds": "int32"}, "leaderboard definition mismatch")
        page = self.client.expect(
            "leaderboards.read", "OK", {"key": "Score", "mode": 0, "start": 0, "size": 10}, token
        )["result"]
        require(page["total"] == 1 and page["entries"][0]["gamertag"] == "Alice", "leaderboard read mismatch")

        chunk = self.client.expect(
            "assets.read", "OK", {"hash": self.server.asset_hash, "offset": 0, "length": 16}, token
        )["result"]
        require(bytes.fromhex(chunk["hex"]) == self.server.asset[:16], "asset chunk bytes changed")
        status, content_type, body = self.client.file(token, self.server.asset_hash)
        require(status == 200 and content_type == "image/png" and body == self.server.asset, "raw file route mismatch")

    def local_leaderboard_commit(self) -> None:
        alice = self.credentials["alice"]
        bob = self.credentials["bob"]
        begun = self.client.expect(
            "leaderboards.game.begin", "OK",
            {"kind": "local", "participants": [alice["token"], bob["token"]]}, alice["token"],
        )["result"]
        entry = {
            "userId": bob["identity"]["userId"], "key": "Score", "mode": 0, "rating": 25,
            "columns": {"Rounds": {"type": "int32", "value": 2}},
        }
        self.client.expect(
            "leaderboards.game.commit", "NOT_AUTHORIZED",
            {"gameplay": begun["gameplay"], "entries": [entry]}, bob["token"],
        )
        self.client.expect(
            "leaderboards.game.commit", "OK",
            {"gameplay": begun["gameplay"], "entries": [entry]}, alice["token"],
        )
        page = self.client.expect(
            "leaderboards.read", "OK",
            {"key": "Score", "mode": 0, "start": 0, "size": 10, "gamers": ["Bob"]},
            alice["token"],
        )["result"]
        require(page["total"] == 1 and page["entries"][0]["rating"] == 25, "committed score was not readable")

        abandoned = self.client.expect(
            "leaderboards.game.begin", "OK", {"kind": "local", "participants": [alice["token"]]},
            alice["token"],
        )["result"]["gameplay"]
        self.client.expect("leaderboards.game.abort", "OK", {"gameplay": abandoned}, alice["token"])
        self.client.expect("leaderboards.game.abort", "OK", {"gameplay": abandoned}, alice["token"])

    def refresh_rotation(self) -> None:
        original = self.credentials["carol"]
        rotated = self.client.expect("auth.refresh", "OK", {"refreshToken": original["refreshToken"]})["result"]
        self.client.expect("auth.ping", "UNAUTHENTICATED", {}, original["token"])
        self.client.expect("auth.ping", "OK", {}, rotated["token"])
        self.client.expect("auth.refresh", "UNAUTHENTICATED", {"refreshToken": original["refreshToken"]})
        self.client.expect("auth.ping", "UNAUTHENTICATED", {}, rotated["token"])

    def social_state(self) -> None:
        alice, bob = self.credentials["alice"]["token"], self.credentials["bob"]["token"]
        self.client.expect("friends.add", "OK", {"gamertag": "Bob"}, alice)
        incoming = self.client.expect("friends.list", "OK", {}, bob)["result"]["friends"]
        require(len(incoming) == 1 and incoming[0]["requestReceived"], "friend request was not visible")
        self.client.expect("friends.accept", "OK", {"gamertag": "Alice"}, bob)
        self.client.expect("presence.set", "OK", {"mode": 7, "text": "Conformance"}, alice)
        self.client.expect("presence.status", "OK", {"status": "busy"}, alice)
        friends = self.client.expect("friends.list", "OK", {}, bob)["result"]["friends"]
        alice_row = next(row for row in friends if row["gamertag"] == "Alice")
        require(alice_row["accepted"] and alice_row["online"] and alice_row["presenceText"] == "Conformance" and alice_row["busy"], "accepted friend presence mismatch")

        message_id = self.client.request_id()
        sent = self.client.expect("messages.send", "OK", {"gamertags": ["Alice"], "text": "hello"}, bob, request_id=message_id)
        replay = self.client.expect("messages.send", "OK", {"gamertags": ["Alice"], "text": "hello"}, bob, request_id=message_id)
        require(sent == replay, "message request replay response changed")
        inbox = self.client.expect("messages.list", "OK", {"start": 0, "limit": 10}, alice)["result"]
        require(inbox["total"] == 1 and inbox["messages"][0]["text"] == "hello", "message replay delivered twice")
        message = inbox["messages"][0]["message"]
        self.client.expect("messages.read", "OK", {"message": message}, alice)
        reread = self.client.expect("messages.list", "OK", {"start": 0, "limit": 10}, alice)["result"]
        require(reread["unread"] == 0 and reread["messages"][0]["read"], "message was not marked read")
        self.client.expect("messages.delete", "OK", {"message": message}, alice)
        require(
            self.client.expect("messages.list", "OK", {"start": 0, "limit": 10}, alice)["result"]["total"] == 0,
            "message was not deleted",
        )

        self.client.expect("reviews.submit", "OK", {"gamertag": "Bob", "rating": "prefer"}, alice)
        bob_profile = self.client.expect("profile.get", "OK", {"gamertag": "Bob"}, alice)["result"]
        require(bob_profile["reputation"] == 5.0, "preferred player reputation did not update")
        self.client.expect("reviews.submit", "OK", {"gamertag": "Bob", "rating": "clear"}, alice)

    def boundaries(self) -> None:
        alice = self.credentials["alice"]["token"]
        self.client.expect("presence.set", "INVALID_ARGUMENT", {"mode": 0, "text": "x" * 257}, alice)
        self.client.expect("messages.send", "INVALID_ARGUMENT", {"gamertags": ["Bob", "Bob"], "text": "x"}, alice)
        self.client.expect(
            "sessions.find", "INVALID_ARGUMENT",
            {"kind": "player", "localCount": 1, "properties": [None] * 7, "start": 0, "limit": 1}, alice,
        )
        self.client.expect("assets.read", "INVALID_ARGUMENT", {"hash": "A" * 64, "offset": 0, "length": 1}, alice)
        self.client.expect("messages.list", "INVALID_ARGUMENT", {"start": 0, "limit": 33}, alice)
        self.client.expect("avatars.get", "INVALID_ARGUMENT", {"userIds": []}, alice)
        self.client.expect("operation.that.does.not.exist", "UNKNOWN_OPERATION", {}, alice)

    def avatar_surface(self) -> None:
        alice = self.credentials["alice"]
        bob = self.credentials["bob"]
        rows = self.client.expect(
            "avatars.get", "OK",
            {"userIds": [alice["identity"]["userId"], bob["identity"]["userId"]]}, alice["token"],
        )["result"]["avatars"]
        require(
            len(rows) == 2 and all(row["description"] is None and row["revision"] == 0 for row in rows),
            "accounts without avatars did not return the documented null representation",
        )
        self.client.expect("avatars.catalog", "NOT_FOUND", {}, alice["token"])
        self.client.expect("avatars.catalogPack", "NOT_FOUND", {"version": 1}, alice["token"])
        self.client.expect("avatars.set", "INVALID_ARGUMENT", {"description": "00" * 1021}, alice["token"])

    def session_and_invitation(self) -> None:
        alice, bob = self.credentials["alice"]["token"], self.credentials["bob"]["token"]
        created = self.client.expect(
            "sessions.create", "OK",
            {
                "kind": "player", "maxGamers": 4, "privateSlots": 1, "properties": [None] * 8,
                "allowJoinInProgress": True, "allowHostMigration": True, "participants": [alice],
            }, alice,
        )["result"]
        self.session, self.host_machine = created["session"], created["hostMachine"]
        invitation = self.client.expect("invites.send", "OK", {"session": self.session, "gamertag": "Bob"}, alice)["result"]
        listed = self.client.expect("invites.list", "OK", {"start": 0, "limit": 8}, bob)["result"]["invites"]
        require(len(listed) == 1 and listed[0]["invite"] == invitation["invite"], "invitation inbox mismatch")
        self.client.expect("invites.accept", "OK", {"invite": invitation["invite"]}, bob)
        joined = self.client.expect(
            "sessions.joinInvited", "OK",
            {"session": self.session, "invite": invitation["invite"], "participants": [bob]}, bob,
        )["result"]
        self.remote_machine = joined["machine"]
        require(self.remote_machine != self.host_machine and len(joined["members"]) == 2, "session membership mismatch")
        touched = self.client.expect("sessions.touch", "OK", {"session": self.session}, bob)["result"]
        require(touched["revision"] >= 2, "session revision did not advance")

    async def _relay(self) -> None:
        alice, bob = self.credentials["alice"]["token"], self.credentials["bob"]["token"]
        endpoint = self.server.url.replace("https:", "wss:").replace("/cna/v1", "/cna/relay/v1")
        context = self.client.context

        def ticket(owner: str) -> str:
            return self.client.expect(
                "sessions.relayTicket", "OK", {"session": self.session, "participants": [owner]}, owner
            )["result"]["ticket"]

        async def connect(secret: str, request_id: str):
            socket = await websockets.connect(
                endpoint, ssl=context, compression=None, proxy=None, ping_interval=None, close_timeout=2, max_queue=2
            )
            await socket.send(json.dumps({"v": 1, "id": request_id, "game": TITLE, "ticket": secret}))
            welcome = json.loads(await asyncio.wait_for(socket.recv(), 5))
            require(welcome["error"] == "OK" and welcome["id"] == request_id, "relay welcome mismatch")
            return socket

        def frame(machine: str, payload: bytes) -> bytes:
            return b"CNR\x01\x01\x00\x00\x00" + bytes.fromhex(machine) + payload

        alice_ticket = ticket(alice)
        left = await connect(alice_ticket, "relay-alice")
        replay = await websockets.connect(endpoint, ssl=context, compression=None, proxy=None, ping_interval=None, close_timeout=2)
        await replay.send(json.dumps({"v": 1, "id": "replay", "game": TITLE, "ticket": alice_ticket}))
        try:
            await asyncio.wait_for(replay.recv(), 5)
            raise Failure("one-use relay ticket was accepted twice")
        except websockets.ConnectionClosed as closed:
            require(closed.rcvd is not None and closed.rcvd.code == 1008, "relay replay used the wrong close policy")
        finally:
            await replay.close()

        right = await connect(ticket(bob), "relay-bob")
        try:
            payload = b"conformance-datagram"
            await left.send(frame(self.remote_machine, payload))
            require(await asyncio.wait_for(right.recv(), 5) == frame(self.host_machine, payload), "relay did not inject the authenticated source")
            await left.send(frame("11" * 16, b"drop"))
            try:
                await asyncio.wait_for(right.recv(), 0.15)
                raise Failure("unknown relay destination was not dropped")
            except asyncio.TimeoutError:
                pass
        finally:
            await left.close()
            await right.close()
        await asyncio.sleep(0.05)

        malformed = await connect(ticket(alice), "relay-malformed")
        await malformed.send(b"bad")
        try:
            await asyncio.wait_for(malformed.recv(), 5)
            raise Failure("truncated relay frame remained connected")
        except websockets.ConnectionClosed as closed:
            require(closed.rcvd is not None and closed.rcvd.code == 1002, "malformed relay frame used the wrong close policy")
        finally:
            await malformed.close()

    def relay(self) -> None:
        asyncio.run(self._relay())

    async def _relay_ticket_expiry(self) -> None:
        alice = self.credentials["alice"]["token"]
        bob = self.credentials["bob"]["token"]
        # Keep the directory lease alive beyond the ticket's shorter lifetime so rejection proves
        # ticket expiry, not loss of session membership.
        self.client.expect("sessions.touch", "OK", {"session": self.session}, alice)
        self.client.expect("sessions.touch", "OK", {"session": self.session}, bob)
        issued = self.client.expect(
            "sessions.relayTicket", "OK", {"session": self.session, "participants": [alice]}, alice
        )["result"]
        wait_seconds = issued["expires"] - issued["serverTime"] + 1.1
        require(1 <= wait_seconds <= 65, "relay ticket lifetime left the documented bound")
        await asyncio.sleep(wait_seconds)

        endpoint = self.server.url.replace("https:", "wss:").replace("/cna/v1", "/cna/relay/v1")
        channel = await websockets.connect(
            endpoint, ssl=self.client.context, compression=None, proxy=None,
            ping_interval=None, close_timeout=2,
        )
        await channel.send(json.dumps({
            "v": 1, "id": "expired-ticket", "game": TITLE, "ticket": issued["ticket"],
        }))
        try:
            await asyncio.wait_for(channel.recv(), 5)
            raise Failure("expired relay ticket was accepted")
        except websockets.ConnectionClosed as closed:
            require(
                closed.rcvd is not None and closed.rcvd.code == 1008,
                "expired relay ticket used the wrong close policy",
            )
        finally:
            await channel.close()

    def relay_ticket_expiry(self) -> None:
        asyncio.run(self._relay_ticket_expiry())

    async def _events(self) -> None:
        endpoint = self.server.url.replace("https:", "wss:") + "/events"
        alice, bob = self.credentials["alice"]["token"], self.credentials["bob"]["token"]
        channel = await websockets.connect(endpoint, ssl=self.client.context, compression=None, proxy=None, ping_interval=None, close_timeout=2)
        await channel.send(json.dumps({"v": 1, "id": "events", "game": TITLE, "token": alice}))
        welcome = json.loads(await asyncio.wait_for(channel.recv(), 5))
        require(welcome["error"] == "OK" and "friends" in welcome["result"]["topics"], "event welcome mismatch")
        self.client.expect("friends.remove", "OK", {"gamertag": "Alice"}, bob)
        hint = json.loads(await asyncio.wait_for(channel.recv(), 5))
        require(hint == {"v": 1, "topics": ["friends"]}, "event hint mismatch")
        await channel.close()

        unexpected = await websockets.connect(endpoint, ssl=self.client.context, compression=None, proxy=None, ping_interval=None, close_timeout=2)
        await unexpected.send(json.dumps({"v": 1, "id": "events", "game": TITLE, "token": alice, "extra": True}))
        try:
            await asyncio.wait_for(unexpected.recv(), 5)
            raise Failure("event hello accepted an unexpected field")
        except websockets.ConnectionClosed:
            pass
        finally:
            await unexpected.close()

    def events(self) -> None:
        asyncio.run(self._events())

    def host_migration_and_privacy(self) -> None:
        alice, bob = self.credentials["alice"]["token"], self.credentials["bob"]["token"]
        left = self.client.expect("sessions.leave", "OK", {"session": self.session}, alice)["result"]
        require(left["ended"] is False, "migration-enabled host departure ended the session")
        snapshot = self.client.expect("sessions.get", "OK", {"session": self.session}, bob)["result"]
        require(snapshot["hostMachine"] == self.remote_machine, "replacement host was not selected")
        require(self.client.expect("sessions.leave", "OK", {"session": self.session}, bob)["result"]["ended"] is True, "last host did not end the session")

        self.client.expect("privacy.block", "OK", {"gamertag": "Bob"}, alice)
        blocked = self.client.expect("privacy.list", "OK", {}, alice)["result"]["blocked"]
        require(blocked == ["Bob"], "block list mismatch")
        self.client.expect("messages.send", "NOT_AUTHORIZED", {"gamertags": ["Alice"], "text": "blocked"}, bob)
        self.client.expect("privacy.unblock", "OK", {"gamertag": "Bob"}, alice)

    def directory_mutation_and_removal(self) -> None:
        alice = self.credentials["alice"]["token"]
        bob = self.credentials["bob"]["token"]
        dave = self.credentials["dave"]["token"]
        properties = [42, None, None, None, None, None, None, None]
        wildcard = [None] * 8
        created = self.client.expect(
            "sessions.create", "OK",
            {
                "kind": "player", "maxGamers": 5, "privateSlots": 0, "properties": properties,
                "allowJoinInProgress": False, "allowHostMigration": False, "participants": [alice],
            }, alice,
        )["result"]
        session = created["session"]
        found = self.client.expect(
            "sessions.find", "OK",
            {"kind": "player", "localCount": 1, "properties": wildcard, "start": 0, "limit": 8}, bob,
        )["result"]["sessions"]
        require(any(row["session"] == session for row in found), "advertised session was not discoverable")
        self.client.expect("sessions.get", "NOT_AUTHORIZED", {"session": session}, bob)
        joined = self.client.expect(
            "sessions.join", "OK", {"session": session, "participants": [bob]}, bob
        )["result"]
        remote_machine = joined["machine"]
        grouped = self.client.expect(
            "sessions.addMembers", "OK", {"session": session, "participants": [dave]}, bob
        )["result"]
        require(
            len(grouped["members"]) == 3
            and sum(member["machine"] == remote_machine for member in grouped["members"]) == 2,
            "added local member was not attached to the caller's machine",
        )
        snapshot = self.client.expect("sessions.get", "OK", {"session": session}, alice)["result"]
        update = {
            "session": session, "revision": snapshot["revision"], "maxGamers": 5,
            "privateSlots": 0, "properties": properties, "allowJoinInProgress": False,
            "allowHostMigration": False, "state": "playing",
        }
        playing = self.client.expect("sessions.update", "OK", update, alice)["result"]
        require(playing["state"] == "playing", "host update did not enter gameplay")
        self.client.expect("sessions.update", "CONFLICT", update, alice)
        removed = self.client.expect(
            "sessions.remove", "OK", {"session": session, "machine": remote_machine}, alice
        )["result"]
        require(len(removed["members"]) == 1, "host removal did not remove the complete remote machine")
        self.client.expect("sessions.get", "REMOVED_BY_HOST", {"session": session}, bob)
        self.client.expect("sessions.get", "REMOVED_BY_HOST", {"session": session}, dave)
        require(
            self.client.expect("sessions.leave", "OK", {"session": session}, alice)["result"]["ended"],
            "last host did not end the removal fixture",
        )

    def parties_and_join_friend(self) -> None:
        alice = self.credentials["alice"]["token"]
        bob = self.credentials["bob"]["token"]
        # Blocking deliberately removed the friendship in the preceding privacy case. Rebuild it
        # explicitly so this scenario proves that unblock alone does not silently restore trust.
        self.client.expect("friends.add", "OK", {"gamertag": "Bob"}, alice)
        self.client.expect("friends.accept", "OK", {"gamertag": "Alice"}, bob)
        initial = self.client.expect("parties.get", "OK", {}, alice)["result"]
        require(initial["party"] is None, "party fixture did not start empty")
        invited = self.client.expect("parties.invite", "OK", {"gamertag": "Bob"}, alice)["result"]
        party = invited["party"]["id"]
        bob_inbox = self.client.expect("parties.get", "OK", {}, bob)["result"]["invitations"]
        require(len(bob_inbox) == 1 and bob_inbox[0]["party"] == party, "party invitation was not visible")
        self.client.expect("parties.decline", "OK", {"party": party}, bob)
        require(
            not self.client.expect("parties.get", "OK", {}, bob)["result"]["invitations"],
            "declined party invitation remained visible",
        )
        self.client.expect("parties.invite", "OK", {"gamertag": "Bob"}, alice)
        joined = self.client.expect("parties.accept", "OK", {"party": party}, bob)["result"]
        require(len(joined["party"]["members"]) == 2, "party acceptance did not add the member")
        self.client.expect("parties.leave", "OK", {}, bob)
        self.client.expect("parties.leave", "OK", {}, alice)

        created = self.client.expect(
            "sessions.create", "OK",
            {
                "kind": "player", "maxGamers": 4, "privateSlots": 0, "properties": [None] * 8,
                "allowJoinInProgress": True, "allowHostMigration": False, "participants": [alice],
            }, alice,
        )["result"]
        request = self.client.expect("invites.joinFriend", "OK", {"gamertag": "Alice"}, bob)["result"]
        require(request["session"] == created["session"] and request["status"] == "pending", "join-friend grant mismatch")
        self.client.expect("invites.accept", "OK", {"invite": request["invite"]}, bob)
        self.client.expect(
            "sessions.joinInvited", "OK",
            {"session": created["session"], "invite": request["invite"], "participants": [bob]}, bob,
        )
        self.client.expect("sessions.leave", "OK", {"session": created["session"]}, bob)

        dismissed = self.client.expect(
            "invites.send", "OK", {"session": created["session"], "gamertag": "Bob"}, alice
        )["result"]
        inspected = self.client.expect("invites.get", "OK", {"invite": dismissed["invite"]}, bob)["result"]
        require(inspected["status"] == "pending", "invitation inspection changed its state")
        dismissed = self.client.expect("invites.dismiss", "OK", {"invite": dismissed["invite"]}, bob)["result"]
        require(dismissed["status"] == "dismissed", "invitation was not dismissed")
        self.client.expect("sessions.leave", "OK", {"session": created["session"]}, alice)

    def ranked_arbitration(self) -> None:
        alice = self.credentials["alice"]
        bob = self.credentials["bob"]
        properties = [None] * 8
        created = self.client.expect(
            "sessions.create", "OK",
            {
                "kind": "ranked", "maxGamers": 2, "privateSlots": 0, "properties": properties,
                "allowJoinInProgress": False, "allowHostMigration": False,
                "participants": [alice["token"]],
            }, alice["token"],
        )["result"]
        session = created["session"]
        self.client.expect(
            "sessions.join", "OK", {"session": session, "participants": [bob["token"]]}, bob["token"]
        )
        snapshot = self.client.expect("sessions.get", "OK", {"session": session}, alice["token"])["result"]
        playing = self.client.expect(
            "sessions.update", "OK",
            {
                "session": session, "revision": snapshot["revision"], "maxGamers": 2,
                "privateSlots": 0, "properties": properties, "allowJoinInProgress": False,
                "allowHostMigration": False, "state": "playing",
            }, alice["token"],
        )["result"]
        arbitration = {"session": session, "revision": playing["revision"]}
        rows = [
            {
                "userId": alice["identity"]["userId"], "key": "RankedScore", "mode": 0,
                "rating": 100, "columns": {},
            },
            {
                "userId": bob["identity"]["userId"], "key": "RankedScore", "mode": 0,
                "rating": 80, "columns": {},
            },
        ]
        for credential in (alice, bob):
            epoch = self.client.expect(
                "leaderboards.game.begin", "OK",
                {"kind": "local", "participants": [credential["token"]]}, credential["token"],
            )["result"]["gameplay"]
            self.client.expect(
                "leaderboards.game.commit", "OK",
                {"gameplay": epoch, "entries": rows, "arbitration": arbitration}, credential["token"],
            )
        result = self.client.expect(
            "leaderboards.read", "OK",
            {"key": "RankedScore", "mode": 0, "start": 0, "size": 10}, alice["token"],
        )["result"]
        require(
            {row["gamertag"]: row["rating"] for row in result["entries"]} == {"Alice": 100, "Bob": 80},
            "matching machine reports did not resolve the Ranked round",
        )
        self.client.expect("sessions.leave", "OK", {"session": session}, bob["token"])
        self.client.expect("sessions.leave", "OK", {"session": session}, alice["token"])

    def logout(self) -> None:
        fresh = self.client.expect(
            "auth.login", "OK", {"username": "carol", "password": PASSWORDS["carol"]}
        )["result"]
        self.client.expect("auth.logout", "OK", {}, fresh["token"])
        self.client.expect("auth.ping", "UNAUTHENTICATED", {}, fresh["token"])

    def access_expiration(self) -> None:
        fresh = self.client.expect(
            "auth.login", "OK", {"username": "dave", "password": PASSWORDS["dave"]}
        )["result"]
        self.server.command("expire-access", "dave")
        self.client.expect("auth.ping", "UNAUTHENTICATED", {}, fresh["token"])
        rotated = self.client.expect(
            "auth.refresh", "OK", {"refreshToken": fresh["refreshToken"]}
        )["result"]
        self.client.expect("auth.ping", "OK", {}, rotated["token"])

    def operation_coverage(self) -> None:
        missing = sorted(PUBLIC_OPERATIONS - self.client.operations)
        require(not missing, "public operations with no black-box exercise: " + ", ".join(missing))

    def run(self) -> list[dict]:
        cases = [
            ("hello and capability discovery", self.hello),
            ("envelope and version validation", self.envelope_validation),
            ("authentication and title isolation", self.authentication),
            ("identity, profile, gamer zone and defaults", self.identity_and_preferences),
            ("achievements, leaderboards, assets and files", self.resources_and_progress),
            ("local leaderboard commit and abort", self.local_leaderboard_commit),
            ("refresh rotation and replay revocation", self.refresh_rotation),
            ("friends, presence, messages, reviews and request replay", self.social_state),
            ("documented request boundaries", self.boundaries),
            ("avatar read, validation and empty-catalog behavior", self.avatar_surface),
            ("session and invitation lifecycle", self.session_and_invitation),
            ("relay ticket, framing, routing and replay", self.relay),
            ("host migration and privacy enforcement", self.host_migration_and_privacy),
            ("directory mutation, local members and host removal", self.directory_mutation_and_removal),
            ("parties, join-friend and invitation dismissal", self.parties_and_join_friend),
            ("Ranked arbitration agreement", self.ranked_arbitration),
            ("event hints and event hello validation", self.events),
            ("access-token expiration and refresh recovery", self.access_expiration),
            ("logout revocation", self.logout),
            ("public operation coverage", self.operation_coverage),
        ]
        if self.include_slow:
            cases.insert(12, ("relay ticket wall-clock expiry", self.relay_ticket_expiry))
        for name, function in cases:
            self.record(name, function)
        return self.cases


def first_line(command: list[str], cwd: pathlib.Path | None = None) -> str:
    try:
        result = subprocess.run(command, cwd=cwd, check=False, text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        return result.stdout.splitlines()[0] if result.stdout else "unknown"
    except OSError:
        return "unknown"


def build_type(build: pathlib.Path) -> str:
    cache = build / "CMakeCache.txt"
    if cache.is_file():
        for line in cache.read_text(errors="replace").splitlines():
            if line.startswith("CMAKE_BUILD_TYPE:STRING="):
                return line.split("=", 1)[1]
    return "unknown"


def cpu_description() -> str:
    cpuinfo = pathlib.Path("/proc/cpuinfo")
    if cpuinfo.is_file():
        for line in cpuinfo.read_text(errors="replace").splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    return platform.processor() or "unknown"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=pathlib.Path, help="ccache-built CMake binary directory")
    parser.add_argument("--json", type=pathlib.Path, help="write the complete machine-readable report here")
    parser.add_argument("--keep-work-dir", action="store_true", help="retain scratch state for diagnosis (never use production state)")
    parser.add_argument("--smoke", action="store_true", help="CI-compatible profile; skips only deliberate wall-clock expiry")
    options = parser.parse_args()

    repository = pathlib.Path(__file__).resolve().parents[2]
    server = ManagedServer(options.build, options.keep_work_dir)
    started = time.perf_counter()
    setup_error = ""
    cases: list[dict] = []
    try:
        server.start()
        cases = Suite(server, include_slow=not options.smoke).run()
        require(server.process is not None and server.process.poll() is None, "server exited during conformance run")
    except Exception as error:
        setup_error = f"{type(error).__name__}: {error}"
    finally:
        retained = str(server.root) if options.keep_work_dir and server.root else ""
        server.stop()

    passed = sum(case["status"] == "passed" for case in cases)
    failed = sum(case["status"] == "failed" for case in cases) + (1 if setup_error else 0)
    report = {
        "schemaVersion": 1,
        "tool": "cna-gamer-services-conformance",
        "toolVersion": TOOL_VERSION,
        "timestamp": dt.datetime.now(dt.timezone.utc).isoformat(),
        "gitCommit": first_line(["git", "rev-parse", "HEAD"], repository),
        "gitDirty": bool(first_line(["git", "status", "--porcelain"], repository)),
        "compiler": first_line(["c++", "--version"]),
        "buildType": build_type(options.build.resolve()),
        "os": platform.platform(),
        "cpu": cpu_description(),
        "configuration": {"profile": "smoke" if options.smoke else "default", "transport": "verified loopback TLS/WSS", "workers": 1},
        "durationSeconds": round(time.perf_counter() - started, 3),
        "summary": {"passed": passed, "failed": failed, "total": len(cases) + (1 if setup_error else 0)},
        "setupError": setup_error,
        "retainedWorkDirectory": retained,
        "cases": cases,
        "scopeBoundary": "Loopback black-box protocol evidence; not CNA-harness or public-Internet qualification.",
    }
    if options.json:
        options.json.parent.mkdir(parents=True, exist_ok=True)
        options.json.write_text(json.dumps(report, indent=2) + "\n")

    for case in cases:
        marker = "PASS" if case["status"] == "passed" else "FAIL"
        suffix = f" — {case['detail']}" if case["detail"] else ""
        print(f"{marker:4} {case['name']} ({case['durationMs']:.2f} ms){suffix}")
    if setup_error:
        print(f"FAIL setup — {setup_error}")
    print(f"\nConformance: {passed} passed, {failed} failed in {report['durationSeconds']:.3f}s")
    print(report["scopeBoundary"])
    if retained:
        print(f"Scratch state retained at {retained}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
