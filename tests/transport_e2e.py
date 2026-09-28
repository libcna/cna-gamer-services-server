# SPDX-License-Identifier: MS-PL
"""Real TLS listener, independent client processes and restart persistence."""
import json, os, pathlib, ssl, subprocess, sys, tempfile, urllib.request


def request(url, ca, game, op, args=None, token=None):
    import uuid
    envelope = {"v": 1, "id": uuid.uuid4().hex, "game": game, "op": op, "args": args or {}}
    if token: envelope["token"] = token
    data = json.dumps(envelope).encode()
    context = ssl.create_default_context(cafile=ca) if ca else ssl.create_default_context()
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), urllib.request.HTTPSHandler(context=context))
    with opener.open(urllib.request.Request(url, data=data, headers={"Content-Type": "application/json"}), timeout=10) as response:
        return json.load(response)


def worker():
    url, ca, username, state, action = sys.argv[2:]
    login = request(url, ca, "one", "auth.login", {"username": username, "password": sys.stdin.readline().strip()})
    assert login["error"] == "OK"
    token = login["result"]["token"]
    entries = request(url, ca, "one", "achievements.list", token=token)["result"]["achievements"]
    assert entries[0]["earnedTicks"] == 0
    if action == "award":
        assert request(url, ca, "one", "achievements.award", {"key": "first"}, token)["error"] == "OK"
    pathlib.Path(state).write_text(json.dumps({"token": token}))
    os.chmod(state, 0o600)


def main():
    build = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="service-e2e-", dir=build) as temp:
        root = pathlib.Path(temp); os.chmod(root, 0o700)
        ca, key, db = root/"test-cert.pem", root/"test-key.pem", root/"service.sqlite3"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1", "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost", "-keyout", str(key), "-out", str(ca)], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        admin = str(build/"cna-gamer-services-admin")
        for game in ("one", "two"):
            subprocess.run([admin, str(db), "title", game, game], check=True, stdout=subprocess.DEVNULL)
            definition = {"key": "first", "name": "First", "description": "Test award", "howToEarn": "Play", "score": 10}
            subprocess.run([admin, str(db), "achievement", game], input=json.dumps(definition), text=True, check=True)
        for user in ("alice", "bob"):
            subprocess.run([admin, str(db), "user", user, user.title()], input=user+"-password\n", text=True, check=True, stdout=subprocess.DEVNULL)
        server = None
        def start():
            process = subprocess.Popen([str(build/"cna-gamer-services-server"), "--database", str(db), "--listen", "127.0.0.1", "--port", "0", "--cert", str(ca), "--key", str(key)], stdout=subprocess.PIPE, text=True)
            line = process.stdout.readline(); assert "listening" in line
            return process, "https://localhost:"+line.strip().rsplit(":", 1)[1]+"/cna/v1"
        def stop(process):
            process.terminate(); process.wait(timeout=10); process.stdout.close()
        try:
            server, url = start()
            assert request(url, str(ca), "one", "hello")["error"] == "OK"
            for endpoint, trust in ((url, None), (url.replace("localhost", "127.0.0.1"), str(ca))):
                rejected = False
                try: request(endpoint, trust, "one", "hello")
                except (urllib.error.URLError, ssl.SSLError): rejected = True
                assert rejected, "TLS trust/hostname must fail closed"
            for user, action in (("alice", "award"), ("bob", "read")):
                subprocess.run([sys.executable, __file__, "--worker", url, str(ca), user, str(root/(user+".json")), action], input=user+"-password\n", text=True, check=True)
            token = json.loads((root/"alice.json").read_text())["token"]
            assert request(url, str(ca), "two", "achievements.list", token=token)["error"] == "UNAUTHENTICATED"
            stop(server); server, url = start()
            earned = request(url, str(ca), "one", "achievements.list", token=token)["result"]["achievements"]
            assert earned[0]["earnedTicks"] > 0
            assert request(url, str(ca), "one", "auth.logout", token=token)["error"] == "OK"
            assert request(url, str(ca), "one", "achievements.list", token=token)["error"] == "UNAUTHENTICATED"
        finally:
            if server: stop(server)
        refused = subprocess.run([str(build/"cna-gamer-services-server"), "--database", str(db), "--listen", "0.0.0.0", "--insecure-loopback"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        assert refused.returncode != 0
        print("TLS E2E passed: trusted TLS, untrusted CA, wrong hostname, two client processes, title isolation, restart persistence, revocation, insecure public bind refusal")


if __name__ == "__main__":
    if len(sys.argv)>1 and sys.argv[1]=="--worker": worker()
    else: main()
