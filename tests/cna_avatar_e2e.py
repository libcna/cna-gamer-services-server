# SPDX-License-Identifier: MIT
"""A CNA process reads and renders service avatars with the standard XNA API.

The service imports every catalog CNA embeds (v1, v2, ...) and one newer catalog the client does
not have: the newest plus one extra hat. Alice's avatar is a format 2 description (facial hair and
a shaped face) wearing that hat. Her CNA client installs that catalog as one pack -- only the
files it lacks cross the wire, through the binary file route, verified, validated and activated
at once -- and renders her avatar without substituting anything; a later run uses the installed
pack. A client that declines catalog updates is given the service's projection onto the newest
catalog it has, which it draws with nothing substituted, and installs nothing.
"""
import json, os, pathlib, shutil, subprocess, sys, tempfile, zlib


def description(catalog, items, body=1, height=1760, build=140, facial=0, face=None):
    d = bytearray(1021)
    d[0] = 2 if facial or face else 1; d[1:4] = b"CNA"; d[4] = body
    d[5:7] = height.to_bytes(2, "little"); d[7] = build; d[8:10] = catalog.to_bytes(2, "little")
    colors = [(240, 194, 160), (74, 48, 30), (60, 110, 160), (230, 40, 40), (40, 60, 220), (250, 250, 250), (30, 30, 30)]
    for index, color in enumerate(colors):
        d[10 + index * 3:13 + index * 3] = bytes(color)
    for slot, item in enumerate(items):
        d[31 + slot * 2:33 + slot * 2] = item.to_bytes(2, "little")
    if d[0] == 2:
        d[43:45] = facial.to_bytes(2, "little")
        d[45:61] = bytes(face or [128] * 16)
    d[1017:1021] = (zlib.crc32(bytes(d[:1017])) & 0xffffffff).to_bytes(4, "little")
    return bytes(d)


def main():
    build = pathlib.Path(sys.argv[1]).resolve()
    client = os.environ.get("CNA_SERVICE_AVATAR_CLIENT_HARNESS")
    catalogs = os.environ.get("CNA_AVATAR_CATALOGS")
    if not catalogs and os.environ.get("CNA_AVATAR_CATALOG_DIR"):
        catalogs = str(pathlib.Path(os.environ["CNA_AVATAR_CATALOG_DIR"]).parent)
    if not client or not catalogs:
        print("CNA_SERVICE_AVATAR_CLIENT_HARNESS and CNA_AVATAR_CATALOGS (CNA's modules/gamer-services/assets/avatars) required")
        return 77
    catalogs = pathlib.Path(catalogs)
    versions = sorted(int(p.name[1:]) for p in catalogs.glob("v*") if (p / "catalog.json").is_file())
    assert pathlib.Path(client).is_file() and versions and versions == list(range(1, len(versions) + 1)), "CNA inputs"
    newest = catalogs / ("v%d" % versions[-1])
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
        for version in versions:
            assert run("avatar-catalog", str(catalogs / ("v%d" % version))) == str(version), "CNA catalog v%d import" % version
        # One version past CNA's: its newest catalog plus a crown, a distinct valid GLB derived from the cap.
        target = versions[-1] + 1
        newer = root / ("catalog-v%d" % target); shutil.copytree(newest, newer)
        manifest = json.loads((newest / "catalog.json").read_text())
        manifest["catalogVersion"] = target
        crown = {}
        for body in ("female", "male"):
            data = (newest / ("hat_cap.%s.glb" % body)).read_bytes().replace(b"hat_cap", b"hat_crn")
            name = "hat_crown.%s.glb" % body
            (newer / name).write_bytes(data)
            import hashlib
            crown[body] = hashlib.sha256(data).hexdigest()
            manifest["assets"].append({"name": name, "sha256": crown[body], "size": len(data)})
        crown_id = max(i["id"] for i in manifest["items"] + manifest.get("featureItems", [])) + 1
        manifest["items"].append({"id": crown_id, "slot": "hat", "name": "hat_crown",
                                  "assets": {"female": "hat_crown.female.glb", "male": "hat_crown.male.glb"}})
        (newer / "catalog.json").write_text(json.dumps(manifest))
        assert run("avatar-catalog", str(newer)) == str(target), "newer catalog import"
        beard = next((i["id"] for i in manifest.get("featureItems", [])), 0)
        face = [128] * 16; face[5] = 210; face[0] = 90
        own = description(target, [1, 20, 40, 60, 0, crown_id], facial=beard, face=face)
        run("avatar", "alice", "set", text=own.hex() + "\n")
        run("avatar", "bob", "random", "female")
        server = subprocess.Popen([str(build / "cna-gamer-services-server"), "--database", str(db), "--listen", "127.0.0.1",
                                   "--port", "0", "--cert", str(cert), "--key", str(key)], stdout=subprocess.PIPE, text=True)
        try:
            line = server.stdout.readline(); assert "listening" in line, "service startup"
            url = "https://localhost:" + line.strip().rsplit(":", 1)[1] + "/cna/v1"
            cache = root / "cache"; installed = root / "catalogs"
            env = os.environ.copy(); env.pop("DISPLAY", None); env["WAYLAND_DISPLAY"] = ""
            env.update(CNA_GAMER_SERVICES_ENDPOINT=url, CNA_GAME_ID="one", CNA_GAMER_SERVICES_CA_BUNDLE=str(cert),
                       CNA_GAMER_SERVICES_CREDENTIALS_DIR="0", CNA_GAMER_SERVICES_INSECURE_LOOPBACK="0",
                       CNA_GAMER_SERVICES_CACHE_DIR=str(cache), CNA_GAMER_SERVICES_CATALOGS_DIR=str(installed))
            embedded = ",".join(str(v) for v in versions)
            pack = installed / ("v%d" % target)
            stamp = None
            for attempt in ("first", "installed"):
                result = subprocess.run([client], input="alice-password\n", text=True, capture_output=True, env=env, timeout=120)
                assert result.returncode == 0, "avatar client %s run: %s" % (attempt, result.stderr.strip())
                lines = dict(l.split(" ", 1) for l in result.stdout.splitlines() if l.startswith("avatar-"))
                # Before anything is read: the release's catalogs, plus the pack installed by the first run.
                assert lines.get("avatar-catalogs") == "available=" + embedded + ("" if attempt == "first" else ",%d" % target), lines
                assert lines.get("avatar-own") == "valid=1 body=1 height=1760 catalog=%d" % target, lines
                assert lines.get("avatar-lookup", "").startswith("valid=1 body=0 "), lines
                assert lines.get("avatar-none") == "valid=0 body=0 height=0 catalog=0", lines
                assert lines.get("avatar-ready") == "substituted=0 unavailable=0", lines
                # The avatar editor's save: an edit on the newest catalog, stored and read back.
                assert lines.get("avatar-edit") == "saved=1 format=2", lines
                # One installed, self-contained pack; nothing in the per-file asset cache; no staging left.
                record = json.loads((pack / "pack.json").read_text())
                assert record["version"] == target and record["packFormat"] == 1, record
                assert sorted(p.name for p in pack.iterdir()) == sorted({a["sha256"] for a in manifest["assets"]} | {"catalog.json", "pack.json"})
                assert not any(p.name.startswith(".staging-") for p in installed.iterdir()), "staging removed"
                assert not cache.is_dir() or not any(cache.iterdir()), "no avatar file in the asset cache"
                if stamp is None:
                    stamp = (pack / "pack.json").stat().st_mtime_ns
                assert (pack / "pack.json").stat().st_mtime_ns == stamp, "the installed pack is reused, not reinstalled"
                print(attempt, "run:", result.stdout.strip().replace("\n", "; "))
            # A client that declines catalog updates: the service projects Alice's avatar onto the
            # newest catalog it has (the crown, which only the newer catalog has, left out).
            declined = root / "catalogs-declined"
            env.update(CNA_AVATAR_CATALOG_UPDATES="0", CNA_GAMER_SERVICES_CATALOGS_DIR=str(declined))
            result = subprocess.run([client, "--view-only"], input="alice-password\n", text=True, capture_output=True, env=env, timeout=120)
            assert result.returncode == 0, "declining client: " + result.stderr.strip()
            lines = dict(l.split(" ", 1) for l in result.stdout.splitlines() if l.startswith("avatar-"))
            assert lines.get("avatar-own") == "valid=1 body=1 height=1760 catalog=%d" % versions[-1], lines
            assert lines.get("avatar-ready") == "substituted=0 unavailable=0", lines
            assert not declined.exists() or not any(declined.iterdir()), "nothing installed"
            print("declining run:", result.stdout.strip().replace("\n", "; "))
        finally:
            server.terminate(); server.wait(timeout=10); server.stdout.close()
    print("Standard XNA avatars over the CNA service: every CNA catalog imported; a format 2 avatar on catalog v%d; "
          "own/looked-up/absent descriptions; the missing catalog installed as one pack and reused; a declining "
          "client drawing the projection; an editor save stored through avatars.set." % target)
    return 0


if __name__ == "__main__":
    sys.exit(main())
