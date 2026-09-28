# SPDX-License-Identifier: MIT
"""Two genuine CNA processes: TLS/WSS preparation, directory/invites/restart and multi-local credentials.
Public XNA BeginFind/EndFind plus private create/join control evidence; no public online
create/join or Internet realtime assertion.
"""
import os, pathlib, selectors, subprocess, sys, tempfile, urllib.parse
from transport_e2e import request


def main():
    build = pathlib.Path(sys.argv[1]).resolve()
    client = os.environ.get("CNA_SERVICE_DIRECTORY_CLIENT_HARNESS")
    if not client:
        print("CNA_SERVICE_DIRECTORY_CLIENT_HARNESS required")
        return 77
    assert pathlib.Path(client).is_file(), "missing CNA directory harness"
    with tempfile.TemporaryDirectory(prefix="cna-directory-", dir=build) as temp:
        root=pathlib.Path(temp); os.chmod(root,0o700)
        db=root/"state.sqlite3"; cert=root/"cert.pem"; key=root/"key.pem"
        subprocess.run(["openssl","req","-x509","-newkey","rsa:2048","-nodes","-days","1","-subj","/CN=localhost","-addext","subjectAltName=DNS:localhost","-keyout",str(key),"-out",str(cert)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        admin=str(build/"cna-gamer-services-admin")
        for game in ("one","two"):
            subprocess.run([admin,str(db),"title",game,game],check=True,stdout=subprocess.DEVNULL)
        for user in ("alice","bob","charlie","dana"):
            subprocess.run([admin,str(db),"user",user,user.title()],input=user+"-password\n",text=True,check=True,stdout=subprocess.DEVNULL)
        server=None; children=[]
        def start(port=0):
            process=subprocess.Popen([str(build/"cna-gamer-services-server"),"--database",str(db),"--listen","127.0.0.1","--port",str(port),"--cert",str(cert),"--key",str(key)],stdout=subprocess.PIPE,text=True)
            line=process.stdout.readline(); assert "listening" in line
            return process,"https://localhost:"+line.strip().rsplit(":",1)[1]+"/cna/v1"
        def stop(process):
            process.terminate();process.wait(timeout=10);process.stdout.close()
        def read(process,prefix):
            ready=selectors.DefaultSelector();ready.register(process.stdout,selectors.EVENT_READ)
            try:
                assert ready.select(30), "CNA control probe stalled"
                line=process.stdout.readline().strip();assert line.startswith(prefix), "CNA control boundary failed"
                return line
            finally:ready.close()
        def expire(*users):
            for user in users:subprocess.run([admin,str(db),"expire-access",user],check=True)
        def spawn(role,kind,game,url,users):
            env=os.environ.copy();env.pop("DISPLAY",None);env["WAYLAND_DISPLAY"]=""
            env.update(CNA_GAMER_SERVICES_ENDPOINT=url,CNA_GAME_ID=game,CNA_GAMER_SERVICES_CA_BUNDLE=str(cert),CNA_GAMER_SERVICES_CREDENTIALS_DIR="0",CNA_GAMER_SERVICES_INSECURE_LOOPBACK="0")
            process=subprocess.Popen([client,role,kind],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True,env=env)
            children.append(process);process.stdin.write("".join(user+"-password\n" for user in users));process.stdin.flush()
            read(process,"directory-signed")
            return process
        try:
            for kind,game in (("player","one"),("ranked","two")):
                server,url=start()
                host=spawn("host",kind,game,url,("alice","charlie"))
                expire("alice","charlie")
                host.stdin.write("continue\n");host.stdin.flush()
                if kind=="ranked":
                    _,session,invite=read(host,"directory-playing ").split()
                    credentials=request(url,str(cert),game,"auth.login",{"username":"bob","password":"bob-password"})
                    assert credentials["error"]=="OK", "Ranked negative probe login"
                    token=credentials["result"]["token"]
                    def rpc(op,args):return request(url,str(cert),game,op,args,token)
                    found=rpc("sessions.find",{"kind":"ranked","localCount":1,"start":0,"limit":32,"properties":[None]*8})
                    assert found["error"]=="OK" and not found["result"]["sessions"], "playing Ranked advertisement"
                    args={"session":session,"participants":[token]}
                    assert rpc("sessions.join",args)["error"]=="INVALID_STATE", "playing Ranked ordinary admission"
                    assert rpc("invites.accept",{"invite":invite})["error"]=="OK", "Ranked invitation consent"
                    args["invite"]=invite
                    assert rpc("sessions.joinInvited",args)["error"]=="INVALID_STATE", "playing Ranked invited admission"
                    preserved=rpc("invites.get",{"invite":invite})
                    assert preserved["error"]=="OK" and preserved["result"]["status"]=="accepted", "rejected admission consumed consent"
                    assert rpc("invites.dismiss",{"invite":invite})["error"]=="OK", "negative probe cleanup"
                    assert rpc("auth.logout",{})["error"]=="OK", "negative probe logout"
                    host.stdin.write("continue\n");host.stdin.flush()
                _,session,invite=read(host,"directory-host ").split()
                # Same endpoint, new real server process, persisted invitation/membership.
                port=urllib.parse.urlsplit(url).port;stop(server);server=None;server,url=start(port)
                other_title="two" if game=="one" else "one"
                foreign=spawn("search-empty",kind,other_title,url,("bob","dana"))
                foreign.stdin.write("continue\n");foreign.stdin.flush()
                foreign_output,_=foreign.communicate(timeout=30)
                assert foreign.returncode==0 and "directory-done" in foreign_output,"public cross-title Find isolation"
                join=spawn("join",kind,game,url,("bob","dana"))
                expire("dana") # Owner stays valid: exercise stale secondary credential repair.
                join.stdin.write("continue\n"+session+"\n"+invite+"\n");join.stdin.flush()
                read(join,"directory-before-revoke")
                subprocess.run([admin,str(db),"revoke-user","dana"],check=True)
                join.stdin.write("continue\n");join.stdin.flush()
                output,_=join.communicate(timeout=45);assert join.returncode==0 and "directory-done" in output,"CNA join probe"
                print(kind,output.strip())
                host.stdin.write("continue\n");host.stdin.flush();read(host,"directory-after-join")
                expire("alice","charlie")
                host.stdin.write("continue\n");host.stdin.flush()
                output,_=host.communicate(timeout=45);assert host.returncode==0 and "directory-done" in output,"CNA host/leaderboard probe"
                print(kind,output.strip())
                stop(server);server=None
            print("Two primary CNA processes/four accounts plus an isolated-title search peer passed: standard BeginFind/EndFind, update-thread callback/metadata/End-once, both directory kinds, two title IDs, verified TLS/WSS, owned create/ordinary/invited preparation, failed/abandoned rollback, server restart, filtering, ordinary/private invited control join/leave/replay, auxiliary verified-TLS Ranked playing find/join/invited-join rejection and preserved consent, expired owner+secondary credentials, owner-update snapshot/lease pump renewal across restart and expired credentials, and leaderboard multi-local refresh. Public online create/join/lifecycle and realtime relay integration remain unfinished.")
        finally:
            for process in children:
                if process.poll() is None:process.kill();process.wait()
                if process.stdin and not process.stdin.closed:process.stdin.close()
                if process.stdout and not process.stdout.closed:process.stdout.close()
            if server and server.poll() is None:stop(server)
    return 0

if __name__=="__main__":sys.exit(main())
