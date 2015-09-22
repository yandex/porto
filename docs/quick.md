# Basics

Specify arbitrary command to start a container:
```
# Create container, set command and start
$ portoctl run uname command="uname -a"
# Get current container state
$ portoctl get uname state
dead
# Get container stdout
$ portoctl get uname stdout
Linux ya 3.16.4-200.fc20.x86_64 #1 SMP Mon Oct 6 12:57:00 UTC 2014 x86_64 x86_64 x86_64 GNU/Linux
# Destroy container
$ portoctl destroy uname
```

By default, container is started in isolated PID name space (i.e. process pid is 1 and no host processes are visible):
```
$ portoctl run ps command="ps aux"
$ portoctl get ps stdout
USER       PID %CPU %MEM    VSZ   RSS TTY      STAT START   TIME COMMAND
stfomic+     1  0.0  0.0  17644  1068 ?        Rs   18:45   0:00 ps aux
$ portoctl destroy ps
```

By default, container shares host network:
```
$ portoctl run ip command="ip link show"
$ portoctl get ip stdout
1: lo: <LOOPBACK,UP,LOWER_UP> mtu 65536 qdisc noqueue state UNKNOWN mode DEFAULT group default
    link/loopback 00:00:00:00:00:00 brd 00:00:00:00:00:00
2: em1: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc htb state UP mode DEFAULT group default qlen 1000
    link/ether ec:f4:bb:53:1e:ed brd ff:ff:ff:ff:ff:ff
$ portoctl destroy ip
```

By default, container shares host filesystem:
```
$ portoctl run ls command="ls /"
$ portoctl get ls stdout
bin
boot
dev
etc
home
lib
lib64
lost+found
media
mnt
opt
place
proc
root
run
sbin
srv
sys
tmp
usr
var
$ portoctl destroy ls
```

Container user and group is set to the user and group of calling process:
```
$ id
uid=1000(stfomichev) gid=1000(stfomichev) groups=1000(stfomichev),1001(porto) context=unconfined_u:unconfined_r:unconfined_t:s0-s0:c0.c1023

$ portoctl create id
$ portoctl set id command id
# Make sure container has correct user and group
$ portoctl get id user
stfomichev
$ portoctl get id group
stfomichev
# Start container
$ portoctl start id
$ portoctl get id stdout
uid=1000(stfomichev) gid=1000(stfomichev) groups=1000(stfomichev),1001(porto) context=unconfined_u:unconfined_r:unconfined_t:s0-s0:c0.c1023
$ portoctl destroy id
```

Only root can start containers with arbitrary user/group:
```
$ portoctl run id command="id" user="daemon"
Can't set property: Permission (Only root can change this property)
$ sudo portoctl run id command="id" user="daemon" group="daemon"
$ portoctl get id stdout
uid=2(daemon) gid=2(daemon) groups=2(daemon) context=unconfined_u:unconfined_r:unconfined_t:s0-s0:c0.c1023
$ portoctl destroy id
Can't destroy container: Permission (Permission error)
$ sudo portoctl destroy id
```

# Properties

If for some reason container can't start, Porto returns errno of failed syscall. Use start\_errno to get it:
```
$ portoctl create invalid
$ portoctl set invalid command __invalid_command__
$ portoctl start invalid
Can't start container: Unknown (No such file or directory: execvpe(__invalid_command__))
$ portoctl get invalid start_errno
2
```

Container exit status is available via exit\_status data (format is the same as wait syscall, man 2 wait), portoctl get shows human readable status:
```
$ portoctl run true command="true"
$ portoctl run false command="false"
$ portoctl get true exit_status
Container exited with 0
$ portoctl get false exit_status
Container exited with 1
$ portoctl run sleep command="sleep 1000"
$ kill -9 $(portoctl get sleep root_pid)
$ portoctl get sleep exit_status
Container killed by signal 9
```
Be aware that if you start bash as command it does it's own exit code interpretation: http://www.tldp.org/LDP/abs/html/exitcodes.html

portoctl dget returns data without any post processing:
```
$ portoctl dget sleep exit_status
9
$ portoctl dget true exit_status
0
$ portoctl dget false exit_status
256
```

Porto shows whether container is killed by OOM:
```
$ portoctl get false oom_killed
false
```

Container environment, working directory and resource limits may be specified:
```
$ portoctl run env command="bash -c 'env | grep TEST_PORTO'" env="TEST_PORTO1=1; TEST_PORTO2=2"
$ portoctl get env stdout
TEST_PORTO2=2
TEST_PORTO1=1

$ portoctl run cwd command="pwd" cwd="/tmp"
$ portoctl get cwd stdout
/tmp

$ portoctl run ulimit command="bash -c 'builtin ulimit -a'" ulimit = "nproc: 20480 30720; nofile: 819200 1024000"
$ portoctl get ulimit stdout
core file size          (blocks, -c) 0
data seg size           (kbytes, -d) unlimited
scheduling priority             (-e) 0
file size               (blocks, -f) unlimited
pending signals                 (-i) 31490
max locked memory       (kbytes, -l) 64
max memory size         (kbytes, -m) unlimited
open files                      (-n) 819200
pipe size            (512 bytes, -p) 8
POSIX message queues     (bytes, -q) 819200
real-time priority              (-r) 0
stack size              (kbytes, -s) 8192
cpu time               (seconds, -t) unlimited
max user processes              (-u) 20480
virtual memory          (kbytes, -v) unlimited
```

PID of root container process can be obtained via root\_pid data:
```
$ portoctl run sleep command="sleep 1000"
$ portoctl get sleep root_pid
9354
$ ls -la /proc/9354/exe
lrwxrwxrwx. 1 stfomichev stfomichev 0 ноя 21 19:34 /proc/9354/exe -> /usr/bin/sleep
```

When container starts, Porto creates temporary files for stdout/stderr and stdin is /dev/null. Container stdin/stdout/stderr may be changed via stdin\_path, stdout\_path and stderr\_path properties.

# Isolation

Porto supports various degrees of isolation. By default, container can't see host/other container processes but runs in host filesystem and network namespace.

It's possible to start container without PID isolation (so it can see all host processes) via isolate property:
```
$ portoctl run ps command="bash -c 'ps aux | wc -l'" isolate=false
$ portoctl get ps stdout
334
$ portoctl destroy ps
```

In order to isolate container filesystem use root property:
```
$ mkdir /tmp/porto
$ portoctl run root command=ls root=/tmp/porto
Can't start property: Unknown (No such file or directory: execvpe(ls))
$ cat /tmp/bringup
CMD=$1
PREF=$2

link() { local d=$(dirname $1); mkdir -p $PREF/$d; cp $1 $PREF/$1; }
ldd $CMD | while read line; do
	f=$(echo $line | cut -d ' ' -f 3);
	if [ -e "$f" ]; then link $f; fi;
	f=$(echo $line | cut -d ' ' -f 1);
	if [ -e "$f" ]; then link $f; fi;
done
link $CMD
$ bash /tmp/bringup /bin/ls /tmp/porto/
$ ./portoctl run root command=ls root=/tmp/porto
$ ./portoctl get root stdout
bin
dev
etc
lib64
proc
stderr
stdout
sys
```

In order to make root of container readonly use root\_readonly property:
```
$ bash /tmp/bringup /bin/touch /tmp/porto/
$ portoctl run root command="touch a" root=/tmp/porto
$ portoctl get root exit_status
Container exited with 0
$ portoctl destroy root
$ portoctl run root command="touch a" root=/tmp/porto root_readonly=true
$ portoctl get root exit_status
Container exited with 1
```

It's possible to bind some host files into container if container still needs to access some host files when its filesystem is isolated:
```
$ bash /tmp/bringup /bin/cat /tmp/porto/
$ mkdir /tmp/porto2
$ echo hello > /tmp/porto2/hello
$ portoctl run bind command="cat /binded/hello" root=/tmp/porto bind="/tmp/porto2 /binded"
$ portoctl get bind stdout
hello
```

By default, Porto binds host /etc/hosts and /etc/resolv.conf files into container. This can be disabled with bind\_dns property.

There are also some low level properties like allowed\_devices and capabilities to start LXC-like containers (see also virt\_mode property).

Container hostname (UTS namespace) may be isolated using hostname property:
```
$ bash /tmp/bringup /bin/hostname /tmp/porto/
$ portoctl run hostname command=hostname hostname=porto root=/tmp/porto
$ portoctl get hostname stdout
porto
```

# Limits

Porto supports container resource limits. For full list of supported limits look at [limits page](limits.md).

# Hierarchy

Porto support hierarchical containers in two modes (to create child container use /):

1. Parent container is started first and runs for a long time; child containers are periodically created and destroyed. When parent container dies, all child containers are stopped:

        $ portoctl run parent command="sleep 1000"
        $ portoctl run parent/child command="sleep 100" isolate=false
        $ portoctl run parent/ps command="ps aux" isolate=false
        $ portoctl get parent state
        running
        $ portoctl get parent/child state
        running
        $ portoctl get parent/ps state
        dead
        $ portoctl get parent/ps stdout
        USER       PID %CPU %MEM    VSZ   RSS TTY      STAT START   TIME COMMAND
        stfomic+     1  0.0  0.0   4308   684 ?        Ss   16:27   0:00 sleep 1000
        stfomic+     2  0.0  0.0   4308   608 ?        Ss   16:27   0:00 sleep 100
        stfomic+     3  0.0  0.0  19744  1968 ?        Rs   16:27   0:00 ps aux
        $ portoctl stop parent
        $ portoctl get parent/child state
        stopped

    In this mode if child's container isolate property is set to false, it starts in the parent container namespaces (PID, filesystem, network).

2. Parent container is used to set total resource limit for a set of child containers. In this case, parent container is not started explicitly and it's mode is meta:

        $ portoctl create meta
        $ portoctl set meta memory_limit 1073741824
        $ portoctl run meta/child1 command="sleep 1000"
        $ portoctl run meta/child2 command="sleep 1000"
        $ portoctl get meta state
        meta
        $ portoctl get meta/child1 state
        running
        $ portoctl get meta/child2 state
        running
        $ portoctl stop meta
        $ portoctl get meta/child1 state
        stopped
        $ portoctl get meta/child2 state
        stopped

    Only memory\_limit, memory\_guarantee and recharge\_on\_pgfault limits are supported for meta containers.

Meta containers have different behavior depending on isolate property:
* isolate=true (default) - meta container starts dummy process that holds namespaces; child containers share this namespace if they have isolate=false.
* isolate=false - meta container doesn't have running process and used only to set total resource limits; child containers with isolate=false will not share parent namespace, they will use namespace of some container up in the hierarchy which has running process (or host namespace if no parent container has running process).

```
$ portoctl create meta
$ portoctl set meta isolate false
$ portoctl run meta/child1 isolate=false command="sleep 1000"
$ portoctl run meta/child2 isolate=false command="ps aux"
# child1 and child2 work in the host namespace and can see all processes
$ portoctl get meta/child2 stdout | wc -l
95
$ portoctl destroy meta

$ portoctl create meta
$ portoctl run meta/child1 isolate=false command="sleep 1000"
$ portoctl run meta/child2 isolate=false command="ps aux"
# child1 and child2 work in the parent namespace
$ portoctl get meta/child2 stdout
USER       PID %CPU %MEM    VSZ   RSS TTY      STAT START   TIME COMMAND
root         1  0.0  0.0   1696   260 ?        Ss   15:18   0:00 /place/porto/meta/portoinit
root         2  0.0  0.0   4312   352 ?        Ss   15:18   0:00 sleep 1000
root         3  0.0  0.0  15280  1132 ?        Rs   15:18   0:00 ps aux
$ portoctl destroy meta
```

# Respawn

By default, Porto doesn't restart dead containers. Using respawn property its possible to make Porto restart dead containers (delay is 1s). Maximum number of respawns is limited by max\_respawn property (unlimited by default, -1). respawn\_count data may be used to get number of container respawns.

# Statistics

Every container has different statistics available, for a full list go to [this page](properties.md).

portoctl top conveniently shows container statistics:
```
$ portoctl top
container    cpu_usage memory_usage major_faults minor_faults net_packets
meta/child1  1.41732ms         220K            0           82      em1: 0
meta/child2  1.05066ms         208K            0           83      em1: 0
```
