import os
import subprocess
import porto
import shutil
import time
from test_common import *

if os.getuid() != 0:
    SwitchRoot()

TMPDIR = "/tmp/test-release-upgrade"
PORTOD_PATH = "/run/portod"

try:
    os.mkdir(TMPDIR)
except:
    pass

#Check  working with older version

print "Checking upgrade from the older version..."

cwd=os.path.abspath(os.getcwd())

os.chdir(TMPDIR)
try:
    #FIXME: Remove 2.10 suffix later
    download = subprocess.check_output(["apt-get", "download", "yandex-porto=2.10*"])
except:
    print "Cannot download old version of porto, skipping test..."
    os.chdir(cwd)
    os.rmdir(TMPDIR)
    sys.exit(0)

print "Package successfully downloaded"

subprocess.check_call([portod, "stop"])

downloads = download.split()
pktname = downloads[2] + "_" + downloads[3] + "_amd64.deb"

os.mkdir("old")
subprocess.check_call(["dpkg", "-x", pktname, "old"])
os.unlink(pktname)

os.chdir(cwd)

os.unlink(PORTOD_PATH)
os.symlink(TMPDIR + "/old/usr/sbin/portod", PORTOD_PATH)
subprocess.check_call([PORTOD_PATH + "&"], shell=True)
time.sleep(1)

oldver = subprocess.check_output([portod, "--version"]).split()[6]

c = porto.Connection(timeout=3)
c.Create("test")
c.SetProperty("test", "command", "sleep 5")
c.Start("test")
c.Create("test2")
c.SetProperty("test2", "command", "bash -c 'sleep 20 && echo 456'")
c.Start("test2")
c.disconnect()

os.unlink(PORTOD_PATH)
os.symlink(portod, PORTOD_PATH)
subprocess.check_call([portod, "reload"])
time.sleep(1)

assert subprocess.check_output([portod, "--version"]).split()[6] != oldver
#That means we've upgraded successfully

c = porto.Connection(timeout=3)
c.Wait(["test"])

#Checking if we can create subcontainers successfully (cgroup migration involved)

r = c.Create("a")
r.SetProperty("command", "bash -c '" + portoctl + " run a/a command=\"echo 123\" && sleep 2'")
r.Start()
assert r.Wait() == "a"
assert r.GetProperty("exit_status") == "0"

r2 = c.Find("a/a")
r2.Wait() == "a/a"
assert r2.GetProperty("exit_status") == "0"
assert r2.GetProperty("stdout") == "123\n"
r2.Destroy()
r.Destroy()

assert c.GetProperty("test", "exit_status") == "0"
c.disconnect()

#Now, the downgrade
os.unlink(PORTOD_PATH)
os.symlink(TMPDIR + "/old/usr/sbin/portod", PORTOD_PATH)

subprocess.check_call([portod, "reload"])
time.sleep(1)

assert subprocess.check_output([portod, "--version"]).split()[6] == oldver

c = porto.Connection(timeout=3)
c.Wait(["test2"])
assert c.GetProperty("test2", "stdout") == "456\n"
assert c.GetProperty("test2", "exit_status") == "0"
c.disconnect()

subprocess.check_call([portod, "restart"])

shutil.rmtree(TMPDIR)
