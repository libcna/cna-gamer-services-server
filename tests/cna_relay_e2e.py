# SPDX-License-Identifier: MIT
"""Two genuine CNA libcurl/ENet processes through TLS relay; no Internet isolation claim."""
import os, pathlib, selectors, subprocess, sys, tempfile, time


def main():
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
            subprocess.run(["openssl","req","-x509","-newkey","rsa:2048","-nodes","-days","1","-subj","/CN=localhost","-addext","subjectAltName=DNS:localhost","-keyout",str(private),"-out",str(public)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        certificate(cert,key);certificate(wrong,root/"wrong-key.pem")
        admin=str(build/"cna-gamer-services-admin")
        for game in ("one","two"):
            subprocess.run([admin,str(db),"title",game,game],check=True,stdout=subprocess.DEVNULL)
        for user in ("alice","bob","charlie","dana"):
            subprocess.run([admin,str(db),"user",user,user.title()],input=user+"-password\n",text=True,check=True,stdout=subprocess.DEVNULL)
        children=[];buffers={};server=None
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
            process=subprocess.Popen([client,role,kind],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=0,env=env)
            children.append(process);buffers[process]=b""
            return process
        def done(process):
            result=read(process,"relay-done ");process.wait(timeout=15);assert process.returncode==0,"native relay probe failed"
            print(result)
        try:
            for kind,game in (("player","one"),("ranked","two")):
                server,url=start()
                if kind=="player":
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
                print(kind,"two CNA processes/four local accounts exchanged four verified ENet application packets each way, including 32KiB fragmentation and unreliable channel")
                if kind=="player":
                    subprocess.run([admin,str(db),"revoke-user","dana"],check=True,stdout=subprocess.DEVNULL)
                    send(join,"continue\n");done(join)
                    send(host,"continue\n");read(host,"relay-still-open")
                    stop(server);server=None;send(host,"continue\n");done(host)
                else:
                    stop(server);server=None;send(host,"continue\n");send(join,"continue\n");done(host);done(join)
            print("Verified CNA libcurl WSS/ENet relay: CA/hostname refusals, two categories/titles, four identities, source-preserving game data, multi-local revocation, owner channel isolation and server failure. Localhost only; public online NetworkSession/Internet isolation/reconnect unfinished.")
        finally:
            for process in children:
                if process.poll() is None:process.kill();process.wait()
                for stream in (process.stdin,process.stdout,process.stderr):
                    if stream and not stream.closed:stream.close()
            if server and server.poll() is None:stop(server)
    return 0

if __name__=="__main__":sys.exit(main())
