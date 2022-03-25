from test_common import *
import porto
import os
import subprocess
import shutil
import tarfile

test_path = "/tmp/test"
script_path = test_path + "/_check.sh"


def create_tar():
    with open(test_path + "/file.txt", "w") as f:
        f.write("Oh shit, here we go again")

    with tarfile.open(name=test_path + "/file_layer.tar", mode="w") as t:
        t.add(test_path + "/file.txt", arcname="file.txt")


def invoke(type, mem_cgroup):
    # /portod-helpers is a default memory cgroup
    container = ""
    if mem_cgroup == "/porto%w":
        container = "w"
    if type == "python":
        conn.ImportLayer("file_layer", test_path + "/file_layer.tar", place=v.place, container=container)
    elif type == "portoctl":
        subprocess.check_call(
            [portoctl, "layer", "-P", v.place, "-I", "file_layer", test_path + "/file_layer.tar", container])
    else:
        raise Exception("Unknown type")

    try_remove("file_layer")


def try_remove(layer):
    try:
        conn.RemoveLayer(layer)
    except:
        pass


def test(mem_cgroup):
    # This script checks memory cgroup of his process
    open(script_path, 'w').write("""
    #!/bin/bash
    cgroup=$(cat /proc/self/cgroup | grep "memory" | awk -F ':' '{print $3}')
    if [ "$cgroup" = "%s" ]; then
        tar $@ 
    else
        exit 1
    fi
    """ % (mem_cgroup))
    os.chmod(script_path, 0o755)

    invoke("python", mem_cgroup)
    invoke("portoctl", mem_cgroup)


# We replace default tar to our custom script via porto configs
ConfigurePortod("test-import-layer-cgroup", """
daemon {
    tar_path: "%s" 
}
""" % (script_path))

AsRoot()

conn = porto.Connection(timeout=30)
w = conn.Run('w', weak=True)

try:
    shutil.rmtree(test_path)
except:
    pass
os.mkdir(test_path)

v = conn.CreateVolume(path=test_path, backend="tmpfs", space_limit="1Mb", containers="w")
create_tar()

try:
    test('/portod-helpers')
    test('/porto%w')

except Exception as ex:
    raise ex

finally:
    v.Destroy()
    try_remove("file_layer")
    shutil.rmtree(test_path)
    ConfigurePortod("test-import-layer-cgroup", "")
