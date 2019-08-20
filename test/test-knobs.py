#!/usr/bin/python

import porto
import subprocess
import types
from test_common import *

AsRoot()

c = porto.Connection(timeout=10)
r = c.Create("test")

def SetProps(r, knobs):
    for k in knobs:
        try:
            r.SetProperty(k, knobs[k])
        except BaseException as e:
            print "Cannot set: \n <{}> \n with \n <{}> : \n {}".format(k, knobs[k], e)
            raise e


def VerifyProperties(r, knobs):
    for k in knobs:
        value = r.GetProperty(k)
        try:
            assert value == knobs[k]
        except BaseException as e:
            print "Assertion for {} : \n <{}> \n != \n <{}>".format(k, value, knobs[k])
            raise e

knobs = {
    "aging_time" : "3600",
    "bind" : "/var/log /newvar ;/tmp /tmp rw;/home /home ro",
    "capabilities" : "CHOWN;DAC_OVERRIDE;DAC_READ_SEARCH;FOWNER;FSETID;KILL;"\
                     "SETGID;SETUID;SETPCAP;LINUX_IMMUTABLE;NET_BIND_SERVICE;"\
                     "NET_BROADCAST;NET_ADMIN;NET_RAW;IPC_LOCK;IPC_OWNER;"\
                     "SYS_MODULE;SYS_RAWIO;SYS_CHROOT;SYS_PTRACE;SYS_PACCT;"\
                     "SYS_ADMIN;SYS_BOOT;SYS_NICE;SYS_RESOURCE;SYS_TIME;"\
                     "SYS_TTY_CONFIG;MKNOD;LEASE;AUDIT_WRITE;AUDIT_CONTROL;"\
                     "SETFCAP;MAC_OVERRIDE;MAC_ADMIN;SYSLOG;WAKE_ALARM;"\
                     "BLOCK_SUSPEND;AUDIT_READ",
    "command" : "bash -c \'echo $(sleep) | xargs -I%sdf echo %sdf\'",
    #"controllers" : "freezer;memory;cpu;cpuacct;net_cls;blkio;devices;hugetlb;cpuset",
    "cpu_guarantee" : "0.756c",
    "cpu_limit" : "0.9c",
    "cpu_policy" : "normal",
    "cpu_set" : "1-2,4",
    "cwd" : "/var/log/../../home",
    "devices" : "/dev/loop0 rwm /dev/loop_alice 0660 porto-alice porto-alice; "\
                "/dev/loop1 rwm /dev/loop_bob 0660 porto-bob porto-bob; ",
    "enable_porto" : "child-only",
    "env" : "A1=123;A2=321;B=D",
    "group" : "porto-bob",
    "hostname" : "hostname.local",
    "io_limit" : "200000",
    "io_ops_limit" : "50000",
    "io_policy" : "",
    "ip" : "eth0 1.1.1.1/32",
    "isolate" : True,
    "max_respawns" : "5",
    "memory_limit" : "268435456",
    "net" : "inherited",
    "net_guarantee" : "default: 0",
    "net_limit" : "default: 0",
    "owner_user" : "root",
    "owner_group" : "root",
    "porto_namespace" : "/porto",
    "private" : "123;321321   2323cv",
    "resolv_conf" : "nameserver 1.1.1.1",
    "respawn" : False,
    "root" : "/var/log/../../",
    "root_readonly" : True,
    "stderr_path" : "/place/porto/../stderr.txt",
    "stdin_path" : "/place/porto/../stdin.txt",
    "stdout_limit" : "4194304",
    "stdout_path" : "/place/porto/../stdout.txt",
    "ulimit" : "as: 1048576 1048576; core: 1024 1024; cpu: 1 1; data: 2097152 2097152; "\
             "fsize: 4096 4096; locks: 16 16; memlock: 4096 4096; "\
             "msgqueue: 8192 8192; nice: 10 15; "\
             "nofile: 819200 1024000; nproc: 20480 30720; rss: 65536 65536; "\
             "rtprio: 1024 1024; rttime: 10000 10000; sigpending: 10 10; "\
             "stack: 16384 16384; ",
    "umask" : "0777",
    "user" : "porto-alice",
    "virt_mode" : "os",
    "weak" : False
}

if os.access("/sys/fs/cgroup/memory/memory.recharge_on_pgfault", os.F_OK):
    knobs["recharge_on_pgfault"] = True

if os.access("/sys/fs/cgroup/memory/memory.low_limit_in_bytes", os.F_OK):
    knobs["memory_guarantee"] = "33554432"

if os.access("/sys/fs/cgroup/memory/portod/memory.anon.limit", os.F_OK):
    knobs["anon_limit"] = "134217728"

if os.access("/sys/fs/cgroup/memory/memory.dirty_limit_in_bytes", os.F_OK):
    knobs["dirty_limit"] = "67108864"

SetProps(r, knobs)
VerifyProperties(r, knobs)

subprocess.check_call([portod, "--verbose", "reload"])

c = porto.Connection(timeout=10)
r = c.Find("test")

VerifyProperties(r, knobs)

#FIXME: ipvlan modes can be unsupported; netns is likely unused - skip

Net = [ "none", "inherited", "steal eth0", "container test", "macvlan eth0 eth0 bridge 1400 11:22:33:44:55:66", "ipvlan eth0 eth0", "veth veth0 veth1 1400 22:33:44:55:66:77", "L3 eth0 eth0", "NAT eth0", "veth veth0 veth1;MTU veth0 1400" ]

for n in Net:
    SetProps(r, { "net" : n })
    VerifyProperties(r, { "net" : n })

r.Destroy()

c.Create("test")

for k in knobs:
    value = knobs[k]
    try:
        subprocess.check_call([portoctl, "set", "test", k, "%s" %(str(value).lower()\
                          if type(value) is bool else value)])
    except subprocess.CalledProcessError as e:
        print "Cannot set property {} with <{}> : {} : {}".format(k, value, e, e.output)
        raise e

for k in knobs:
    value = subprocess.check_output([portoctl, "get", "test", k]).rstrip("\n")
    try:
        if type(knobs[k]) is bool:
            assert value == str(knobs[k]).lower()
        else:
            assert value == knobs[k]
    except AssertionError as e:
        print "portoctl get {} result:\n <{}> \n != <{}> \n".format(k, value, knobs[k])
        raise e

c.Destroy("test")

subprocess.check_call([portod, "--verbose", "--discard", "reload"])
