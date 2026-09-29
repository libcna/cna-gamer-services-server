# SPDX-License-Identifier: MIT
"""Concurrent-player benchmark for the CNA service over its real TLS listener.

Starts one server on a scratch database inside the build directory, provisions accounts and
drives it from independent client processes. Every client binds its own loopback source address
(127.x.y.z), because the service limits sign-in per peer address and real players do not share
one.

Scenarios, each a fixed wall-clock window:
  steady   N signed-in players in a closed loop over the ordinary authenticated mix
  logins   the same players while L more clients sign in continuously (scrypt per login)
  idle     one address holds C connections that send nothing, during the steady mix
  replay   the steady mix after the title's request-ID table was filled close to its cap
  descriptors  a server limited to 48 descriptors facing 80 idle connections must keep serving

It reports throughput, latency percentiles per operation and every error code. It asserts nothing
about speed: --smoke runs a short pass that only requires no INTERNAL_ERROR and no transport failure.
"""
import argparse, http.client, json, multiprocessing, os, pathlib, random, socket, sqlite3, ssl, subprocess, sys, tempfile, time, uuid

TITLE = "bench"
PASSWORD = "bench-password"
MIX = (("auth.ping", 30), ("friends.list", 15), ("presence.set", 10), ("profile.get", 10),
       ("leaderboards.read", 10), ("sessions.find", 10), ("achievements.list", 5), ("presence.status", 5),
       ("sessions.touch", 5))


def source(index):
    return f"127.{1 + index // 62500}.{index // 250 % 250 + 1}.{index % 250 + 1}"


class Client:
    """One player's connection; reuses it when the server keeps it open."""

    def __init__(self, port, ca, address, keep_alive):
        self.port, self.address, self.keep_alive = port, address, keep_alive
        self.context = ssl.create_default_context(cafile=ca)
        self.connection = None

    def call(self, op, args=None, token=None):
        envelope = {"v": 1, "id": uuid.uuid4().hex, "game": TITLE, "op": op, "args": args or {}}
        if token: envelope["token"] = token
        body = json.dumps(envelope).encode()
        started = time.perf_counter()
        try:
            if self.connection is None:
                self.connection = http.client.HTTPSConnection("127.0.0.1", self.port, context=self.context,
                                                              timeout=30, source_address=(self.address, 0))
            headers = {"Content-Type": "application/json"}
            if not self.keep_alive: headers["Connection"] = "close"
            self.connection.request("POST", "/cna/v1", body, headers)
            response = self.connection.getresponse()
            data = json.loads(response.read())
            if response.will_close: self.close()
            return data["error"], time.perf_counter() - started, data.get("result")
        except Exception as error:
            self.close()
            return "TRANSPORT_" + type(error).__name__, time.perf_counter() - started, None

    def close(self):
        if self.connection is not None:
            self.connection.close()
            self.connection = None


def player(index, port, ca, keep_alive, start, stop, players, results):
    client = Client(port, ca, source(index), keep_alive)
    random.seed(index)
    error, _, login = client.call("auth.login", {"username": f"player{index}", "password": PASSWORD})
    if error != "OK":
        results.put([("setup", error, 0.0)]); return
    token = login["token"]
    session = None
    if index % 8 == 0:
        created = client.call("sessions.create", {"kind": "player", "maxGamers": 8, "privateSlots": 0,
                                                   "properties": [index % 4] + [None] * 7, "allowJoinInProgress": True,
                                                   "participants": [token]}, token)
        if created[0] == "OK": session = created[2]["session"]
    operations = [op for op, weight in MIX for _ in range(weight)]
    samples = []
    while time.time() < start: time.sleep(0.005)
    while time.time() < stop:
        op = random.choice(operations)
        if op == "sessions.touch" and session is None: op = "auth.ping"
        args = {"auth.ping": {}, "friends.list": {}, "achievements.list": {},
                "presence.set": {"mode": random.randrange(256), "text": "Benchmark"},
                "presence.status": {"status": random.choice(("online", "away", "busy"))},
                "profile.get": {"gamertag": f"Player{(index + 1) % players}"},
                "leaderboards.read": {"key": "Score", "mode": 0, "start": random.randrange(64), "size": 10},
                "sessions.find": {"kind": "player", "localCount": 1, "start": 0, "limit": 16, "properties": [None] * 8},
                "sessions.touch": {"session": session}}[op]
        error, seconds, _ = client.call(op, args, token)
        samples.append((op, error, seconds))
    if session is not None: client.call("sessions.leave", {"session": session}, token)
    client.close()
    results.put(samples)


def signer(index, port, ca, players, start, stop, results):
    # A fresh source address per sign-in keeps each peer under the ten-per-minute limit.
    random.seed(1000 + index)
    samples, attempt = [], 0
    while time.time() < start: time.sleep(0.005)
    while time.time() < stop:
        client = Client(port, ca, source(20000 + index * 5000 + attempt), False)
        attempt += 1
        error, seconds, _ = client.call("auth.login", {"username": f"player{random.randrange(players)}", "password": PASSWORD})
        samples.append(("auth.login", error, seconds))
    results.put(samples)


def idler(port, count, stop, results):
    # Connections that never send a request, all from one address and reopened as the server drops
    # them: one slow or hostile host holding connection slots.
    import selectors
    def connect(index):
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=5, source_address=(source(40000), 0))
        except OSError:
            return None
    held = [connect(index) for index in range(count)]
    reopened = 0
    while time.time() < stop:
        with selectors.DefaultSelector() as selector:
            for index, s in enumerate(held):
                if s is not None: selector.register(s, selectors.EVENT_READ, index)
            for key, _ in selector.select(0.2):
                try: closed = key.fileobj.recv(1) == b""
                except OSError: closed = True
                if closed:
                    key.fileobj.close(); held[key.data] = connect(key.data); reopened += 1
    results.put([("idle.held", "OK", float(count)), ("idle.reopened", "OK", float(reopened))])
    for s in held:
        if s is not None: s.close()


def descriptors(build, db, ca, key):
    """Exhausts the server's file descriptors with idle connections, then asks it for service."""
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0)); port = probe.getsockname()[1]
    process = subprocess.Popen(["prlimit", "--nofile=48:48", str(build / "cna-gamer-services-server"), "--database", str(db),
                                "--listen", "127.0.0.1", "--port", str(port), "--cert", str(ca), "--key", str(key)],
                               stdout=subprocess.PIPE, text=True)
    try:
        assert "listening" in process.stdout.readline()
        held = [socket.create_connection(("127.0.0.1", port), timeout=5, source_address=(source(45000 + index), 0)) for index in range(80)]
        time.sleep(1.5)
        survived = process.poll() is None
        for s in held: s.close()
        time.sleep(1.0)
        error = Client(port, str(ca), source(46000), False).call("hello")[0]
        return {"scenario": "descriptors", "serverSurvived": survived and process.poll() is None, "helloAfter": error}
    finally:
        process.terminate(); process.wait(timeout=10)


def percentile(values, share):
    if not values: return 0.0
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(share * len(ordered)))]


def summarize(name, samples, seconds):
    rows, errors = {}, {}
    for op, error, value in samples:
        if op in ("setup", "idle.held", "idle.reopened"): continue
        rows.setdefault(op, []).append(value)
        errors[error] = errors.get(error, 0) + 1
    total = sum(len(v) for v in rows.values())
    report = {"scenario": name, "seconds": seconds, "requests": total, "throughput": round(total / seconds, 1),
              "errors": errors, "operations": {}}
    held = [value for op, _, value in samples if op == "idle.held"]
    if held: report["idleConnectionsHeld"] = int(held[0])
    reopened = [value for op, _, value in samples if op == "idle.reopened"]
    if reopened: report["idleConnectionsReopened"] = int(reopened[0])
    setup = [error for op, error, _ in samples if op == "setup"]
    if setup: report["setupFailures"] = setup
    everything = [value for values in rows.values() for value in values]
    report["latencyMs"] = {"p50": round(1000 * percentile(everything, .5), 2), "p95": round(1000 * percentile(everything, .95), 2),
                           "p99": round(1000 * percentile(everything, .99), 2), "max": round(1000 * max(everything, default=0), 2)}
    for op, values in sorted(rows.items()):
        report["operations"][op] = {"count": len(values), "p50": round(1000 * percentile(values, .5), 2),
                                    "p95": round(1000 * percentile(values, .95), 2), "p99": round(1000 * percentile(values, .99), 2)}
    return report


def run(name, port, ca, options, signers=0, idle=0):
    results = multiprocessing.Queue()
    start = time.time() + (6.0 if idle else 2.0)
    stop = start + options.seconds
    processes = []
    if idle:
        processes.append(multiprocessing.Process(target=idler, args=(port, idle, stop, results)))
    for index in range(options.players):
        processes.append(multiprocessing.Process(target=player, args=(index, port, ca, options.keep_alive, start, stop, options.players, results)))
    for index in range(signers):
        processes.append(multiprocessing.Process(target=signer, args=(index, port, ca, options.players, start, stop, results)))
    for process in processes: process.start()
    samples = []
    for _ in processes: samples.extend(results.get(timeout=options.seconds + 120))
    for process in processes: process.join()
    return summarize(name, samples, options.seconds)


def provision(build, root, players):
    ca, key, db = root / "cert.pem", root / "key.pem", root / "service.sqlite3"
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1", "-subj", "/CN=localhost",
                    "-addext", "subjectAltName=IP:127.0.0.1", "-keyout", str(key), "-out", str(ca)],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    admin = str(build / "cna-gamer-services-admin")
    subprocess.run([admin, str(db), "title", TITLE, "Benchmark"], check=True, stdout=subprocess.DEVNULL)
    board = {"key": "Score", "mode": 0, "ascending": False, "aggregation": "best", "arbitrated": False, "columns": {"Rounds": "int32"}}
    subprocess.run([admin, str(db), "leaderboard", TITLE], input=json.dumps(board), text=True, check=True)
    def user(index):
        return subprocess.Popen([admin, str(db), "user", f"player{index}", f"Player{index}"], stdin=subprocess.PIPE,
                                stdout=subprocess.DEVNULL, text=True)
    pending = []
    for index in range(players):
        process = user(index); process.stdin.write(PASSWORD + "\n"); process.stdin.close(); pending.append(process)
        if len(pending) >= 8:
            assert pending.pop(0).wait() == 0
    for process in pending: assert process.wait() == 0
    for index in range(players):
        entry = {"key": "Score", "mode": 0, "gamertag": f"Player{index}", "rating": index * 7 % 1000,
                 "columns": {"Rounds": {"type": "int32", "value": index}}}
        subprocess.run([admin, str(db), "seed-leaderboard", TITLE], input=json.dumps(entry), text=True, check=True)
    # Mutual friendships: every player befriends the next eight, straight in the store.
    with sqlite3.connect(db) as connection:
        ids = dict(connection.execute("SELECT username,id FROM users"))
        for index in range(players):
            for step in range(1, 9):
                a, b = ids[f"player{index}"], ids[f"player{(index + step) % players}"]
                if a != b: connection.executemany("INSERT OR IGNORE INTO friends(user_id,friend_id) VALUES(?,?)", [(a, b), (b, a)])
    return ca, key, db


def fill_request_ids(db, target):
    # Tops the title's request-ID table up to the target, as if the day had already been busy.
    with sqlite3.connect(db) as connection:
        now = int(time.time())
        present = connection.execute("SELECT COUNT(*) FROM request_ids WHERE game_id=?", (TITLE,)).fetchone()[0]
        connection.executemany("INSERT INTO request_ids(game_id,id,created) VALUES(?,?,?)",
                               ((TITLE, f"fill-{index}", now - 3600) for index in range(max(0, target - present))))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("build")
    parser.add_argument("--players", type=int, default=64)
    parser.add_argument("--signers", type=int, default=8)
    parser.add_argument("--idle", type=int, default=160)
    parser.add_argument("--seconds", type=float, default=15)
    parser.add_argument("--replay-fill", type=int, default=90000)
    parser.add_argument("--scenarios", default="steady,logins,idle,replay,descriptors")
    parser.add_argument("--keep-alive", action="store_true", help="reuse connections when the server allows it")
    parser.add_argument("--json")
    parser.add_argument("--smoke", action="store_true")
    options = parser.parse_args()
    if options.smoke:
        options.players, options.signers, options.idle, options.seconds, options.replay_fill = 8, 2, 0, 2, 0
        options.scenarios = "steady,logins,descriptors"
    build = pathlib.Path(options.build).resolve()
    reports = []
    with tempfile.TemporaryDirectory(prefix="service-benchmark-", dir=build) as temp:
        root = pathlib.Path(temp); os.chmod(root, 0o700)
        ca, key, db = provision(build, root, options.players)
        def serve():
            with socket.socket() as probe:
                probe.bind(("127.0.0.1", 0)); port = probe.getsockname()[1]
            process = subprocess.Popen([str(build / "cna-gamer-services-server"), "--database", str(db), "--listen", "127.0.0.1",
                                        "--port", str(port), "--cert", str(ca), "--key", str(key)], stdout=subprocess.PIPE, text=True)
            assert "listening" in process.stdout.readline()
            return process, port
        for scenario in options.scenarios.split(","):
            if scenario == "descriptors":
                report = descriptors(build, db, ca, key)
                reports.append(report); print(json.dumps(report), flush=True)
                continue
            if scenario == "replay": fill_request_ids(db, options.replay_fill)
            process, port = serve()
            try:
                if scenario == "steady": report = run("steady", port, str(ca), options)
                elif scenario == "logins": report = run("logins", port, str(ca), options, signers=options.signers)
                elif scenario == "idle": report = run("idle", port, str(ca), options, idle=options.idle)
                elif scenario == "replay": report = run("replay", port, str(ca), options)
                else: raise SystemExit(f"unknown scenario {scenario}")
            finally:
                process.terminate(); process.wait(timeout=10)
            with sqlite3.connect(db) as connection:
                report["requestIds"] = connection.execute("SELECT COUNT(*) FROM request_ids").fetchone()[0]
            reports.append(report)
            print(json.dumps(report), flush=True)
    if options.json: pathlib.Path(options.json).write_text(json.dumps(reports, indent=1))
    if options.smoke:
        for report in reports:
            if report["scenario"] == "descriptors":
                assert report["serverSurvived"] and report["helloAfter"] == "OK", report
                continue
            bad = {code: n for code, n in report["errors"].items() if code == "INTERNAL_ERROR" or code.startswith("TRANSPORT_")}
            assert not bad and "setupFailures" not in report, f"{report['scenario']}: {bad or report.get('setupFailures')}"
            assert report["requests"] > 0, report["scenario"]


if __name__ == "__main__":
    main()
