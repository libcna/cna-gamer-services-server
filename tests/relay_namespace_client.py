# SPDX-License-Identifier: MIT
"""Executes the native probe after externally managed rootless NAT setup, without buffering secrets."""
import json, os, subprocess, sys

inode=os.stat("/proc/self/ns/net").st_ino
print("namespace-created",os.getpid(),inode,flush=True)
line=b""
while not line.endswith(b"\n"):
    byte=os.read(0,1)
    assert byte and len(line)<32,"namespace setup boundary"
    line+=byte
assert line==b"configured\n","namespace setup command"
interfaces=json.loads(subprocess.check_output(["ip","-j","-4","address","show"]))
addresses=[address["local"] for interface in interfaces for address in interface.get("addr_info",[]) if address["family"]=="inet"]
assert sorted(addresses)==["10.0.2.100","127.0.0.1"],"isolated IPv4 interfaces"
routes=json.loads(subprocess.check_output(["ip","-j","-4","route","show"]))
assert any(route.get("dst")=="default" and route.get("gateway")=="10.0.2.2" and route.get("dev")=="tap0" for route in routes),"NAT default route"
print("namespace-ready",inode,"10.0.2.100",flush=True)
os.execv(sys.argv[1],sys.argv[1:])
