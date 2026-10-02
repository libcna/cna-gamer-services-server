# SPDX-License-Identifier: MIT
"""Real TLS cache policy and repeated-read authorization transitions, without client mocks."""
import hashlib, json, os, pathlib, selectors, ssl, subprocess, sys, tempfile, urllib.error, urllib.request
from transport_e2e import original_png, request


def main():
    build = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="service-cache-policy-") as temp:
        root = pathlib.Path(temp)
        db, cert, key = root/"state.sqlite3", root/"cert.pem", root/"key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1", "-subj", "/CN=localhost",
                        "-addext", "subjectAltName=DNS:localhost", "-keyout", str(key), "-out", str(cert)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        admin = str(build/"cna-gamer-services-admin")
        def provision(*args, text=None):
            return subprocess.check_output([admin, str(db), *args], input=text, text=True).strip()
        provision("title", "one", "One")
        provision("title", "pictures", "Picture import")
        for name in ("alice", "bob"):
            provision("user", name, name.title(), text=name+"-password\n")
        png = original_png(); image = root/"picture.png"; image.write_bytes(png)
        digest = provision("asset", "pictures", "image/png", str(image))
        assert digest == hashlib.sha256(png).hexdigest()
        provision("picture", "alice", digest)
        server = subprocess.Popen([str(build/"cna-gamer-services-server"), "--database", str(db), "--listen", "127.0.0.1", "--port", "0",
                                   "--cert", str(cert), "--key", str(key)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            with selectors.DefaultSelector() as ready:
                ready.register(server.stdout, selectors.EVENT_READ)
                assert ready.select(15), "server startup timeout"
            line = server.stdout.readline(); assert "listening" in line, "server startup"
            url = "https://localhost:"+line.strip().rsplit(":", 1)[1]+"/cna/v1"
            def call(op, args=None, token=None):
                return request(url, str(cert), "one", op, args, token)
            logins = [call("auth.login", {"username": name, "password": name+"-password"}) for name in ("alice", "bob")]
            assert all(login["error"] == "OK" for login in logins)
            alice, bob = [login["result"]["token"] for login in logins]
            alice_id = logins[0]["result"]["identity"]["userId"]
            context = ssl.create_default_context(cafile=str(cert))
            opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), urllib.request.HTTPSHandler(context=context))
            def binary(token, status):
                req = urllib.request.Request(url+"/files/"+digest, headers={"Authorization": "Bearer "+token, "X-CNA-Game": "one"})
                try:
                    response = opener.open(req, timeout=10)
                except urllib.error.HTTPError as error:
                    response = error
                with response:
                    assert response.status == status, (response.status, status)
                    assert response.headers["Cache-Control"] == "no-store", "authorization-gated responses must not advertise reusable HTTP grants"
                    content = response.read()
                    if status == 200: assert content == png
                    else: assert content != png and json.loads(content)["error"] != "OK"
            # Reuse the same read request ID: reads must re-evaluate, rather than replay old authorization.
            def profile(expected):
                envelope = {"v": 1, "id": "repeat-profile", "game": "one", "op": "profile.get", "args": {"gamertag": "Alice"}, "token": bob}
                with opener.open(urllib.request.Request(url, data=json.dumps(envelope).encode(), headers={"Content-Type": "application/json"}), timeout=10) as response:
                    assert json.load(response)["error"] == expected
                binary(bob, 200 if expected == "OK" else 403 if expected == "NOT_AUTHORIZED" else 401)
                assert call("assets.read", {"hash": digest, "offset": 0, "length": 1}, bob)["error"] == expected
            profile("OK")
            provision("privilege", "bob", "profileViewing", "friends")
            profile("NOT_AUTHORIZED")
            assert call("friends.add", {"gamertag": "Alice"}, bob)["error"] == "OK"
            profile("NOT_AUTHORIZED")
            assert call("friends.accept", {"gamertag": "Bob"}, alice)["error"] == "OK"
            assert call("presence.set", {"mode": 1, "text": "fresh presence"}, alice)["error"] == "OK"
            assert call("friends.list", token=bob)["result"]["friends"][0]["presenceText"] == "fresh presence"
            profile("OK")
            assert call("friends.remove", {"gamertag": "Alice"}, bob)["error"] == "OK"
            profile("NOT_AUTHORIZED")
            assert call("friends.list", token=bob)["result"]["friends"] == []
            provision("privilege", "bob", "profileViewing", "everyone")
            profile("OK")
            assert call("privacy.block", {"gamertag": "Bob"}, alice)["error"] == "OK"
            profile("NOT_AUTHORIZED")
            assert call("privacy.list", token=alice)["result"]["blocked"] == ["Bob"]
            # Avatars are explicitly public to signed-in accounts, independent of profile policy.
            assert call("avatars.get", {"userIds": [alice_id]}, bob)["error"] == "OK"
            assert call("privacy.unblock", {"gamertag": "Bob"}, alice)["error"] == "OK"
            profile("OK")
            assert call("privacy.list", token=alice)["result"]["blocked"] == []
            assert call("auth.logout", token=bob)["error"] == "OK"
            profile("UNAUTHENTICATED")
            for op, args in (("friends.list", {}), ("privacy.list", {}), ("avatars.get", {"userIds": [alice_id]})):
                assert call(op, args, bob)["error"] == "UNAUTHENTICATED"
            print("TLS cache policy and profile/picture/friend/block/presence/avatar/logout transitions passed")
        finally:
            server.terminate(); server.wait(timeout=10)
            server.stdout.close(); server.stderr.close()


if __name__ == "__main__":
    main()
