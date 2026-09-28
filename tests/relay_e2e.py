# SPDX-License-Identifier: MIT
"""Independent verified-WSS transport checks; not yet CNA/ENet Internet acceptance."""
import asyncio, json, os, pathlib, socket, sqlite3, ssl, subprocess, sys, tempfile, time
import websockets
from websockets.frames import Frame, OP_TEXT
from websockets.exceptions import InvalidStatus
from transport_e2e import request

checks=0

def check(value):
    global checks
    checks+=1
    assert value, 'Relay transport assertion '+str(checks)


def frame(machine,payload=b'\x00enet\xff'):
    return b'CNR\x01\x01\x00\x00\x00'+bytes.fromhex(machine)+payload


async def exercise(build,root,db,cert,key,game,kind,url,all_credentials):
    context=ssl.create_default_context(cafile=cert)
    endpoint=url.replace('https:','wss:').replace('/cna/v1','/cna/relay/v1')
    def call(op,args=None,token=None):
        result=request(url,cert,game,op,args,token);check(result['error']=='OK');return result['result']
    credentials=all_credentials[game]
    a,c,b,d=(credentials[name]['token'] for name in ('alice','charlie','bob','dana'))
    settings={'kind':kind,'maxGamers':8,'privateSlots':0,'properties':[None]*8,'allowJoinInProgress':False,'participants':[a,c]}
    directory=call('sessions.create',settings,a);session=directory['session'];host=directory['hostMachine']
    joined=call('sessions.join',{'session':session,'participants':[b,d]},b)
    remote=next(member['machine'] for member in joined['members'] if member['machine']!=host)
    def ticket(owner,participants):
        return call('sessions.relayTicket',{'session':session,'participants':participants},owner)['ticket']
    async def connect():
        return await websockets.connect(endpoint,ssl=context,compression=None,proxy=None,ping_interval=None,close_timeout=3,max_queue=1)
    async def authenticated(owner,participants):
        ws=await connect();secret=ticket(owner,participants)
        await ws.send(json.dumps({'v':1,'id':'relay-probe','game':game,'ticket':secret}))
        result=json.loads(await asyncio.wait_for(ws.recv(),5));check(result['error']=='OK' and result['id']=='relay-probe')
        check(result['result']['session']==session and result['result']['maxDatagramBytes']==4096)
        return ws,secret
    async def closed(ws,code=None,timeout=7):
        try:
            await asyncio.wait_for(ws.recv(),timeout);check(False)
        except websockets.ConnectionClosed as error:
            if code is not None:check(error.rcvd is not None and error.rcvd.code==code)
            else:check(True)
        finally:await ws.close()
    check('relay' in call('hello')['capabilities'])
    # Verification must fail for an untrusted CA and for an IP absent from the SAN.
    for target,trust in ((endpoint,ssl.create_default_context()),(endpoint.replace('localhost','127.0.0.1'),context)):
        try:
            async with websockets.connect(target,ssl=trust,compression=None,proxy=None):check(False)
        except ssl.SSLCertVerificationError:check(True)
    try:
        async with websockets.connect(endpoint+'?unexpected=1',ssl=context,compression=None,proxy=None):check(False)
    except InvalidStatus as error:check(error.response.status_code==404)
    ws=await connect();ws.transport.write(Frame(OP_TEXT,b'\xff').serialize(mask=True));await closed(ws,1007)
    malformed=[{'v':2,'id':'x','game':game,'ticket':'a'*64}, {'v':1,'id':'x','game':game,'ticket':'a'*64,'extra':1}, {'v':1,'id':'x','game':game,'ticket':'a'*64}]
    for index,hello in enumerate(malformed):
        ws=await connect();await ws.send(json.dumps(hello));await closed(ws,1002 if index<2 else 1008)
    ws=await connect();await ws.send('{"v":1,"v":1}');await closed(ws,1002)
    ws=await connect();await ws.send(json.dumps({'v':1,'id':'unknown','game':game,'unknown':'x'}));await closed(ws,1002)
    ws=await connect();await ws.send(b'not-text');await closed(ws,1002)
    ws=await connect();await ws.send('x'*1025);await closed(ws,1009)
    ws=await connect();started=time.monotonic();await closed(ws,None);check(4.5<=time.monotonic()-started<7)
    left,used=await authenticated(a,[a,c]);right,_=await authenticated(b,[b,d])
    try:
        # A used ticket is refused; a new ticket cannot replace an existing machine channel.
        duplicate=await connect();await duplicate.send(json.dumps({'v':1,'id':'replay','game':game,'ticket':used}));await closed(duplicate,1008)
        duplicate=await connect();await duplicate.send(json.dumps({'v':1,'id':'duplicate','game':game,'ticket':ticket(a,[a,c])}));await closed(duplicate,1008)
        await left.send(frame(remote));check(await asyncio.wait_for(right.recv(),5)==frame(host))
        await right.send(frame(host,b'\x00return'));check(await asyncio.wait_for(left.recv(),5)==frame(remote,b'\x00return'))
        data=frame(remote,bytes(range(256))*16)
        await left.send(data);check(await asyncio.wait_for(right.recv(),5)==frame(host,data[24:]))
        await left.send([data[:3],data[3:24],data[24:]])
        try:check(await asyncio.wait_for(right.recv(),5)==frame(host,data[24:]))
        except asyncio.TimeoutError:
            print('Maximum-fragment failure states',left.state.name,left.close_code,right.state.name,right.close_code,flush=True)
            raise
        for destination in (host,'11111111111111111111111111111111'):
            await left.send(frame(destination))
            try:await asyncio.wait_for(right.recv(),0.1);check(False)
            except asyncio.TimeoutError:check(True)
        # A live foreign session and a foreign title cannot be selected by machine ID.
        for title in (game,'two' if game=='one' else 'one'):
            owner=all_credentials[title]['eve']['token']
            def foreign_call(op,args):
                result=request(url,cert,title,op,args,owner);check(result['error']=='OK');return result['result']
            foreign_settings=dict(settings,participants=[owner])
            foreign=foreign_call('sessions.create',foreign_settings)
            secret=foreign_call('sessions.relayTicket',{'session':foreign['session'],'participants':[owner]})['ticket']
            channel=await connect();await channel.send(json.dumps({'v':1,'id':'isolated','game':title,'ticket':secret}))
            check(json.loads(await asyncio.wait_for(channel.recv(),5))['error']=='OK')
            await left.send(frame(foreign['hostMachine']))
            try:await asyncio.wait_for(channel.recv(),0.1);check(False)
            except asyncio.TimeoutError:check(True)
            await channel.send(frame(remote))
            try:await asyncio.wait_for(right.recv(),0.1);check(False)
            except asyncio.TimeoutError:check(True)
            await channel.close();foreign_call('sessions.leave',{'session':foreign['session']})
        await left.send(frame(remote,b'after-isolation'));check(await asyncio.wait_for(right.recv(),5)==frame(host,b'after-isolation'))
        with sqlite3.connect(db) as storage:storage.execute('UPDATE directory_sessions SET expires=0 WHERE id=?',(session,))
        await closed(left,1008);await closed(right,1008)
        with sqlite3.connect(db) as storage:check(storage.execute('SELECT COUNT(*) FROM relay_tickets WHERE used=1').fetchone()[0]==0)
    finally:
        await left.close();await right.close()
    # Recreate the expired directory; old identity cannot route into a fresh session.
    directory=call('sessions.create',settings,a);session=directory['session'];host=directory['hostMachine']
    joined=call('sessions.join',{'session':session,'participants':[b,d]},b)
    remote=next(member['machine'] for member in joined['members'] if member['machine']!=host)
    for payload,code in ((b'bad',1002),(frame(remote,b'x'*4097),1009),('text-data',1002),(frame('0'*32),1002)):
        ws,_=await authenticated(a,[a,c]);await ws.send(payload);await closed(ws,code)
    # Repeated ordinary disconnect must reclaim tickets instead of exhausting the eight-record cap.
    for _ in range(12):
        ws,_=await authenticated(a,[a,c]);await ws.close()
        await asyncio.sleep(0.02)
    ws,_=await authenticated(a,[a,c])
    try:
        for _ in range(700):await ws.send(frame(host,b'x'))
    except websockets.ConnectionClosed:pass
    await closed(ws,1008)
    left,_=await authenticated(a,[a,c]);right,_=await authenticated(b,[b,d])
    # Pause the receiver's application and constrain its TCP receive window. The sender remains
    # within ingress policy; only the bounded recipient queue may terminate this slow channel.
    right.transport.get_extra_info('socket').setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,4096)
    reclaimed=False
    for _ in range(10):
        for _ in range(200):await left.send(frame(remote,b'q'*4096))
        await asyncio.sleep(1.05)
        with sqlite3.connect(db) as storage:
            reclaimed=storage.execute('SELECT COUNT(*) FROM relay_tickets WHERE machine_id=? AND used=1',(remote,)).fetchone()[0]==0
        if reclaimed:break
    check(reclaimed)
    print(kind,'slow-recipient grant released',flush=True)
    # The grant has already been released by server queue overflow. Abort this intentionally
    # uncooperative socket instead of draining a tiny-window TCP backlog for minutes.
    right.transport.abort();await asyncio.wait_for(right.wait_closed(),3);await left.close()
    left,_=await authenticated(a,[a,c]);right,_=await authenticated(b,[b,d])
    await left.send(frame(remote,b'after-backpressure'));check(await asyncio.wait_for(right.recv(),5)==frame(host,b'after-backpressure'))
    if kind=='ranked':
        subprocess.run([str(build/'cna-gamer-services-admin'),str(db),'revoke-user','dana'],check=True)
        await closed(right,1008)
    else:
        call('sessions.leave',{'session':session},b);await closed(right,1008)
    await left.send(frame(host,b'still-open'));check(left.state.name=='OPEN');await left.close()
    call('sessions.leave',{'session':session},a)
    print(kind,'verified WSS transport checks',checks,flush=True)


def main():
    build=pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='cna-relay-',dir=build) as temp:
        root=pathlib.Path(temp);os.chmod(root,0o700);db=root/'state.sqlite3';cert=root/'cert.pem';key=root/'key.pem'
        subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','1','-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost','-keyout',str(key),'-out',str(cert)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        admin=str(build/'cna-gamer-services-admin')
        for game in ('one','two'):subprocess.run([admin,str(db),'title',game,game],check=True,stdout=subprocess.DEVNULL)
        for name in ('alice','bob','charlie','dana','eve'):subprocess.run([admin,str(db),'user',name,name.title()],input=name+'-password\n',text=True,check=True,stdout=subprocess.DEVNULL)
        server=subprocess.Popen([str(build/'cna-gamer-services-server'),'--database',str(db),'--listen','127.0.0.1','--port','0','--cert',str(cert),'--key',str(key)],stdout=subprocess.PIPE,text=True)
        try:
            line=server.stdout.readline();check('listening' in line);url='https://localhost:'+line.strip().rsplit(':',1)[1]+'/cna/v1'
            credentials={}
            for game in ('one','two'):
                credentials[game]={}
                for name in ('alice','bob','charlie','dana','eve'):
                    result=request(url,cert,game,'auth.login',{'username':name,'password':name+'-password'})
                    check(result['error']=='OK');credentials[game][name]=result['result']
            for game,kind in (('one','player'),('two','ranked')):asyncio.run(exercise(build,root,db,cert,key,game,kind,url,credentials))
            check(server.poll() is None)
            print('Verified WSS forwarding/security passed',checks,'checks. No CNA ENet or Internet multiplayer claim.')
        finally:
            server.terminate();server.wait(timeout=10);server.stdout.close()

if __name__=='__main__':main()
