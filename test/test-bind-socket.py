#!/usr/bin/python

import porto
from test_common import *

conn = porto.Connection(timeout=30)

w = conn.Create("w", weak=True)

volume = conn.CreateVolume(backend='overlay', layers=['ubuntu-precise'], containers='w')

# run

def run(root, enable_porto, bind_socket):
    exit_code = 0
    if root != "/" and enable_porto == "false":
        exit_code = 2

    bind = ""
    if enable_porto == "isolate":
        bind = "/run/portod.socket /run/portod.socket"

    a = conn.Run("a", wait=1, root=root, command="ls /run/portod.socket", enable_porto=enable_porto, \
                 bind_socket=bind_socket)
    ExpectEq(a['bind_socket'].strip(), bind)
    ExpectEq(a['exit_code'], str(exit_code))
    a.Destroy()

for r in ("/", volume.path):
    for ep in ("isolate", "false"):
        for bs in ("", "/run/portod.socket /run/portod.socket"):
            run(root=r, enable_porto=ep, bind_socket=bs)

for r in ("/", volume.path):
    for ep in ("isolate", "false"):
        for bs in ("/tmp /tmp", "/tmp1 /tmp1"):
            ExpectException(run, porto.exceptions.InvalidPath, root=r, enable_porto=ep, bind_socket=bs)

# respawn

def respawn(root, enable_porto, new_enable_porto):
    bind = ""
    if enable_porto == "isolate":
        bind = "/run/portod.socket /run/portod.socket"

    new_bind = ""
    if new_enable_porto == "isolate":
        new_bind = "/run/portod.socket /run/portod.socket"

    a = conn.Run("a", wait=1, root=root, command="true", enable_porto=enable_porto)
    ExpectEq(a['bind_socket'].strip(), bind)
    ExpectEq(a['exit_code'], "0")
    a.Stop()

    a.SetProperty('enable_porto', new_enable_porto)
    a.Start()
    a.WaitContainer(timeout=1)
    ExpectEq(a['bind_socket'].strip(), new_bind)
    ExpectEq(a['exit_code'], "0")
    a.Destroy()

for r in ("/", volume.path):
    for ep in ("isolate", "false"):
        for nep in ("isolate", "false"):
            respawn(root=r, enable_porto=ep, new_enable_porto=nep)