# SPDX-License-Identifier: MIT
"""A CNA process reads and renders service avatars with the standard XNA API.

The service imports CNA's embedded catalog (v1) and a newer v2 with one extra hat. Alice's avatar
wears that hat, so her CNA client must download it by hash from the service, verify and cache it,
and still render her avatar without substituting anything.
"""
import json, os, pathlib, shutil, subprocess, sys, tempfile, zlib


def description(catalog, items, body=1, height=1760, build=140):
    d = bytearray(1021)
    d[0] = 1; d[1:4] = b"CNA"; d[4] = body
    d[5:7] = height.to_bytes(2, "little"); d[7] = build; d[8:10] = catalog.to_bytes(2, "little")
    colors = [(240, 194, 160), (74, 48, 30), (60, 110, 160), (230, 40, 40), (40, 60, 220), (250, 250, 250), (30, 30, 30)]
    for index, color in enumerate(colors):
        d[10 + index * 3:13 + index * 3] = bytes(color)
    for slot, item in enumerate(items):
        d[31 + slot * 2:33 + slot * 2] = item.to_bytes(2, "little")
    d[1017:1021] = (zlib.crc32(bytes(d[:1017])) & 0xffffffff).to_bytes(4, "little")
    return bytes(d)


def main():
    build = pathlib.Path(sys.argv[1]).resolve()
    client = os.environ.get("CNA_SERVICE_AVATAR_CLIENT_HARNESS")
    catalog = os.environ.get("CNA_AVATAR_CATALOG_DIR")
    if not client or not catalog:
        print("CNA_SERVICE_AVATAR_CLIENT_HARNESS and CNA_AVATAR_CATALOG_DIR required")
        return 77
    catalog = pathlib.Path(catalog)
    assert pathlib.Path(client).is_file() and (catalog / "catalog.json").is_file(), "CNA inputs"
    with tempfile.TemporaryDirectory(prefix="cna-avatars-", dir=build) as temp:
        root = pathlib.Path(temp); os.chmod(root, 0o700)
        db = root / "state.sqlite3"; cert = root / "cert.pem"; key = root / "key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1", "-subj", "/CN=localhost",
                        "-addext", "subjectAltName=DNS:localhost", "-keyout", str(key), "-out", str(cert)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        admin = str(build / "cna-gamer-services-admin")
        run = lambda *args, text=None: subprocess.run([admin, str(db), *args], input=text, text=True, check=True,
                                                      stdout=subprocess.PIPE).stdout.strip()
        run("title", "one", "One")
        for user in ("alice", "bob", "charlie"):
            run("user", user, user.title(), text=user + "-password\n")
        assert run("avatar-catalog", str(catalog)) == "1", "catalog v1 import"
        # v2: everything from v1 plus a crown, a distinct valid GLB derived from the cap.
        newer = root / "catalog-v2"; shutil.copytree(catalog, newer)
        manifest = json.loads((catalog / "catalog.json").read_text())
        manifest["catalogVersion"] = 2
        crown = {}
        for body in ("female", "male"):
            data = (catalog / ("hat_cap.%s.glb" % body)).read_bytes().replace(b"hat_cap", b"hat_crn")
            name = "hat_crown.%s.glb" % body
            (newer / name).write_bytes(data)
            import hashlib
            crown[body] = hashlib.sha256(data).hexdigest()
            manifest["assets"].append({"name": name, "sha256": crown[body], "size": len(data)})
        manifest["items"].append({"id": 102, "slot": "hat", "name": "hat_crown",
                                  "assets": {"female": "hat_crown.female.glb", "male": "hat_crown.male.glb"}})
        (newer / "catalog.json").write_text(json.dumps(manifest))
        assert run("avatar-catalog", str(newer)) == "2", "catalog v2 import"
        run("avatar", "alice", "set", text=description(2, [1, 20, 40, 60, 0, 102]).hex() + "\n")
        run("avatar", "bob", "random", "female")
        server = subprocess.Popen([str(build / "cna-gamer-services-server"), "--database", str(db), "--listen", "127.0.0.1",
                                   "--port", "0", "--cert", str(cert), "--key", str(key)], stdout=subprocess.PIPE, text=True)
        try:
            line = server.stdout.readline(); assert "listening" in line, "service startup"
            url = "https://localhost:" + line.strip().rsplit(":", 1)[1] + "/cna/v1"
            cache = root / "cache"
            env = os.environ.copy(); env.pop("DISPLAY", None); env["WAYLAND_DISPLAY"] = ""
            env.update(CNA_GAMER_SERVICES_ENDPOINT=url, CNA_GAME_ID="one", CNA_GAMER_SERVICES_CA_BUNDLE=str(cert),
                       CNA_GAMER_SERVICES_CREDENTIALS_DIR="0", CNA_GAMER_SERVICES_INSECURE_LOOPBACK="0",
                       CNA_GAMER_SERVICES_CACHE_DIR=str(cache))
            for attempt in ("first", "cached"):
                result = subprocess.run([client], input="alice-password\n", text=True, capture_output=True, env=env, timeout=120)
                assert result.returncode == 0, "avatar client %s run: %s" % (attempt, result.stderr.strip())
                lines = dict(l.split(" ", 1) for l in result.stdout.splitlines() if l.startswith("avatar-"))
                assert lines.get("avatar-own") == "valid=1 body=1 height=1760 catalog=2", lines
                assert lines.get("avatar-lookup", "").startswith("valid=1 body=0 "), lines
                assert lines.get("avatar-none") == "valid=0 body=0 height=0 catalog=0", lines
                assert lines.get("avatar-ready") == "substituted=0", lines
                # Only the item missing from CNA's embedded catalog came over the wire, verified and cached.
                cached = sorted(p.name for p in cache.iterdir()) if cache.is_dir() else []
                assert cached == [crown["male"]], cached
                print(attempt, "run:", result.stdout.strip().replace("\n", "; "))
        finally:
            server.terminate(); server.wait(timeout=10); server.stdout.close()
    print("Standard XNA avatars over the CNA service: own/looked-up/absent descriptions, catalog v2 item fetched by hash, cached, rendered Ready.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
