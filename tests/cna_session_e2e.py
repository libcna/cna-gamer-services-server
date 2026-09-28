# SPDX-License-Identifier: MIT
"""Two CNA processes using only the public XNA NetworkSession API against this service."""
import os, pathlib, selectors, shutil, subprocess, sys, tempfile, time


def main():
    flags=sys.argv[2:]
    assert len(sys.argv)>=2 and len(flags)==len(set(flags)) and set(flags)<={"--isolated","--invite"},"test arguments"
    isolated="--isolated" in flags
    invite="--invite" in flags
    helper=os.environ.get("CNA_SERVICE_SLIRP4NETNS") or shutil.which("slirp4netns")
    if isolated:
        if sys.platform!="linux" or not helper or not shutil.which("unshare") or not shutil.which("ip"):
            print("Linux unshare/ip/slirp4netns required for NAT-isolated session test")
            return 77
        assert pathlib.Path(helper).is_file(),"missing configured slirp4netns"
        probe=subprocess.run(["unshare","--user","--map-root-user","--net","true"],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        if probe.returncode:
            print("Unprivileged user/network namespaces are unavailable; NAT session test not run")
            return 77
    build=pathlib.Path(sys.argv[1]).resolve()
    client=os.environ.get("CNA_SERVICE_SESSION_CLIENT_HARNESS")
    if not client:
        print("CNA_SERVICE_SESSION_CLIENT_HARNESS required")
        return 77
    assert pathlib.Path(client).is_file(),"missing CNA public session harness"
    with tempfile.TemporaryDirectory(prefix="cna-public-session-",dir=build) as temp:
        root=pathlib.Path(temp);os.chmod(root,0o700)
        db=root/"state.sqlite3";cert=root/"cert.pem";key=root/"key.pem"
        subprocess.run(["openssl","req","-x509","-newkey","rsa:2048","-nodes","-days","1","-subj","/CN=localhost",
            "-addext","subjectAltName=DNS:localhost,IP:10.0.2.2","-keyout",str(key),"-out",str(cert)],
            check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        admin=str(build/"cna-gamer-services-admin")
        for game in ("one","two"):
            subprocess.run([admin,str(db),"title",game,game],check=True,stdout=subprocess.DEVNULL)
        board='{"key":"BestScoreLifeTime","mode":0,"ascending":false,"aggregation":"best","arbitrated":false,"columns":{"Rounds":"int32"}}'
        kills='{"key":"Kills","mode":0,"ascending":false,"aggregation":"latest","arbitrated":true,"columns":{}}'
        for game in ("one","two"):
            subprocess.run([admin,str(db),"leaderboard",game],input=board,text=True,check=True,stdout=subprocess.DEVNULL)
            subprocess.run([admin,str(db),"leaderboard",game],input=kills,text=True,check=True,stdout=subprocess.DEVNULL)
        for user in ("alice","bob","charlie","dana"):
            subprocess.run([admin,str(db),"user",user,user.title()],input=user+"-password\n",text=True,check=True,stdout=subprocess.DEVNULL)
        children=[];buffers={};server=None;nat_helpers=[];namespace_ids=set()
        def start():
            process=subprocess.Popen([str(build/"cna-gamer-services-server"),"--database",str(db),"--listen","127.0.0.1",
                "--port","0","--cert",str(cert),"--key",str(key)],stdout=subprocess.PIPE,text=True)
            line=process.stdout.readline();assert "listening" in line,"service startup"
            return process,"https://localhost:"+line.strip().rsplit(":",1)[1]+"/cna/v1"
        def stop(process):
            process.terminate();process.wait(timeout=10);process.stdout.close()
        def read(process,prefix):
            limit=time.monotonic()+45
            ready=selectors.DefaultSelector();ready.register(process.stdout,selectors.EVENT_READ)
            try:
                while b"\n" not in buffers[process]:
                    assert time.monotonic()<limit and ready.select(max(0,limit-time.monotonic())),"CNA session boundary timeout: "+prefix
                    data=os.read(process.stdout.fileno(),4096)
                    if not data:
                        diagnostic=process.stderr.read(4096).decode("utf-8","replace") if process.stderr else ""
                        raise AssertionError("CNA session child exited before "+prefix+": "+diagnostic.strip())
                    buffers[process]+=data
                    assert len(buffers[process])<=16384,"bounded native diagnostic output"
                line,buffers[process]=buffers[process].split(b"\n",1)
                line=line.decode("utf-8");assert line.startswith(prefix),"CNA session boundary mismatch: "+line
                return line
            finally:ready.close()
        def send(process,lines):
            process.stdin.write(lines.encode("utf-8"));process.stdin.flush()
        def spawn(role,kind,game,url):
            env=os.environ.copy();env.pop("DISPLAY",None);env["WAYLAND_DISPLAY"]=""
            env.update(CNA_GAMER_SERVICES_ENDPOINT=url,CNA_GAME_ID=game,CNA_GAMER_SERVICES_CA_BUNDLE=str(cert),
                CNA_GAMER_SERVICES_CREDENTIALS_DIR="0",CNA_GAMER_SERVICES_INSECURE_LOOPBACK="0")
            arguments=[client,role,kind]+(["invite"] if invite else [])
            if isolated:
                env["CNA_GAMER_SERVICES_ENDPOINT"]=url.replace("localhost","10.0.2.2")
                arguments=["unshare","--user","--map-root-user","--net",sys.executable,
                    str(pathlib.Path(__file__).with_name("relay_namespace_client.py"))]+arguments
            process=subprocess.Popen(arguments,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=0,env=env)
            children.append(process);buffers[process]=b""
            if isolated:
                _,pid,inode=read(process,"namespace-created ").split();pid=int(pid);inode=int(inode)
                assert pid==process.pid and inode!=os.stat("/proc/self/ns/net").st_ino and inode not in namespace_ids,"distinct client network namespaces"
                namespace_ids.add(inode)
                ready_read,ready_write=os.pipe();exit_read,exit_write=os.pipe()
                try:
                    helper_env=os.environ.copy()
                    library=os.environ.get("CNA_SERVICE_SLIRP_LIBRARY_PATH")
                    if library:helper_env["LD_LIBRARY_PATH"]=library
                    nat=subprocess.Popen([helper,"--configure","--disable-dns","--ready-fd="+str(ready_write),"--exit-fd="+str(exit_read),str(pid),"tap0"],
                        pass_fds=(ready_write,exit_read),env=helper_env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
                    nat_helpers.append((nat,exit_write));exit_write=None
                    os.close(ready_write);ready_write=None;os.close(exit_read);exit_read=None
                    ready=selectors.DefaultSelector();ready.register(ready_read,selectors.EVENT_READ)
                    try:assert ready.select(10) and os.read(ready_read,1)==b"1","external NAT configuration"
                    finally:ready.close()
                finally:
                    for descriptor in (ready_read,ready_write,exit_read,exit_write):
                        if descriptor is not None:os.close(descriptor)
                send(process,"configured\n")
                assert read(process,"namespace-ready ").split()==["namespace-ready",str(inode),"10.0.2.100"],"private address/default route verification"
            return process
        def both(prefix):
            return read(host,prefix),read(join,prefix)
        def proceed(*processes):
            for process in processes:send(process,"continue\n")
        def done(process):
            result=read(process,"session-done ");process.wait(timeout=15);assert process.returncode==0,"public session probe failed"
            return result
        try:
            for kind,game in (("player","one"),("ranked","two")):
                server,url=start()
                host=spawn("host",kind,game,url);send(host,"alice-password\ncharlie-password\n");read(host,"session-created")
                if invite:read(host,"invite-sent")
                join=spawn("join",kind,game,url);send(join,"bob-password\ndana-password\n");read(join,"session-joined")
                both("session-roster");proceed(host,join)
                exchanged=both("session-exchanged ");assert exchanged==("session-exchanged 6","session-exchanged 6"),"six verified deliveries each way"
                proceed(host,join);both("session-playing");proceed(host,join);both("session-lobby")
                if kind=="player":
                    proceed(join);print(done(join));proceed(host);print(done(host))
                else:
                    proceed(host);print(done(host));proceed(join);print(done(join))
                stop(server);server=None
                print(kind,"public XNA NetworkSession: two CNA processes/four Guide-signed-in accounts, pending Begin/End with one owner-thread callback,",
                    "Guide.ShowGameInvite -> Guide acceptance -> InviteAccepted -> synchronous JoinInvited," if invite else "property-filtered Find,",
                    "complete GamerJoined replay, shared machines, six verified packets each way incl. 32KiB and in-order,",
                    "host properties/join-in-progress and StartGame/EndGame observed remotely, per-machine EndGame leaderboard commits read back by both,",
                    "Ranked arbitrated rows from both machines resolved by agreement," if kind=="ranked" else "",
                    "client leave -> GamerLeft" if kind=="player" else "host leave -> SessionEnded(HostEndedSession)")
            if isolated:
                assert len(namespace_ids)==4,"two separate NAT clients per category"
                print("Verified in separate rootless NAT namespaces (identical private addresses, no inbound mappings); shared-host NAT, not public Internet deployment.")
            else:
                print("Verified public PlayerMatch/Ranked NetworkSession over verified TLS/WSS relay on localhost.")
        finally:
            for process in children:
                if process.poll() is None:process.kill();process.wait()
                for stream in (process.stdin,process.stdout,process.stderr):
                    if stream and not stream.closed:stream.close()
            for nat,exit_write in nat_helpers:
                os.close(exit_write)
                try:nat.wait(timeout=5)
                except subprocess.TimeoutExpired:nat.kill();nat.wait()
            if server and server.poll() is None:stop(server)
    return 0

if __name__=="__main__":sys.exit(main())
