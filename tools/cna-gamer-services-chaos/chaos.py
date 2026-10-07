#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Scratch-only process, transport and SQLite failure qualification."""

from __future__ import annotations

import argparse
import asyncio
import datetime as dt
import hashlib
import http.client
import json
import os
import pathlib
import platform
import shutil
import signal
import socket
import sqlite3
import ssl
import subprocess
import tempfile
import threading
import time
import uuid

import websockets


TITLE = "chaos"
PASSWORD = "chaos-password"


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


class Client:
    def __init__(self, lab):
        self.lab = lab
        self.context = ssl.create_default_context(cafile=str(lab.certificate))

    def call(self, operation, arguments=None, token=None, request_id=None):
        envelope = {"v": 1, "id": request_id or uuid.uuid4().hex, "game": TITLE,
                    "op": operation, "args": arguments or {}}
        if token:
            envelope["token"] = token
        connection = http.client.HTTPSConnection("localhost", self.lab.port, context=self.context, timeout=12)
        try:
            connection.request("POST", "/cna/v1", json.dumps(envelope), {"Content-Type": "application/json"})
            response = connection.getresponse()
            require(response.status == 200, f"HTTP {response.status}")
            return json.loads(response.read())
        finally:
            connection.close()

    def send_and_disconnect(self, operation, arguments, token, request_id):
        envelope = {"v": 1, "id": request_id, "game": TITLE, "op": operation,
                    "args": arguments, "token": token}
        body = json.dumps(envelope).encode()
        raw = socket.create_connection(("127.0.0.1", self.lab.port), timeout=10)
        wrapped = self.context.wrap_socket(raw, server_hostname="localhost")
        request = (f"POST /cna/v1 HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
                   f"Content-Length: {len(body)}\r\nConnection: close\r\n\r\n").encode() + body
        wrapped.sendall(request)
        wrapped.close()


class Lab:
    def __init__(self, build):
        self.build = build.resolve()
        self.temporary = tempfile.TemporaryDirectory(prefix="cna-chaos-")
        self.root = pathlib.Path(self.temporary.name)
        os.chmod(self.root, 0o700)
        self.database = self.root / "state.sqlite3"
        self.certificate = self.root / "certificate.pem"
        self.key = self.root / "private-key.pem"
        self.process = None
        self.port = 0

    @property
    def server(self):
        return self.build / "cna-gamer-services-server"

    @property
    def admin(self):
        return self.build / "cna-gamer-services-admin"

    def command(self, *arguments, stdin=None):
        result = subprocess.run([str(self.admin), str(self.database), *arguments], input=stdin,
                                text=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        require(result.returncode == 0, f"admin {' '.join(arguments[:2])}: {result.stderr.strip()}")

    def provision(self):
        require(self.server.is_file() and self.admin.is_file(), "build lacks server/admin executables")
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                        "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost",
                        "-keyout", str(self.key), "-out", str(self.certificate)], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.command("title", TITLE, "Chaos scratch title")
        for name in ("alice", "bob"):
            self.command("user", name, name.title(), stdin=PASSWORD + "\n")

    def start(self, file_size_limit=None):
        require(self.process is None, "owned server is already running")
        child_setup = None
        if file_size_limit is not None:
            def restrict_file_growth():
                import resource
                signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
                resource.setrlimit(resource.RLIMIT_FSIZE, (file_size_limit, file_size_limit))
            child_setup = restrict_file_growth
        self.process = subprocess.Popen(
            [str(self.server), "--database", str(self.database), "--listen", "127.0.0.1",
             "--port", "0", "--cert", str(self.certificate), "--key", str(self.key),
             "--diagnostics-port", "0"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            preexec_fn=child_setup,
        )
        line = self.process.stdout.readline().strip()
        diagnostics = self.process.stdout.readline().strip()
        require(line.startswith("CNA service listening") and diagnostics.startswith("CNA diagnostics listening"),
                "server startup failed: " + (line or self.process.stderr.read()))
        self.port = int(line.rsplit(":", 1)[1])

    def stop(self, signum=signal.SIGTERM):
        if self.process is None:
            return
        if self.process.poll() is None:
            self.process.send_signal(signum)
            self.process.wait(timeout=15)
        self.process.stdout.close();self.process.stderr.close();self.process = None

    def restart(self, signum=signal.SIGTERM):
        self.stop(signum);self.start()

    def close(self):
        self.stop();self.temporary.cleanup()

    def integrity(self):
        with sqlite3.connect(f"file:{self.database}?mode=ro", uri=True) as connection:
            return connection.execute("PRAGMA integrity_check").fetchone()[0]


class Suite:
    def __init__(self, lab):
        self.lab = lab
        self.client = Client(lab)
        self.cases = []
        self.alice = None
        self.bob = None
        self.session = None
        self.alice_machine = None
        self.bob_machine = None

    def record(self, name, function):
        started = time.perf_counter();detail = "";status = "passed"
        try:
            detail = function() or ""
        except Exception as error:
            status = "failed";detail = f"{type(error).__name__}: {error}"
        self.cases.append({"name": name, "status": status,
                           "durationMs": round((time.perf_counter() - started) * 1000, 3), "detail": detail})

    def login(self, name):
        result = self.client.call("auth.login", {"username": name, "password": PASSWORD})
        require(result["error"] == "OK", f"{name} login: {result['error']}")
        return result["result"]

    def graceful_restart(self):
        self.alice, self.bob = self.login("alice"), self.login("bob")
        require(self.client.call("friends.add", {"gamertag": "Bob"}, self.alice["token"])["error"] == "OK",
                "friend request")
        require(self.client.call("friends.accept", {"gamertag": "Alice"}, self.bob["token"])["error"] == "OK",
                "friend acceptance")
        self.lab.restart(signal.SIGTERM);self.client = Client(self.lab)
        require(self.client.call("auth.ping", {}, self.alice["token"])["error"] == "OK", "access token after SIGTERM")
        friends = self.client.call("friends.list", {}, self.alice["token"])
        require(friends["error"] == "OK" and len(friends["result"]["friends"]) == 1, "friendship after SIGTERM")
        require(self.lab.integrity() == "ok", "integrity after SIGTERM")
        return "durable credentials and friendship survived"

    def active_session_sigkill(self):
        created = self.client.call("sessions.create", {"kind": "player", "maxGamers": 4,
            "privateSlots": 0, "properties": [None] * 8, "allowJoinInProgress": True,
            "allowHostMigration": True, "participants": [self.alice["token"]]}, self.alice["token"])
        require(created["error"] == "OK", "session creation")
        self.session = created["result"]["session"];self.alice_machine = created["result"]["hostMachine"]
        joined = self.client.call("sessions.join", {"session": self.session,
            "participants": [self.bob["token"]]}, self.bob["token"])
        require(joined["error"] == "OK", "session join");self.bob_machine = joined["result"]["machine"]
        self.lab.restart(signal.SIGKILL);self.client = Client(self.lab)
        require(self.lab.integrity() == "ok", "integrity after SIGKILL")
        require(self.client.call("auth.ping", {}, self.alice["token"])["error"] == "OK", "credential after SIGKILL")
        snapshot = self.client.call("sessions.get", {"session": self.session}, self.bob["token"])
        require(snapshot["error"] == "OK" and len(snapshot["result"]["members"]) == 2,
                "active directory session after SIGKILL")
        asyncio.run(self.reconnect_channels())
        return "WAL recovery, session authority, event auth and relay routing survived process crash"

    async def reconnect_channels(self):
        event_endpoint = f"wss://localhost:{self.lab.port}/cna/v1/events"
        relay_endpoint = f"wss://localhost:{self.lab.port}/cna/relay/v1"
        context = ssl.create_default_context(cafile=str(self.lab.certificate))
        event = await websockets.connect(event_endpoint, ssl=context, compression=None, proxy=None,
                                         ping_interval=None, close_timeout=2)
        await event.send(json.dumps({"v": 1, "id": "chaos-event", "game": TITLE,
                                     "token": self.alice["token"]}))
        require(json.loads(await asyncio.wait_for(event.recv(), 10))["error"] == "OK", "event reconnect")

        def ticket(credential):
            result = self.client.call("sessions.relayTicket", {"session": self.session,
                                      "participants": [credential["token"]]}, credential["token"])
            require(result["error"] == "OK", "relay ticket after restart")
            return result["result"]["ticket"]

        async def relay(secret, name):
            channel = await websockets.connect(relay_endpoint, ssl=context, compression=None, proxy=None,
                                               ping_interval=None, close_timeout=2)
            await channel.send(json.dumps({"v": 1, "id": name, "game": TITLE, "ticket": secret}))
            require(json.loads(await asyncio.wait_for(channel.recv(), 10))["error"] == "OK", "relay reconnect")
            return channel

        left = await relay(ticket(self.alice), "left");right = await relay(ticket(self.bob), "right")
        try:
            payload = b"chaos-reconnect"
            await left.send(b"CNR\x01\x01\x00\x00\x00" + bytes.fromhex(self.bob_machine) + payload)
            expected = b"CNR\x01\x01\x00\x00\x00" + bytes.fromhex(self.alice_machine) + payload
            require(await asyncio.wait_for(right.recv(), 10) == expected, "relay route after restart")
        finally:
            await left.close();await right.close();await event.close()

    def disconnect_during_mutation(self):
        request_id = "disconnect-" + uuid.uuid4().hex
        self.client.send_and_disconnect("messages.send", {"gamertags": ["Bob"], "text": "exactly once"},
                                        self.alice["token"], request_id)
        time.sleep(.1)
        retry = self.client.call("messages.send", {"gamertags": ["Bob"], "text": "exactly once"},
                                 self.alice["token"], request_id)
        require(retry["error"] == "OK", "retry after disconnect")
        inbox = self.client.call("messages.list", {"start": 0, "limit": 10}, self.bob["token"])
        rows = [message for message in inbox["result"]["messages"] if message["text"] == "exactly once"]
        require(len(rows) == 1, f"disconnect mutation delivered {len(rows)} times")
        return "retry converged to one durable message"

    def relay_ticket_refusals(self):
        def issue():
            result = self.client.call(
                "sessions.relayTicket", {"session": self.session, "participants": [self.alice["token"]]},
                self.alice["token"],
            )
            require(result["error"] == "OK", "relay ticket issue")
            return result["result"]["ticket"]

        async def expect_refused(ticket, name):
            endpoint = f"wss://localhost:{self.lab.port}/cna/relay/v1"
            context = ssl.create_default_context(cafile=str(self.lab.certificate))
            channel = await websockets.connect(endpoint, ssl=context, compression=None, proxy=None,
                                               ping_interval=None, close_timeout=2)
            await channel.send(json.dumps({"v": 1, "id": name, "game": TITLE, "ticket": ticket}))
            try:
                await asyncio.wait_for(channel.recv(), 10)
                raise RuntimeError(name + " relay ticket was accepted")
            except websockets.ConnectionClosed as closed:
                require(closed.rcvd is not None and closed.rcvd.code == 1008,
                        name + " relay refusal used the wrong close policy")
            finally:
                await channel.close()

        async def redeem_once(ticket):
            endpoint = f"wss://localhost:{self.lab.port}/cna/relay/v1"
            context = ssl.create_default_context(cafile=str(self.lab.certificate))
            channel = await websockets.connect(endpoint, ssl=context, compression=None, proxy=None,
                                               ping_interval=None, close_timeout=2)
            await channel.send(json.dumps({"v": 1, "id": "first-use", "game": TITLE, "ticket": ticket}))
            welcome = json.loads(await asyncio.wait_for(channel.recv(), 10))
            require(welcome["error"] == "OK", "first relay ticket redemption")
            await channel.close()

        asyncio.run(expect_refused("0" * 64, "broken"))
        expired = issue()
        digest = hashlib.sha256(expired.encode()).hexdigest()
        with sqlite3.connect(self.lab.database, timeout=5) as connection:
            changed = connection.execute("UPDATE relay_tickets SET expires=0 WHERE hash=?", (digest,)).rowcount
            require(changed == 1, "scratch relay ticket expiry fixture")
        asyncio.run(expect_refused(expired, "expired"))
        used = issue()
        asyncio.run(redeem_once(used))
        asyncio.run(expect_refused(used, "replayed"))
        require(self.client.call("auth.ping", {}, self.alice["token"])["error"] == "OK",
                "service after relay ticket refusals")
        return "broken, expired and replayed one-use tickets failed closed"

    def database_busy(self):
        connection = sqlite3.connect(self.lab.database, timeout=1, check_same_thread=False)
        connection.execute("BEGIN IMMEDIATE")
        released = threading.Event()

        def unlock():
            time.sleep(6)
            connection.rollback();connection.close();released.set()

        thread = threading.Thread(target=unlock, daemon=True);thread.start()
        result = self.client.call("presence.set", {"mode": 9, "text": "busy"}, self.alice["token"])
        thread.join(timeout=10);require(released.is_set(), "scratch writer lock was not released")
        require(result["error"] == "INTERNAL_ERROR", f"busy outcome was {result['error']}")
        recovered = self.client.call("auth.ping", {}, self.alice["token"])
        require(recovered["error"] == "OK", "server did not recover after writer lock")
        return "busy timeout was contained and later work succeeded"

    def database_full(self):
        # A child-only file-size ceiling is a deterministic ENOSPC analogue. SIGXFSZ is ignored in
        # that child so SQLite observes a write failure instead of the kernel killing the process.
        self.lab.stop(signal.SIGTERM)
        self.lab.start(file_size_limit=64 * 1024)
        self.client = Client(self.lab)
        result = None
        try:
            for index in range(100):
                result = self.client.call(
                    "messages.send", {"gamertags": ["Bob"], "text": f"full-{index}-" + "x" * 240},
                    self.alice["token"],
                )
                if result["error"] != "OK":break
            require(result is not None and result["error"] == "INTERNAL_ERROR",
                    "scratch file ceiling did not reach a contained storage error")
            require(self.lab.process.poll() is None, "storage error terminated the server")
        finally:
            self.lab.stop(signal.SIGTERM)
            self.lab.start()
            self.client = Client(self.lab)
        require(self.client.call("auth.ping", {}, self.alice["token"])["error"] == "OK",
                "server did not recover after scratch file ceiling")
        require(self.lab.integrity() == "ok", "integrity after scratch file ceiling")
        return "child file ceiling produced contained INTERNAL_ERROR; normal restart recovered"

    def hostile_transport(self):
        payloads = [b"not tls", b"POST /cna/v1 HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\n{}",
                    b"X" * 70_000]
        for payload in payloads:
            connection = socket.create_connection(("127.0.0.1", self.lab.port), timeout=5)
            connection.sendall(payload);connection.close()
        time.sleep(.1)
        require(self.lab.process.poll() is None, "hostile transport ended server")
        require(self.client.call("hello")["error"] == "OK", "service after hostile transport")
        return "plaintext, truncated and oversized pre-handshake input was contained"

    def ownership_lock(self):
        second = subprocess.run(
            [str(self.lab.server), "--database", str(self.lab.database), "--listen", "127.0.0.1",
             "--port", "0", "--cert", str(self.lab.certificate), "--key", str(self.lab.key)],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15,
        )
        require(second.returncode != 0 and "DATABASE_IN_USE" in second.stderr, "second owner was not refused")
        require(self.client.call("hello")["error"] == "OK", "original owner affected")
        return "second process failed closed"

    def corrupt_copy(self):
        self.lab.stop(signal.SIGTERM)
        corrupt = self.lab.root / "corrupt-copy.sqlite3";shutil.copy2(self.lab.database, corrupt)
        with corrupt.open("r+b") as stream:
            stream.write(b"deliberately bad")
        attempt = subprocess.run(
            [str(self.lab.server), "--database", str(corrupt), "--listen", "127.0.0.1", "--port", "0",
             "--cert", str(self.lab.certificate), "--key", str(self.lab.key)],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15,
        )
        require(attempt.returncode != 0, "corrupted copy unexpectedly started")
        require(self.lab.integrity() == "ok", "original changed during corrupt-copy test")
        self.lab.start();self.client = Client(self.lab)
        require(self.client.call("auth.ping", {}, self.alice["token"])["error"] == "OK", "original restart")
        return "corrupted copy was rejected; original remained usable"

    def run(self):
        cases = [
            ("SIGTERM durable restart", self.graceful_restart),
            ("SIGKILL active-session reconnect", self.active_session_sigkill),
            ("disconnect during mutation", self.disconnect_during_mutation),
            ("broken, expired and replayed relay tickets", self.relay_ticket_refusals),
            ("database busy timeout and recovery", self.database_busy),
            ("controlled database-full recovery", self.database_full),
            ("hostile/truncated transport", self.hostile_transport),
            ("single database owner", self.ownership_lock),
            ("corrupted database copy", self.corrupt_copy),
        ]
        for name, function in cases:
            self.record(name, function)
        return self.cases


def first_line(command, cwd=None):
    try:
        result = subprocess.run(command, cwd=cwd, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL, timeout=10)
        return result.stdout.splitlines()[0] if result.stdout else "unknown"
    except (OSError, subprocess.TimeoutExpired):
        return "unknown"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=pathlib.Path)
    parser.add_argument("--json", type=pathlib.Path)
    parser.add_argument("--smoke", action="store_true", help="run the bounded CI profile (currently all cases)")
    options = parser.parse_args()
    repository = pathlib.Path(__file__).resolve().parents[2]
    lab = Lab(options.build);cases = [];setup_error = "";started = time.perf_counter()
    try:
        lab.provision();lab.start();cases = Suite(lab).run()
    except Exception as error:
        setup_error = f"{type(error).__name__}: {error}"
    finally:
        lab.close()
    report = {
        "schemaVersion": 1, "tool": "cna-gamer-services-chaos", "toolVersion": 1,
        "timestamp": dt.datetime.now(dt.timezone.utc).isoformat(),
        "gitCommit": first_line(["git", "rev-parse", "HEAD"], repository),
        "gitDirty": bool(first_line(["git", "status", "--porcelain"], repository)),
        "compiler": first_line(["c++", "--version"]), "os": platform.platform(),
        "durationSeconds": round(time.perf_counter() - started, 3), "setupError": setup_error,
        "configuration": {"profile": "smoke" if options.smoke else "default", "workers": 1,
                          "state": "generated disposable database and certificate"},
        "cases": cases,
        "scopeBoundary": "Process-crash/SQLite/transport evidence on scratch state; not OS crash, power loss or filesystem fault injection.",
    }
    if options.json:
        options.json.parent.mkdir(parents=True, exist_ok=True)
        options.json.write_text(json.dumps(report, indent=2) + "\n")
    for case in cases:
        print(f"{'PASS' if case['status'] == 'passed' else 'FAIL':4} {case['name']} "
              f"({case['durationMs']:.2f} ms) — {case['detail']}")
    if setup_error:print("FAIL setup —", setup_error)
    failed = sum(case["status"] != "passed" for case in cases) + bool(setup_error)
    print(f"Chaos: {len(cases) - failed} passed, {failed} failed in {report['durationSeconds']}s")
    print(report["scopeBoundary"])
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
