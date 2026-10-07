#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bounded-worker black-box load, stress and soak laboratory."""

from __future__ import annotations

import argparse
import asyncio
import collections
import datetime as dt
import html
import http.client
import json
import os
import pathlib
import platform
import random
import ssl
import subprocess
import tempfile
import threading
import time
import urllib.request
import uuid
from concurrent.futures import ThreadPoolExecutor

import websockets


TITLE = "loadlab"
PASSWORD = "loadlab-password"
MAX_WORKERS = 6
MAX_SAMPLES = 50_000
KNOWN_SCENARIOS = {
    "ordinary", "signin-storm", "refresh-storm", "session-churn", "directory-pressure",
    "reconnect-storm", "host-migration", "events", "relay", "soak",
}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def source_address(index):
    """Distinct addresses within 127/8 model unrelated peers without namespaces."""
    return f"127.{1 + index // 62_500}.{1 + index // 250 % 250}.{1 + index % 250}"


class Client:
    def __init__(self, port, certificate, source_index=0, reconnect=False):
        self.port = port
        self.context = ssl.create_default_context(cafile=str(certificate))
        self.source_index = source_index
        self.reconnect = reconnect
        self.connection = None

    def close(self):
        if self.connection is not None:
            self.connection.close()
            self.connection = None

    def call(self, operation, arguments=None, token=None, source_index=None):
        body = {"v": 1, "id": uuid.uuid4().hex, "game": TITLE, "op": operation,
                "args": arguments or {}}
        if token:
            body["token"] = token
        started = time.perf_counter()
        try:
            if self.connection is None:
                index = self.source_index if source_index is None else source_index
                self.connection = http.client.HTTPSConnection(
                    "localhost", self.port, context=self.context, timeout=20,
                    source_address=(source_address(index), 0),
                )
            headers = {"Content-Type": "application/json"}
            if self.reconnect:
                headers["Connection"] = "close"
            self.connection.request("POST", "/cna/v1", json.dumps(body, separators=(",", ":")), headers)
            response = self.connection.getresponse()
            payload = json.loads(response.read())
            if self.reconnect or response.will_close:
                self.close()
            return payload["error"], time.perf_counter() - started, payload.get("result")
        except Exception as error:
            self.close()
            return "TRANSPORT_" + type(error).__name__, time.perf_counter() - started, None


class Stats:
    def __init__(self, seed=1):
        self.requests = 0
        self.successes = 0
        self.errors = collections.Counter()
        self.operations = collections.Counter()
        self.samples = []
        self.random = random.Random(seed)
        self.extra = collections.Counter()

    def add(self, operation, outcome, seconds):
        self.requests += 1
        self.operations[operation] += 1
        self.errors[outcome] += 1
        if outcome == "OK":
            self.successes += 1
        milliseconds = seconds * 1000
        if len(self.samples) < MAX_SAMPLES:
            self.samples.append(milliseconds)
        else:
            index = self.random.randrange(self.requests)
            if index < MAX_SAMPLES:
                self.samples[index] = milliseconds

    def merge(self, other):
        previous = self.requests
        self.requests += other.requests
        self.successes += other.successes
        self.errors.update(other.errors)
        self.operations.update(other.operations)
        self.extra.update(other.extra)
        for offset, sample in enumerate(other.samples):
            if len(self.samples) < MAX_SAMPLES:
                self.samples.append(sample)
            else:
                index = self.random.randrange(max(1, previous + offset + 1))
                if index < MAX_SAMPLES:
                    self.samples[index] = sample


def percentile(values, fraction):
    if not values:
        return 0.0
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(fraction * len(ordered)))]


def summarize(name, stats, seconds, resources, detail=None):
    relay = {key: value for key, value in stats.extra.items() if key.startswith("relay")}
    connections = {key: value for key, value in stats.extra.items() if not key.startswith("relay")}
    return {
        "scenario": name,
        "durationSeconds": round(seconds, 3),
        "requestCount": stats.requests,
        "successCount": stats.successes,
        "failureCount": stats.requests - stats.successes,
        "failuresByError": dict(sorted((key, value) for key, value in stats.errors.items() if key != "OK")),
        "outcomes": dict(sorted(stats.errors.items())),
        "operations": dict(sorted(stats.operations.items())),
        "throughputPerSecond": round(stats.requests / seconds, 2) if seconds else 0,
        "latencyMs": {
            "p50": round(percentile(stats.samples, .50), 3),
            "p95": round(percentile(stats.samples, .95), 3),
            "p99": round(percentile(stats.samples, .99), 3),
            "max": round(max(stats.samples, default=0), 3),
            "sampleCount": len(stats.samples),
        },
        "connectionStatistics": dict(sorted(connections.items())),
        "relayStatistics": dict(sorted(relay.items())),
        "serverResources": resources,
        "detail": detail or {},
    }


class Lab:
    def __init__(self, build, players, workers):
        self.build = build.resolve()
        self.players = players
        self.workers = workers
        self.temporary = tempfile.TemporaryDirectory(prefix="cna-loadlab-")
        self.root = pathlib.Path(self.temporary.name)
        os.chmod(self.root, 0o700)
        self.database = self.root / "state.sqlite3"
        self.certificate = self.root / "certificate.pem"
        self.key = self.root / "private-key.pem"
        self.process = None
        self.port = 0
        self.diagnostics_port = 0

    def admin(self, *arguments, stdin=None):
        result = subprocess.run(
            [str(self.build / "cna-gamer-services-admin"), str(self.database), *arguments],
            input=stdin, text=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
        )
        require(result.returncode == 0, f"admin {' '.join(arguments[:2])}: {result.stderr.strip()}")

    def start(self):
        require((self.build / "cna-gamer-services-server").is_file(), "server executable is missing")
        subprocess.run(
            ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
             "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost",
             "-keyout", str(self.key), "-out", str(self.certificate)],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        self.admin("title", TITLE, "Load laboratory")
        self.admin("leaderboard", TITLE, stdin=json.dumps({
            "key": "Score", "mode": 0, "ascending": False, "aggregation": "best",
            "arbitrated": False, "columns": {"Rounds": "int32"},
        }))
        for index in range(self.players):
            self.admin("user", f"player{index}", f"Player{index}", stdin=PASSWORD + "\n")
        self.start_process()

    def start_process(self):
        self.process = subprocess.Popen(
            [str(self.build / "cna-gamer-services-server"), "--database", str(self.database),
             "--listen", "127.0.0.1", "--port", "0", "--cert", str(self.certificate),
             "--key", str(self.key), "--diagnostics-port", "0"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        service_line = self.process.stdout.readline().strip()
        diagnostics_line = self.process.stdout.readline().strip()
        require(service_line.startswith("CNA service listening"), "server startup: " + service_line)
        require(diagnostics_line.startswith("CNA diagnostics listening"), "diagnostics startup: " + diagnostics_line)
        self.port = int(service_line.rsplit(":", 1)[1])
        self.diagnostics_port = int(diagnostics_line.rsplit(":", 1)[1])

    def stop_process(self):
        if self.process is not None:
            if self.process.poll() is None:
                self.process.terminate()
                try:
                    self.process.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait(timeout=5)
            self.process.stdout.close()
            self.process.stderr.close()
            self.process = None

    def restart(self):
        self.stop_process()
        self.start_process()

    def stop(self):
        self.stop_process()
        self.temporary.cleanup()

    def call(self, index, operation, arguments=None, token=None, reconnect=False):
        client = Client(self.port, self.certificate, index, reconnect)
        try:
            return client.call(operation, arguments, token)
        finally:
            client.close()

    def credentials(self):
        def login(index):
            outcome, _, result = self.call(index, "auth.login", {
                "username": f"player{index}", "password": PASSWORD,
            })
            require(outcome == "OK", f"player{index} login: {outcome}")
            return result
        with ThreadPoolExecutor(max_workers=self.workers) as executor:
            return list(executor.map(login, range(self.players)))

    def metrics(self):
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        with opener.open(f"http://127.0.0.1:{self.diagnostics_port}/metrics", timeout=10) as response:
            return response.read().decode()

    def resources(self):
        if self.process is None:
            return {}
        result = {"timestampSeconds": round(time.time(), 3)}
        status = pathlib.Path(f"/proc/{self.process.pid}/status")
        if status.is_file():
            values = {}
            for line in status.read_text(errors="replace").splitlines():
                if line.startswith(("VmRSS:", "VmHWM:", "Threads:")):
                    key, value = line.split(":", 1)
                    values[key] = int(value.strip().split()[0])
            result.update({"rssKiB": values.get("VmRSS", 0), "peakRssKiB": values.get("VmHWM", 0),
                           "serverThreads": values.get("Threads", 0)})
            descriptors = pathlib.Path(f"/proc/{self.process.pid}/fd")
            if descriptors.is_dir():
                result["openFileDescriptors"] = len(list(descriptors.iterdir()))
        for line in self.metrics().splitlines():
            if line.startswith("cna_database_bytes "):
                result["databaseBytes"] = int(float(line.split()[1]))
            elif line.startswith('cna_connections{surface="control"}'):
                result["controlConnections"] = int(float(line.split()[1]))
            elif line.startswith('cna_connections{surface="relay"}'):
                result["relayConnections"] = int(float(line.split()[1]))
            elif line.startswith('cna_connections{surface="events"}'):
                result["eventConnections"] = int(float(line.split()[1]))
        return result

    def parallel(self, function):
        with ThreadPoolExecutor(max_workers=self.workers) as executor:
            partial = list(executor.map(function, range(self.workers)))
        merged = Stats(97)
        for stats in partial:
            merged.merge(stats)
        return merged

    def ordinary(self, seconds, reconnect=False, resource_samples=None):
        credentials = self.credentials()
        deadline = time.monotonic() + seconds
        operations = ("auth.ping", "friends.list", "presence.set", "profile.get",
                      "leaderboards.read", "sessions.find", "achievements.list", "presence.status")

        def work(worker):
            stats = Stats(worker + 1)
            assigned = list(range(worker, self.players, self.workers))
            clients = {index: Client(self.port, self.certificate, index, reconnect) for index in assigned}
            randomizer = random.Random(worker)
            next_resource_sample = time.monotonic() + 30
            try:
                while time.monotonic() < deadline:
                    for index in assigned:
                        operation = randomizer.choice(operations)
                        arguments = {
                            "auth.ping": {}, "friends.list": {}, "achievements.list": {},
                            "presence.set": {"mode": randomizer.randrange(256), "text": "Loadlab"},
                            "presence.status": {"status": randomizer.choice(("online", "away", "busy"))},
                            "profile.get": {"gamertag": f"Player{(index + 1) % self.players}"},
                            "leaderboards.read": {"key": "Score", "mode": 0, "start": 0, "size": 10},
                            "sessions.find": {"kind": "player", "localCount": 1, "start": 0, "limit": 16,
                                              "properties": [None] * 8},
                        }[operation]
                        outcome, latency, _ = clients[index].call(operation, arguments, credentials[index]["token"])
                        stats.add(operation, outcome, latency)
                        if worker == 0 and resource_samples is not None and time.monotonic() >= next_resource_sample:
                            resource_samples.append(self.resources());next_resource_sample += 30
                        if time.monotonic() >= deadline:
                            break
            finally:
                for client in clients.values():
                    client.close()
            return stats
        return self.parallel(work)

    def signins(self, seconds):
        deadline = time.monotonic() + seconds
        counter = iter(range(100_000, 1_000_000))
        lock = threading.Lock()

        def work(worker):
            stats = Stats(worker + 11)
            attempt = 0
            while time.monotonic() < deadline:
                with lock:
                    source = next(counter)
                client = Client(self.port, self.certificate, source, True)
                operation = "auth.login"
                outcome, latency, _ = client.call(operation, {
                    "username": f"player{attempt % self.players}", "password": PASSWORD,
                })
                client.close();stats.add(operation, outcome, latency);attempt += 1
            return stats
        return self.parallel(work)

    def refreshes(self, seconds):
        credentials = self.credentials()
        deadline = time.monotonic() + seconds

        def work(worker):
            stats = Stats(worker + 21)
            assigned = list(range(worker, self.players, self.workers))
            clients = {index: Client(self.port, self.certificate, index) for index in assigned}
            while time.monotonic() < deadline:
                for index in assigned:
                    outcome, latency, result = clients[index].call("auth.refresh", {
                        "refreshToken": credentials[index]["refreshToken"]})
                    stats.add("auth.refresh", outcome, latency)
                    if outcome == "OK":
                        credentials[index] = result
                    if time.monotonic() >= deadline:
                        break
            for client in clients.values():
                client.close()
            return stats
        return self.parallel(work)

    def session_churn(self, seconds):
        credentials = self.credentials();deadline = time.monotonic() + seconds

        def work(worker):
            stats = Stats(worker + 31)
            for index in range(worker, self.players, self.workers):
                client = Client(self.port, self.certificate, index)
                while time.monotonic() < deadline:
                    token = credentials[index]["token"]
                    outcome, latency, result = client.call("sessions.create", {
                        "kind": "player", "maxGamers": 4, "privateSlots": 0, "properties": [None] * 8,
                        "allowJoinInProgress": True, "allowHostMigration": True, "participants": [token]}, token)
                    stats.add("sessions.create", outcome, latency)
                    if outcome == "OK":
                        session = result["session"]
                        touch = client.call("sessions.touch", {"session": session}, token)
                        leave = client.call("sessions.leave", {"session": session}, token)
                        stats.add("sessions.touch", touch[0], touch[1]);stats.add("sessions.leave", leave[0], leave[1])
                    if time.monotonic() >= deadline:
                        break
                client.close()
            return stats
        return self.parallel(work)

    def directory_pressure(self, seconds):
        credentials = self.credentials();hosts = []
        for index in range(min(self.players, 16)):
            outcome, _, result = self.call(index, "sessions.create", {
                "kind": "player", "maxGamers": 8, "privateSlots": 0,
                "properties": [index % 4] + [None] * 7, "allowJoinInProgress": True,
                "allowHostMigration": False, "participants": [credentials[index]["token"]]},
                credentials[index]["token"])
            if outcome == "OK":
                hosts.append((index, result["session"]))
        deadline = time.monotonic() + seconds

        def work(worker):
            stats = Stats(worker + 41)
            assigned = list(range(worker, self.players, self.workers));clients = {
                index: Client(self.port, self.certificate, index) for index in assigned}
            while time.monotonic() < deadline:
                for index in assigned:
                    outcome, latency, _ = clients[index].call("sessions.find", {
                        "kind": "player", "localCount": 1, "start": 0, "limit": 32,
                        "properties": [index % 4] + [None] * 7}, credentials[index]["token"])
                    stats.add("sessions.find", outcome, latency)
                    if time.monotonic() >= deadline:
                        break
            for client in clients.values():client.close()
            return stats
        stats = self.parallel(work)
        for index, session in hosts:
            self.call(index, "sessions.leave", {"session": session}, credentials[index]["token"])
        return stats

    def host_migration(self, seconds):
        require(self.players >= 2, "host-migration needs at least two players")
        credentials = self.credentials();deadline = time.monotonic() + seconds

        def work(worker):
            stats = Stats(worker + 51)
            pairs = [(index, index + 1) for index in range(0, self.players - 1, 2)]
            for host, guest in pairs[worker::self.workers]:
                left = Client(self.port, self.certificate, host);right = Client(self.port, self.certificate, guest)
                while time.monotonic() < deadline:
                    host_token, guest_token = credentials[host]["token"], credentials[guest]["token"]
                    created = left.call("sessions.create", {
                        "kind": "player", "maxGamers": 4, "privateSlots": 0, "properties": [None] * 8,
                        "allowJoinInProgress": True, "allowHostMigration": True,
                        "participants": [host_token]}, host_token)
                    stats.add("sessions.create", created[0], created[1])
                    if created[0] == "OK":
                        session = created[2]["session"]
                        joined = right.call("sessions.join", {"session": session, "participants": [guest_token]}, guest_token)
                        stats.add("sessions.join", joined[0], joined[1])
                        if joined[0] == "OK":
                            departed = left.call("sessions.leave", {"session": session}, host_token)
                            snapshot = right.call("sessions.get", {"session": session}, guest_token)
                            ended = right.call("sessions.leave", {"session": session}, guest_token)
                            for operation, value in (("sessions.leave", departed), ("sessions.get", snapshot),
                                                     ("sessions.leave", ended)):
                                stats.add(operation, value[0], value[1])
                    if time.monotonic() >= deadline:break
                left.close();right.close()
            return stats
        return self.parallel(work)

    async def events_async(self, seconds):
        require(self.players >= 2, "events needs at least two players")
        credentials = self.credentials();stats = Stats(61)
        central = Client(self.port, self.certificate, 0)
        for index in range(1, self.players):
            add = central.call("friends.add", {"gamertag": f"Player{index}"}, credentials[0]["token"])
            accept = self.call(index, "friends.accept", {"gamertag": "Player0"}, credentials[index]["token"])
            stats.add("friends.add", add[0], add[1]);stats.add("friends.accept", accept[0], accept[1])
        central.close()
        endpoint = f"wss://localhost:{self.port}/cna/v1/events"
        sockets = []
        started = time.perf_counter()
        try:
            for index, credential in enumerate(credentials):
                before = time.perf_counter()
                socket = await websockets.connect(endpoint, ssl=ssl.create_default_context(cafile=str(self.certificate)),
                    compression=None, proxy=None, ping_interval=None, max_queue=4, close_timeout=2)
                await socket.send(json.dumps({"v": 1, "id": f"events-{index}", "game": TITLE,
                                              "token": credential["token"]}))
                welcome = json.loads(await asyncio.wait_for(socket.recv(), 10))
                outcome = welcome.get("error", "MALFORMED_MESSAGE")
                stats.add("events.connect", outcome, time.perf_counter() - before);sockets.append(socket)
            deadline = time.monotonic() + seconds;sequence = 0
            sender = Client(self.port, self.certificate, 0)
            while time.monotonic() < deadline:
                for index in range(1, self.players):
                    call = sender.call("messages.send", {"gamertags": [f"Player{index}"],
                                                         "text": f"loadlab-{sequence}"}, credentials[0]["token"])
                    stats.add("messages.send", call[0], call[1]);sequence += 1
                    if call[0] == "OK":
                        before = time.perf_counter();hint = json.loads(await asyncio.wait_for(sockets[index].recv(), 10))
                        stats.add("events.hint", "OK" if "messages" in hint.get("topics", []) else "MALFORMED_MESSAGE",
                                  time.perf_counter() - before)
                    if time.monotonic() >= deadline:break
            sender.close()
        finally:
            for socket in sockets:
                await socket.close()
        stats.extra["eventConnectionsOpened"] = len(sockets)
        stats.extra["eventRunMilliseconds"] = int((time.perf_counter() - started) * 1000)
        return stats

    async def relay_async(self, seconds, packet_shape):
        require(2 <= self.players <= 31, "relay needs 2..31 players in one directory session")
        credentials = self.credentials();stats = Stats(71)
        host = Client(self.port, self.certificate, 0)
        token = credentials[0]["token"]
        created = host.call("sessions.create", {"kind": "player", "maxGamers": self.players,
            "privateSlots": 0, "properties": [None] * 8, "allowJoinInProgress": True,
            "allowHostMigration": True, "participants": [token]}, token)
        stats.add("sessions.create", created[0], created[1]);require(created[0] == "OK", "relay session creation")
        session = created[2]["session"];machines = [created[2]["hostMachine"]];clients = [host]
        for index in range(1, self.players):
            client = Client(self.port, self.certificate, index);clients.append(client)
            joined = client.call("sessions.join", {"session": session,
                                 "participants": [credentials[index]["token"]]}, credentials[index]["token"])
            stats.add("sessions.join", joined[0], joined[1]);require(joined[0] == "OK", f"relay join {index}")
            machines.append(joined[2]["machine"])
        tickets = []
        for index, client in enumerate(clients):
            issued = client.call("sessions.relayTicket", {"session": session,
                                 "participants": [credentials[index]["token"]]}, credentials[index]["token"])
            stats.add("sessions.relayTicket", issued[0], issued[1]);require(issued[0] == "OK", "relay ticket")
            tickets.append(issued[2]["ticket"])
        endpoint = f"wss://localhost:{self.port}/cna/relay/v1";sockets = []
        context = ssl.create_default_context(cafile=str(self.certificate))
        try:
            for index, ticket in enumerate(tickets):
                before = time.perf_counter()
                socket = await websockets.connect(endpoint, ssl=context, compression=None, proxy=None,
                                                  ping_interval=None, max_queue=4, close_timeout=2)
                await socket.send(json.dumps({"v": 1, "id": f"relay-{index}", "game": TITLE, "ticket": ticket}))
                welcome = json.loads(await asyncio.wait_for(socket.recv(), 10))
                stats.add("relay.connect", welcome.get("error", "MALFORMED_MESSAGE"), time.perf_counter() - before)
                sockets.append(socket)
            sizes = {"small": 32, "large": 1024, "voice": 160, "burst": 256,
                     "idle": 32, "asymmetric": 256}
            payload = b"L" * sizes[packet_shape];deadline = time.monotonic() + seconds
            datagrams = 0;received_bytes = 0
            while time.monotonic() < deadline:
                senders = [0] if packet_shape == "asymmetric" else list(range(self.players))
                if packet_shape == "idle":
                    await asyncio.sleep(.2)
                before = time.perf_counter()
                for index in senders:
                    target = (index + 1) % self.players
                    await sockets[index].send(b"CNR\x01\x01\x00\x00\x00" + bytes.fromhex(machines[target]) + payload)
                for index in senders:
                    target = (index + 1) % self.players
                    frame = await asyncio.wait_for(sockets[target].recv(), 10)
                    expected = b"CNR\x01\x01\x00\x00\x00" + bytes.fromhex(machines[index]) + payload
                    stats.add("relay.datagram", "OK" if frame == expected else "MALFORMED_MESSAGE",
                              (time.perf_counter() - before) / len(senders))
                    datagrams += 1;received_bytes += len(frame)
                if packet_shape == "voice":await asyncio.sleep(.02)
                elif packet_shape == "small":await asyncio.sleep(.005)
            stats.extra["relayConnectionsOpened"] = len(sockets)
            stats.extra["relayDatagrams"] = datagrams
            stats.extra["relayBytesReceived"] = received_bytes
        finally:
            for socket in sockets:await socket.close()
            for index, client in reversed(list(enumerate(clients))):
                client.call("sessions.leave", {"session": session}, credentials[index]["token"]);client.close()
        return stats


def first_line(command, cwd=None):
    try:
        result = subprocess.run(command, cwd=cwd, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL, timeout=10)
        return result.stdout.splitlines()[0] if result.stdout else "unknown"
    except (OSError, subprocess.TimeoutExpired):
        return "unknown"


def build_type(build):
    cache = build / "CMakeCache.txt"
    if cache.is_file():
        for line in cache.read_text(errors="replace").splitlines():
            if line.startswith("CMAKE_BUILD_TYPE:STRING="):
                return line.split("=", 1)[1]
    return "unknown"


def cpu_description():
    path = pathlib.Path("/proc/cpuinfo")
    if path.is_file():
        for line in path.read_text(errors="replace").splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    return platform.processor() or "unknown"


def html_report(report):
    rows = []
    for scenario in report["scenarios"]:
        rows.append("<tr>" + "".join(f"<td>{html.escape(str(value))}</td>" for value in (
            scenario["scenario"], scenario["requestCount"], scenario["successCount"],
            scenario["failureCount"], scenario["throughputPerSecond"],
            scenario["latencyMs"]["p50"], scenario["latencyMs"]["p95"],
            scenario["latencyMs"]["p99"], scenario["serverResources"][-1].get("rssKiB", 0),
        )) + "</tr>")
    return """<!doctype html><meta charset=utf-8><title>CNA loadlab report</title>
<style>body{font:16px system-ui;max-width:1100px;margin:2rem auto;padding:0 1rem;color:#17202a}table{border-collapse:collapse;width:100%}th,td{border:1px solid #ccd;padding:.45rem;text-align:right}th:first-child,td:first-child{text-align:left}code{background:#eef;padding:.1rem .25rem}</style>
<h1>CNA Gamer Services loadlab</h1><p>Loopback evidence; generated <code>""" + html.escape(report["timestamp"]) + """</code>.</p>
<table><thead><tr><th>Scenario</th><th>Requests</th><th>Success</th><th>Failure</th><th>req/s</th><th>p50 ms</th><th>p95 ms</th><th>p99 ms</th><th>ending RSS KiB</th></tr></thead><tbody>""" + "".join(rows) + """</tbody></table>
<h2>Configuration</h2><pre>""" + html.escape(json.dumps(report["configuration"], indent=2)) + """</pre>
<h2>Scope</h2><p>""" + html.escape(report["scopeBoundary"]) + "</p>"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=pathlib.Path)
    parser.add_argument("--players", type=int, default=16)
    parser.add_argument("--workers", type=int, default=6)
    parser.add_argument("--seconds", type=float, default=10)
    parser.add_argument("--scenarios", default="ordinary")
    parser.add_argument("--packet-shape", choices=("small", "large", "voice", "burst", "idle", "asymmetric"), default="voice")
    parser.add_argument("--json", type=pathlib.Path)
    parser.add_argument("--html", type=pathlib.Path)
    parser.add_argument("--smoke", action="store_true")
    options = parser.parse_args()
    if options.smoke:
        options.players, options.workers, options.seconds = 4, 4, .3
        options.scenarios = ("ordinary,signin-storm,refresh-storm,session-churn,directory-pressure,"
                             "reconnect-storm,host-migration,events,relay,soak")
    require(1 <= options.workers <= MAX_WORKERS, "workers must be between 1 and 6")
    require(2 <= options.players <= 256, "players must be between 2 and 256")
    require(.1 <= options.seconds <= 86_400, "seconds must be between 0.1 and 86400")
    scenarios = [item.strip() for item in options.scenarios.split(",") if item.strip()]
    unknown = set(scenarios) - KNOWN_SCENARIOS
    require(not unknown, "unknown scenarios: " + ", ".join(sorted(unknown)))
    if "relay" in scenarios:
        require(options.players <= 31, "relay scenario supports one session of at most 31 players")

    repository = pathlib.Path(__file__).resolve().parents[2]
    lab = Lab(options.build, options.players, options.workers)
    reports = [];setup_error = "";started = time.perf_counter()
    try:
        lab.start()
        for scenario_index, name in enumerate(scenarios):
            resources = [lab.resources()];scenario_started = time.perf_counter()
            if name == "ordinary":
                stats = lab.ordinary(options.seconds)
            elif name == "soak":
                stats = lab.ordinary(options.seconds, resource_samples=resources)
            elif name == "reconnect-storm":
                stats = lab.ordinary(options.seconds, reconnect=True)
            elif name == "signin-storm":
                stats = lab.signins(options.seconds)
            elif name == "refresh-storm":
                stats = lab.refreshes(options.seconds)
            elif name == "session-churn":
                stats = lab.session_churn(options.seconds)
            elif name == "directory-pressure":
                stats = lab.directory_pressure(options.seconds)
            elif name == "host-migration":
                stats = lab.host_migration(options.seconds)
            elif name == "events":
                stats = asyncio.run(lab.events_async(options.seconds))
            elif name == "relay":
                stats = asyncio.run(lab.relay_async(options.seconds, options.packet_shape))
            elapsed = time.perf_counter() - scenario_started
            resources.append(lab.resources())
            reports.append(summarize(name, stats, elapsed, resources, {
                "packetShape": options.packet_shape if name == "relay" else None,
                "loopback": True,
            }))
            print(f"{name:20} {stats.requests:7} requests {stats.successes:7} OK "
                  f"p95={percentile(stats.samples, .95):8.2f} ms", flush=True)
            require(lab.process.poll() is None, f"server exited during {name}")
            if scenario_index + 1 < len(scenarios):
                lab.restart()
    except Exception as error:
        setup_error = f"{type(error).__name__}: {error}"
    finally:
        lab.stop()

    report = {
        "schemaVersion": 1,
        "tool": "cna-gamer-services-loadlab",
        "toolVersion": 1,
        "timestamp": dt.datetime.now(dt.timezone.utc).isoformat(),
        "gitCommit": first_line(["git", "rev-parse", "HEAD"], repository),
        "gitDirty": bool(first_line(["git", "status", "--porcelain"], repository)),
        "compiler": first_line(["c++", "--version"]),
        "buildType": build_type(options.build.resolve()),
        "os": platform.platform(),
        "cpu": cpu_description(),
        "configuration": {"players": options.players, "workers": options.workers,
                          "secondsPerScenario": options.seconds, "scenarios": scenarios,
                          "packetShape": options.packet_shape, "transport": "verified loopback TLS/WSS"},
        "durationSeconds": round(time.perf_counter() - started, 3),
        "setupError": setup_error,
        "scenarios": reports,
        "scopeBoundary": "Closed-loop Linux loopback load evidence; not LAN or public-Internet latency/capacity.",
    }
    if options.json:
        options.json.parent.mkdir(parents=True, exist_ok=True)
        options.json.write_text(json.dumps(report, indent=2) + "\n")
    if options.html:
        options.html.parent.mkdir(parents=True, exist_ok=True)
        options.html.write_text(html_report(report))
    failures = []
    if setup_error:
        failures.append(setup_error)
    if options.smoke:
        for scenario in reports:
            bad = {key: value for key, value in scenario["failuresByError"].items()
                   if key == "INTERNAL_ERROR" or key.startswith("TRANSPORT_")}
            if bad or scenario["successCount"] == 0:
                failures.append(f"{scenario['scenario']}: {bad or 'no successful work'}")
    print(f"Loadlab completed {len(reports)}/{len(scenarios)} scenarios in {report['durationSeconds']}s")
    print(report["scopeBoundary"])
    for failure in failures:
        print("FAIL", failure)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
