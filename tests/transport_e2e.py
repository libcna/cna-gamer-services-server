# SPDX-License-Identifier: MIT
"""Real TLS listener, independent client processes and restart persistence."""
import json, os, pathlib, ssl, subprocess, sys, tempfile, urllib.request, urllib.parse, sqlite3, struct, zlib, hashlib, selectors


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


def directory_worker():
    url, ca, state, action, kind = sys.argv[2:]
    def login(name):
        result=request(url,ca,"one","auth.login",{"username":name,"password":sys.stdin.readline().strip()})
        assert result["error"]=="OK","directory worker authentication"
        return result["result"]["token"]
    properties=[17,None,None,None,None,None,None,42]
    if action=="host":
        token,local=login("alice"),login("charlie")
        result=request(url,ca,"one","sessions.create",{"kind":kind,"maxGamers":8,"privateSlots":1,"properties":properties,"allowJoinInProgress":False,"participants":[token,local]},token)
        assert result["error"]=="OK" and result["result"]["currentGamers"]==2
        invitation=request(url,ca,"one","invites.send",{"session":result["result"]["session"],"gamertag":"Bob"},token)
        assert invitation["error"]=="OK" and invitation["result"]["status"]=="pending","persistent invitation"
        pathlib.Path(state).write_text(json.dumps({"token":token,"session":result["result"]["session"],"invite":invitation["result"]["invite"]}))
        os.chmod(state,0o600)
    else:
        token,local=login("bob"),login("dana")
        find={"kind":kind,"localCount":2,"start":0,"limit":8,"properties":[17,None,None,None,None,None,None,None]}
        listed=request(url,ca,"one","sessions.find",find,token)["result"]["sessions"]
        assert len(listed)==1 and listed[0]["kind"]==kind,"process discovery"
        find["properties"][7]=41
        assert request(url,ca,"one","sessions.find",find,token)["result"]["sessions"]==[],"property mismatch"
        joined=request(url,ca,"one","sessions.join",{"session":listed[0]["session"],"participants":[token,local]},token)
        assert joined["error"]=="OK" and joined["result"]["currentGamers"]==4
        assert len({member["machine"] for member in joined["result"]["members"]})==2
        assert request(url,ca,"one","sessions.leave",{"session":listed[0]["session"]},token)["result"]["ended"]==False
        inbox=request(url,ca,"one","invites.list",{"start":0,"limit":32},token)["result"]["invites"]
        host=json.loads(pathlib.Path(state).read_text())
        assert len(inbox)==1 and inbox[0]["invite"]==host["invite"] and inbox[0]["status"]=="pending","restart preserves invitation"
        accepted=request(url,ca,"one","invites.accept",{"invite":host["invite"]},token)
        assert accepted["error"]=="OK" and accepted["result"]["status"]=="accepted","explicit user acceptance"
        args={"session":host["session"],"invite":host["invite"],"participants":[token,local]}
        joined=request(url,ca,"one","sessions.joinInvited",args,token)
        assert joined["error"]=="OK" and joined["result"]["currentGamers"]==4
        assert joined["result"]["openPrivateSlots"]==0,"invite consumes private then public slots"
        assert request(url,ca,"one","sessions.joinInvited",args,token)["result"]==joined["result"],"idempotent invited join"
        assert request(url,ca,"one","sessions.leave",{"session":host["session"]},token)["result"]["ended"]==False
        assert request(url,ca,"one","sessions.joinInvited",args,token)["error"]=="INVALID_STATE","used invitation cannot resurrect membership"


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
        for user in ("alice", "bob", "charlie", "dana"):
            subprocess.run([admin, str(db), "user", user, user.title()], input=user+"-password\n", text=True, check=True, stdout=subprocess.DEVNULL)
            subprocess.run([admin,str(db),"picture",user,image_hash],check=True)
        for game in ("one","two"):
            definition={"key":"BestScoreLifeTime","mode":0,"ascending":False,"aggregation":"best","arbitrated":False,"columns":{"Rounds":"int32","Label":"string","Total":"int64","Scale":"single","Precision":"double","When":"datetime","Duration":"timespan","Outcome":"outcome"}}
            subprocess.run([admin,str(db),"leaderboard",game],input=json.dumps(definition),text=True,check=True)
        for user,rating in (("Alice",100),("Bob",200)):
            entry={"key":"BestScoreLifeTime","mode":0,"gamertag":user,"rating":rating,"columns":{"Rounds":{"type":"int32","value":3},"Label":{"type":"string","value":"Original"},"Total":{"type":"int64","value":9223372036854775807},"Scale":{"type":"single","value":1.25},"Precision":{"type":"double","value":2.5},"When":{"type":"datetime","value":123456},"Duration":{"type":"timespan","value":-1000},"Outcome":{"type":"outcome","value":1}}}
            subprocess.run([admin,str(db),"seed-leaderboard","one"],input=json.dumps(entry),text=True,check=True)
        server = None
        main_refresh = None
        def run_cna(url, username, state, action, game="one", password=None, trust=None, credentials=None):
            environment = os.environ.copy()
            environment.update(CNA_GAMER_SERVICES_ENDPOINT=url, CNA_GAME_ID=game,
                               CNA_GAMER_SERVICES_CA_BUNDLE=str(ca) if trust is None else trust,
                               CNA_GAMER_SERVICES_INSECURE_LOOPBACK="0", CNA_GAMER_SERVICES_CACHE_DIR=str(root/"cache"),CNA_GAMER_SERVICES_CREDENTIALS_DIR="0")
            if credentials:environment["CNA_GAMER_SERVICES_CREDENTIALS_DIR"]=str(credentials)
            environment.pop("DISPLAY", None); environment.pop("WAYLAND_DISPLAY", None)
            subprocess.run([client, "--real", username, state, action],
                           input=(("alice-password\nbob-password\ncharlie-password\ndana-password\n") if action=="remember-four" else (password or username+"-password")+"\n"), text=True,
                           env=environment, check=True, timeout=45)
        def start(port=0):
            process = subprocess.Popen([str(build/"cna-gamer-services-server"), "--database", str(db), "--listen", "127.0.0.1", "--port", str(port), "--cert", str(ca), "--key", str(key)], stdout=subprocess.PIPE, text=True)
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
                # A write is a temporary renamed into place (a new inode); a read may only refresh
                # the entry's use time, which the client's least-recently-used eviction relies on.
                cache_inode=(root/"cache"/image_hash).stat().st_ino
                run_cna(url, "bob", "none", "read")
                assert (root/"cache"/image_hash).stat().st_ino==cache_inode,"warm cross-process cache must avoid rewriting"
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
                environment.update(CNA_GAMER_SERVICES_ENDPOINT=url, CNA_GAME_ID="one", CNA_GAMER_SERVICES_CA_BUNDLE=str(ca), CNA_GAMER_SERVICES_INSECURE_LOOPBACK="0", CNA_GAMER_SERVICES_CACHE_DIR=str(root/"cache"),CNA_GAMER_SERVICES_CREDENTIALS_DIR="0")
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
            if client or c_client:
                stop(server);server,url=start()
            if client:
                main_credentials = request(url, str(ca), "one", "auth.login", {"username":"alice", "password":"alice-password"})["result"]
                token = main_credentials["token"]
                main_refresh = main_credentials["refreshToken"]
            if c_client:
                for user,state in (("alice","earned"),("bob","none")):
                    environment=os.environ.copy()
                    environment.update(CNA_GAMER_SERVICES_ENDPOINT=url,CNA_GAME_ID="one",CNA_GAMER_SERVICES_CA_BUNDLE=str(ca),CNA_GAMER_SERVICES_CACHE_DIR=str(root/"cache"),CNA_GAMER_SERVICES_INSECURE_LOOPBACK="0",CNA_GAMER_SERVICES_CREDENTIALS_DIR="0")
                    environment.pop("DISPLAY",None);environment["WAYLAND_DISPLAY"]=""
                    subprocess.run([c_client,user,state],input=user+"-password\n",text=True,env=environment,check=True,timeout=30)
            if client:
                stop(server);server,url=start()
                user_store=root/"credentials"
                run_cna(url,"alice","earned","remember",credentials=user_store)
                records=list(user_store.glob("*.json"));assert len(records)==1
                assert user_store.stat().st_mode&0o777==0o700 and records[0].stat().st_mode&0o777==0o600
                assert set(json.loads(records[0].read_text()))=={"v","expires","refreshToken"}
                port=urllib.parse.urlsplit(url).port
                stop(server);server,url=start(port)
                run_cna(url,"alice","earned","resume",credentials=user_store)
                assert not list(user_store.glob("*.json")),"explicit signout must clear refresh authority"
                run_cna(url,"alice","earned","remember-four",credentials=user_store)
                assert len(list(user_store.glob("*.json")))==4,"four separate local refresh slots"
                port=urllib.parse.urlsplit(url).port;stop(server);server,url=start(port)
                run_cna(url,"alice","earned","resume-four",credentials=user_store)
                assert len(list(user_store.glob("*.json")))==3,"signout removes only its local slot"
                # A live XNA client pumps maintenance, retains identity on outage, and reconnects.
                environment=os.environ.copy()
                environment.update(CNA_GAMER_SERVICES_ENDPOINT=url,CNA_GAME_ID="one",CNA_GAMER_SERVICES_CA_BUNDLE=str(ca),CNA_GAMER_SERVICES_CACHE_DIR=str(root/"cache"),CNA_GAMER_SERVICES_CREDENTIALS_DIR="0",CNA_GAMER_SERVICES_INSECURE_LOOPBACK="0")
                environment.pop("DISPLAY",None);environment["WAYLAND_DISPLAY"]=""
                live=subprocess.Popen([client,"--real","alice","earned","maintenance"],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True,env=environment)
                def line(expected):
                    ready=selectors.DefaultSelector();ready.register(live.stdout,selectors.EVENT_READ)
                    try:
                        assert ready.select(45),"maintenance client stalled"
                        assert live.stdout.readline().strip()==expected,"maintenance client phase"
                    finally:ready.close()
                def advance():live.stdin.write("continue\n");live.stdin.flush()
                try:
                    live.stdin.write("alice-password\n");live.stdin.flush();line("maintenance-ready")
                    subprocess.run([admin,str(db),"expire-access","alice"],check=True)
                    advance();line("maintenance-refreshed")
                    with sqlite3.connect(db) as inspect:inspect.execute("UPDATE sessions SET last_seen=0 WHERE user_id=(SELECT id FROM users WHERE username='alice')")
                    advance();line("maintenance-heartbeat")
                    with sqlite3.connect(db) as inspect:
                        assert inspect.execute("SELECT MAX(last_seen) FROM sessions WHERE user_id=(SELECT id FROM users WHERE username='alice')").fetchone()[0]>0,"Update heartbeat must maintain online activity"
                    port=urllib.parse.urlsplit(url).port;stop(server);server=None
                    advance();line("maintenance-offline")
                    server,url=start(port);advance()
                    live.stdin.close();assert live.wait(timeout=15)==0,"maintenance/reconnect client"
                finally:
                    if live.poll() is None:live.kill();live.wait()
                    live.stdout.close()

                run_cna(url,"alice","earned","leaderboard-write")
                stop(server);server,url=start()
                run_cna(url,"alice","earned","leaderboard-after")
                run_cna(url,"bob","none","leaderboard-after")
            if main_refresh:
                token = request(url, str(ca), "one", "auth.refresh", {"refreshToken":main_refresh})["result"]["token"]
            earned = request(url, str(ca), "one", "achievements.list", token=token)["result"]["achievements"]
            assert earned[0]["earnedTicks"] > 0
            for kind in ("player","ranked"):
                stop(server);server,url=start()
                state=root/("directory-"+kind+".json")
                subprocess.run([sys.executable,__file__,"--directory-worker",url,str(ca),str(state),"host",kind],input="alice-password\ncharlie-password\n",text=True,check=True)
                counts=json.loads(subprocess.check_output([admin,str(db),"inspect-online","one"],text=True))
                assert counts["sessions"]==1 and counts["members"]==2 and counts["invitations"]>=1
                stop(server);server,url=start()
                subprocess.run([sys.executable,__file__,"--directory-worker",url,str(ca),str(state),"join",kind],input="bob-password\ndana-password\n",text=True,check=True)
                host=json.loads(state.read_text())
                snapshot=request(url,str(ca),"one","sessions.get",{"session":host["session"]},host["token"])
                assert snapshot["result"]["currentGamers"]==2,"remote multi-local leave"
                if kind=="ranked":
                    subprocess.run([admin,str(db),"reset-online","one"],check=True)
                    assert request(url,str(ca),"one","sessions.get",{"session":host["session"]},host["token"])["error"]=="NOT_FOUND"
                    counts=json.loads(subprocess.check_output([admin,str(db),"inspect-online","one"],text=True))
                    assert counts["sessions"]==0 and counts["members"]==0 and counts["invitations"]==0 and counts["senderLimits"]>=1,"reset preserves independent quota"
                else:
                    assert request(url,str(ca),"one","sessions.leave",{"session":host["session"]},host["token"])["result"]["ended"]==True
            credentials=request(url,str(ca),"one","auth.login",{"username":"alice","password":"alice-password"})["result"]
            stop(server);server,url=start()
            rotated=request(url,str(ca),"one","auth.refresh",{"refreshToken":credentials["refreshToken"]})["result"]
            assert rotated["refreshToken"]!=credentials["refreshToken"] and rotated["refreshExpires"]==credentials["refreshExpires"]
            assert request(url,str(ca),"one","auth.ping",token=credentials["token"])["error"]=="UNAUTHENTICATED"
            assert request(url,str(ca),"one","auth.ping",token=rotated["token"])["error"]=="OK"
            assert request(url,str(ca),"two","auth.refresh",{"refreshToken":rotated["refreshToken"]})["error"]=="UNAUTHENTICATED"
            assert request(url,str(ca),"one","auth.refresh",{"refreshToken":credentials["refreshToken"]})["error"]=="UNAUTHENTICATED"
            assert request(url,str(ca),"one","auth.ping",token=rotated["token"])["error"]=="UNAUTHENTICATED"
            assert request(url, str(ca), "one", "auth.logout", token=token)["error"] == "OK"
            assert request(url, str(ca), "one", "achievements.list", token=token)["error"] == "UNAUTHENTICATED"
        finally:
            if server: stop(server)
        refused = subprocess.run([str(build/"cna-gamer-services-server"), "--database", str(db), "--listen", "0.0.0.0", "--insecure-loopback"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        assert refused.returncode != 0
        if client: print("CNA E2E passed: Guide authentication and masking, rejected password/CA/hostname, two users/processes/titles, async completion, idempotent award, client/server restart persistence, lookup/profile, four local accounts and private refresh persistence/resume, expiry renewal, Update heartbeat, outage/reconnect without identity duplication, sign-out, mutual friends/rich presence, admin revocation, picture streams/cache and corrupt-cache recovery, remote leaderboard paging/centering/restricted reads and typed columns, LocalWithLeaderboards/EndGame final writes and restart persistence")
        print("TLS E2E passed: trusted TLS, untrusted CA, wrong hostname, two client processes, title isolation, restart persistence, rotating refresh/replay/title isolation/heartbeat, revocation, two-process/four-user PlayerMatch and Ranked directory filtering/join/leave/restart, persisted invitations/acceptance/private invited joins/replay (control only), insecure public bind refusal")


if __name__ == "__main__":
    if len(sys.argv)>1 and sys.argv[1]=="--worker": worker()
    elif len(sys.argv)>1 and sys.argv[1]=="--directory-worker": directory_worker()
    else: main()
