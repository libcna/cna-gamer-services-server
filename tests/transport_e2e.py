# SPDX-License-Identifier: MS-PL
"""Real TLS listener, independent client processes and restart persistence."""
import json, os, pathlib, ssl, subprocess, sys, tempfile, urllib.request, struct, zlib, hashlib, selectors


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
    assert entries[0]["earnedTicks"] == 0 or action == "award"
    if action == "award":
        assert request(url, ca, "one", "achievements.award", {"key": "first"}, token)["error"] == "OK"
    page=request(url,ca,"one","leaderboards.read",{"key":"BestScoreLifeTime","mode":0,"start":1,"size":1},token)["result"]
    assert page["total"]==2 and page["entries"][0]["gamertag"]=="Alice"
    pathlib.Path(state).write_text(json.dumps({"token": token}))
    os.chmod(state, 0o600)


def line_with_timeout(stream):
    with selectors.DefaultSelector() as selector:
        selector.register(stream, selectors.EVENT_READ)
        assert selector.select(15), "client coordination timeout"
        return stream.readline()


def original_png():
    def chunk(kind, data):
        return struct.pack(">I", len(data))+kind+data+struct.pack(">I", zlib.crc32(kind+data))
    # Original procedural two-color gamer picture, not a downloaded asset.
    return b"\x89PNG\r\n\x1a\n"+chunk(b"IHDR", struct.pack(">2I5B", 2, 2, 8, 6, 0, 0, 0))+chunk(b"IDAT", zlib.compress(b"\x00"+b"\x33\x99\xee\xff"*2+b"\x00"+b"\xff\xcc\x33\xff"*2))+chunk(b"IEND", b"")


def main():
    build = pathlib.Path(sys.argv[1]).resolve()
    client = os.environ.get("CNA_SERVICE_CLIENT_HARNESS")
    c_client = os.environ.get("CNA_SERVICE_C_API_HARNESS")
    if client:
        assert pathlib.Path(client).is_file(), "missing CNA client harness"
    with tempfile.TemporaryDirectory(prefix="service-e2e-", dir=build) as temp:
        root = pathlib.Path(temp); os.chmod(root, 0o700)
        ca, key, db = root/"test-cert.pem", root/"test-key.pem", root/"service.sqlite3"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1", "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost", "-keyout", str(key), "-out", str(ca)], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        admin = str(build/"cna-gamer-services-admin")
        image = original_png(); image_path=root/"original.png"; image_path.write_bytes(image)
        image_hash=hashlib.sha256(image).hexdigest()
        for game in ("one", "two"):
            subprocess.run([admin, str(db), "title", game, game], check=True, stdout=subprocess.DEVNULL)
            imported=subprocess.check_output([admin,str(db),"asset",game,"image/png",str(image_path)],text=True).strip()
            assert imported==image_hash
            definition = {"picture": image_hash, "key": "first", "name": "First", "description": "Test award", "howToEarn": "Play", "score": 10}
            subprocess.run([admin, str(db), "achievement", game], input=json.dumps(definition), text=True, check=True)
        for user in ("alice", "bob"):
            subprocess.run([admin, str(db), "user", user, user.title()], input=user+"-password\n", text=True, check=True, stdout=subprocess.DEVNULL)
            subprocess.run([admin,str(db),"picture",user,image_hash],check=True)
        for game in ("one","two"):
            definition={"key":"BestScoreLifeTime","mode":0,"ascending":False,"aggregation":"best","arbitrated":False,"columns":{"Rounds":"int32","Label":"string","Total":"int64","Scale":"single","Precision":"double","When":"datetime","Duration":"timespan","Outcome":"outcome"}}
            subprocess.run([admin,str(db),"leaderboard",game],input=json.dumps(definition),text=True,check=True)
        for user,rating in (("Alice",100),("Bob",200)):
            entry={"key":"BestScoreLifeTime","mode":0,"gamertag":user,"rating":rating,"columns":{"Rounds":{"type":"int32","value":3},"Label":{"type":"string","value":"Original"},"Total":{"type":"int64","value":9223372036854775807},"Scale":{"type":"single","value":1.25},"Precision":{"type":"double","value":2.5},"When":{"type":"datetime","value":123456},"Duration":{"type":"timespan","value":-1000},"Outcome":{"type":"outcome","value":1}}}
            subprocess.run([admin,str(db),"seed-leaderboard","one"],input=json.dumps(entry),text=True,check=True)
        server = None
        def run_cna(url, username, state, action, game="one", password=None, trust=None):
            environment = os.environ.copy()
            environment.update(CNA_GAMER_SERVICES_ENDPOINT=url, CNA_GAME_ID=game,
                               CNA_GAMER_SERVICES_CA_BUNDLE=str(ca) if trust is None else trust,
                               CNA_GAMER_SERVICES_INSECURE_LOOPBACK="0", CNA_GAMER_SERVICES_CACHE_DIR=str(root/"cache"))
            environment.pop("DISPLAY", None); environment.pop("WAYLAND_DISPLAY", None)
            subprocess.run([client, "--real", username, state, action],
                           input=(password or username+"-password")+"\n", text=True,
                           env=environment, check=True, timeout=45)
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
            if client:
                run_cna(url, "alice", "none", "reject", password="wrong-password")
                run_cna(url, "alice", "none", "reject", trust="")
                run_cna(url.replace("localhost", "127.0.0.1"), "alice", "none", "reject")
                run_cna(url, "alice", "none", "award")
                cache_mtime=(root/"cache"/image_hash).stat().st_mtime_ns
                run_cna(url, "bob", "none", "read")
                assert (root/"cache"/image_hash).stat().st_mtime_ns==cache_mtime,"warm cross-process cache must avoid rewriting"
                run_cna(url, "alice", "none", "read", game="two")
                run_cna(url, "alice", "earned", "read")
            for user, action in (("alice", "award"), ("bob", "read")):
                subprocess.run([sys.executable, __file__, "--worker", url, str(ca), user, str(root/(user+".json")), action], input=user+"-password\n", text=True, check=True)
            token = json.loads((root/"alice.json").read_text())["token"]
            assert request(url, str(ca), "two", "achievements.list", token=token)["error"] == "UNAUTHENTICATED"
            stop(server); server, url = start()
            if client:
                run_cna(url, "alice", "earned", "read")
                run_cna(url, "bob", "none", "read")
                cached=root/"cache"/image_hash;assert cached.read_bytes()==image
                cached.write_bytes(b"corrupt-cache")
                run_cna(url,"alice","earned","read")
                assert cached.read_bytes()==image, "corrupt immutable cache must be replaced"
                run_cna(url, "alice", "earned", "request")
                environment = os.environ.copy()
                environment.update(CNA_GAMER_SERVICES_ENDPOINT=url, CNA_GAME_ID="one", CNA_GAMER_SERVICES_CA_BUNDLE=str(ca), CNA_GAMER_SERVICES_INSECURE_LOOPBACK="0", CNA_GAMER_SERVICES_CACHE_DIR=str(root/"cache"))
                environment.pop("DISPLAY", None); environment.pop("WAYLAND_DISPLAY", None)
                for user, action, ready in (("bob", "presence-wait", "READY_PRESENCE"), ("alice", "revoke-wait", "READY_REVOKE")):
                    process = subprocess.Popen([client, "--real", user, "none" if user=="bob" else "earned", action], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=environment)
                    try:
                        process.stdin.write(user+"-password\n");process.stdin.flush()
                        assert line_with_timeout(process.stdout).strip()==ready, "CNA coordination failed"
                        if user=="bob": run_cna(url, "alice", "earned", "friends")
                        else: subprocess.run([admin, str(db), "revoke-user", "alice"], check=True)
                        process.stdin.write("continue\n");process.stdin.flush()
                        output, _ = process.communicate(timeout=30)
                        assert process.returncode==0, "CNA coordinated client failed"
                        print(output.strip())
                    finally:
                        if process.poll() is None: process.kill();process.wait()
            if client:
                token = request(url, str(ca), "one", "auth.login", {"username":"alice", "password":"alice-password"})["result"]["token"]
            if c_client:
                for user,state in (("alice","earned"),("bob","none")):
                    environment=os.environ.copy()
                    environment.update(CNA_GAMER_SERVICES_ENDPOINT=url,CNA_GAME_ID="one",CNA_GAMER_SERVICES_CA_BUNDLE=str(ca),CNA_GAMER_SERVICES_CACHE_DIR=str(root/"cache"),CNA_GAMER_SERVICES_INSECURE_LOOPBACK="0")
                    environment.pop("DISPLAY",None);environment["WAYLAND_DISPLAY"]=""
                    subprocess.run([c_client,user,state],input=user+"-password\n",text=True,env=environment,check=True,timeout=30)
            if client:
                stop(server);server,url=start()
                run_cna(url,"alice","earned","leaderboard-write")
                stop(server);server,url=start()
                run_cna(url,"alice","earned","leaderboard-after")
                run_cna(url,"bob","none","leaderboard-after")
            earned = request(url, str(ca), "one", "achievements.list", token=token)["result"]["achievements"]
            assert earned[0]["earnedTicks"] > 0
            assert request(url, str(ca), "one", "auth.logout", token=token)["error"] == "OK"
            assert request(url, str(ca), "one", "achievements.list", token=token)["error"] == "UNAUTHENTICATED"
        finally:
            if server: stop(server)
        refused = subprocess.run([str(build/"cna-gamer-services-server"), "--database", str(db), "--listen", "0.0.0.0", "--insecure-loopback"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        assert refused.returncode != 0
        if client: print("CNA E2E passed: Guide authentication and masking, rejected password/CA/hostname, two users/processes/titles, async completion, idempotent award, client/server restart persistence, lookup/profile, sign-out, mutual friends/rich presence, admin revocation, picture streams/cache and corrupt-cache recovery, remote leaderboard paging/centering/restricted reads and typed columns, LocalWithLeaderboards/EndGame final writes and restart persistence")
        print("TLS E2E passed: trusted TLS, untrusted CA, wrong hostname, two client processes, title isolation, restart persistence, revocation, insecure public bind refusal")


if __name__ == "__main__":
    if len(sys.argv)>1 and sys.argv[1]=="--worker": worker()
    else: main()
