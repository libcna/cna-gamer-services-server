# SPDX-License-Identifier: MIT
"""Two genuine CNA libcurl/ENet processes through TLS relay; no Internet isolation claim."""
import os, pathlib, selectors, shutil, subprocess, sys, tempfile, time


def main():
    assert len(sys.argv) in (2,3) and (len(sys.argv)==2 or sys.argv[2]=="--isolated"),"test arguments"
    isolated=len(sys.argv)==3
    helper=os.environ.get("CNA_SERVICE_SLIRP4NETNS") or shutil.which("slirp4netns")
    if isolated:
        if sys.platform!="linux" or not helper or not shutil.which("unshare") or not shutil.which("ip"):
            print("Linux unshare/ip/slirp4netns required for NAT-isolated relay test")
            return 77
        assert pathlib.Path(helper).is_file(),"missing configured slirp4netns"
        probe=subprocess.run(["unshare","--user","--map-root-user","--net","true"],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        if probe.returncode:
            print("Unprivileged user/network namespaces are unavailable; NAT relay test not run")
            return 77
    build=pathlib.Path(sys.argv[1]).resolve()
    client=os.environ.get("CNA_SERVICE_RELAY_CLIENT_HARNESS")
    if not client:
        print("CNA_SERVICE_RELAY_CLIENT_HARNESS required")
        return 77
    assert pathlib.Path(client).is_file(), "missing CNA relay harness"
    with tempfile.TemporaryDirectory(prefix="cna-native-relay-",dir=build) as temp:
        root=pathlib.Path(temp);os.chmod(root,0o700)
        db=root/"state.sqlite3";cert=root/"cert.pem";key=root/"key.pem";wrong=root/"wrong.pem"
        def certificate(public,private):
            subprocess.run(["openssl","req","-x509","-newkey","rsa:2048","-nodes","-days","1","-subj","/CN=localhost","-addext","subjectAltName=DNS:localhost,IP:10.0.2.2","-keyout",str(private),"-out",str(public)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        certificate(cert,key);certificate(wrong,root/"wrong-key.pem")
        admin=str(build/"cna-gamer-services-admin")
        for game in ("one","two"):
            subprocess.run([admin,str(db),"title",game,game],check=True,stdout=subprocess.DEVNULL)
        for user in ("alice","bob","charlie","dana"):
            subprocess.run([admin,str(db),"user",user,user.title()],input=user+"-password\n",text=True,check=True,stdout=subprocess.DEVNULL)
        children=[];buffers={};server=None;nat_helpers=[];namespace_ids=set()
        def start():
            process=subprocess.Popen([str(build/"cna-gamer-services-server"),"--database",str(db),"--listen","127.0.0.1","--port","0","--cert",str(cert),"--key",str(key)],stdout=subprocess.PIPE,text=True)
            line=process.stdout.readline();assert "listening" in line,"service startup"
            return process,"https://localhost:"+line.strip().rsplit(":",1)[1]+"/cna/v1"
        def stop(process):
            process.terminate();process.wait(timeout=10);process.stdout.close()
        def read(process,prefix):
            limit=time.monotonic()+30
            ready=selectors.DefaultSelector();ready.register(process.stdout,selectors.EVENT_READ)
            try:
                while b"\n" not in buffers[process]:
                    assert time.monotonic()<limit and ready.select(max(0,limit-time.monotonic())),"CNA relay boundary timeout"
                    data=os.read(process.stdout.fileno(),4096)
                    assert data,"CNA relay child exited before boundary"
                    buffers[process]+=data
                    assert len(buffers[process])<=16384,"bounded native diagnostic output"
                line,buffers[process]=buffers[process].split(b"\n",1)
                line=line.decode("utf-8");assert line.startswith(prefix),"CNA relay boundary mismatch"
                return line
            finally:ready.close()
        def send(process,lines):
            process.stdin.write(lines.encode("utf-8"));process.stdin.flush()
        def spawn(role,kind,game,url,trust=cert):
            env=os.environ.copy();env.pop("DISPLAY",None);env["WAYLAND_DISPLAY"]=""
            env.update(CNA_GAMER_SERVICES_ENDPOINT=url,CNA_GAME_ID=game,CNA_GAMER_SERVICES_CA_BUNDLE=str(trust),CNA_GAMER_SERVICES_CREDENTIALS_DIR="0",CNA_GAMER_SERVICES_INSECURE_LOOPBACK="0")
            arguments=[client,role,kind]
            if isolated:
                env["CNA_GAMER_SERVICES_ENDPOINT"]=url.replace("localhost","10.0.2.2")
                arguments=["unshare","--user","--map-root-user","--net",sys.executable,str(pathlib.Path(__file__).with_name("relay_namespace_client.py"))]+arguments
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
                    nat=subprocess.Popen([helper,"--configure","--disable-dns","--ready-fd="+str(ready_write),"--exit-fd="+str(exit_read),str(pid),"tap0"],pass_fds=(ready_write,exit_read),env=helper_env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
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
                print(role,"isolated network namespace",inode,"private IPv4 10.0.2.100, NAT outbound only, no published ports")
            return process
        def done(process):
            result=read(process,"relay-done ");process.wait(timeout=15);assert process.returncode==0,"native relay probe failed"
            print(result)
        try:
            for kind,game in (("player","one"),("ranked","two")):
                server,url=start()
                if kind=="player" and not isolated:
                    for target,trust in ((url,wrong),(url.replace("localhost","127.0.0.1"),cert)):
                        refusal=spawn("refusal",kind,game,target,trust)
                        print(read(refusal,"relay-refused "));refusal.wait(timeout=15);assert refusal.returncode==0,"TLS refusal probe"
                host=spawn("host",kind,game,url);send(host,"alice-password\ncharlie-password\n")
                _,session,machine=read(host,"relay-host ").split()
                join=spawn("join",kind,game,url);send(join,"bob-password\ndana-password\n"+session+"\n")
                _,remote=read(join,"relay-join ").split();assert remote!=machine,"distinct authenticated machines"
                send(host,remote+"\n");read(host,"relay-ready");read(join,"relay-ready")
                send(host,"continue\n");send(join,"continue\n")
                read(host,"relay-exchanged");read(join,"relay-exchanged")
                print(kind,"two CNA processes/four local accounts exchanged four verified ENet application packets each way, including 32KiB fragmentation and unreliable channel; forged existing sender and unknown target refused before delivery")
                if kind=="player":
                    subprocess.run([admin,str(db),"revoke-user","dana"],check=True,stdout=subprocess.DEVNULL)
                    send(join,"continue\n");done(join)
                    send(host,"continue\n");read(host,"relay-still-open")
                    stop(server);server=None;send(host,"continue\n");done(host)
                else:
                    stop(server);server=None;send(host,"continue\n");send(join,"continue\n");done(host);done(join)
            if isolated:
                assert len(namespace_ids)==4,"two separate NAT clients per category"
                print("Verified NAT-isolated CNA libcurl WSS/ENet relay: separate rootless namespaces/NAT helpers, identical private addresses, no inbound mappings, two categories/titles, four identities, fragmented/unreliable game data, local UDP guards, revocation and server failure. Public online NetworkSession, reconnect and public Internet deployment remain unfinished.")
            else:
                print("Verified CNA libcurl WSS/ENet relay: CA/hostname refusals, two categories/titles, four identities, source-preserving game data, multi-local revocation, owner channel isolation and server failure. Localhost only; public online NetworkSession/Internet isolation/reconnect unfinished.")
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
