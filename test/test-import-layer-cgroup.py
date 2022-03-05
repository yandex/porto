from test_common import *
import porto
import os
import subprocess
import shutil
import tarfile

test_path = "/tmp/test"


def create_tar():
    with open(test_path + "/file.txt", "w") as f:
        f.write("Oh shit, here we go again")

    with tarfile.open(name=test_path + "/file_layer.tar", mode="w") as t:
        t.add(test_path + "/file.txt", arcname="file.txt")


def invoke(type, mem_cgroup):
    if type == "python":
        conn.ImportLayer("file_layer", test_path + "/file_layer.tar", place=v.place, mem_cgroup=mem_cgroup)
    elif type == "portoctl":
        subprocess.check_call(
            [portoctl, "layer", "-P", v.place, "-I", "file_layer", test_path + "/file_layer.tar", mem_cgroup])
    else:
        raise Exception("Unknown type")


def try_remove(layer):
    try:
        conn.RemoveLayer(layer)
    except:
        pass


def get_usage(mem_cgroup):
    return int(subprocess.check_output(["cat", "/sys/fs/cgroup/memory/{}/memory.usage_in_bytes".format(mem_cgroup)]))


def test(type, mem_cgroup=""):
    usage_helpers = get_usage("/portod-helpers")
    usage_w = get_usage("/porto%w")

    invoke(type, mem_cgroup)

    diff_helpers = get_usage("/portod-helpers") - usage_helpers
    diff_w = get_usage("/porto%w") - usage_w

    if mem_cgroup:
        Expect(diff_helpers <= 0)
        Expect(diff_w > 0)
    else:
        Expect(diff_helpers > 0)
        Expect(diff_w <= 0)

    try_remove("file_layer")


AsRoot()
conn = porto.Connection(timeout=30)
w = conn.Run('w', weak=True)

try:
    shutil.rmtree(test_path)
except:
    pass
os.mkdir(test_path)

v = conn.CreateVolume(path=test_path, backend="tmpfs", space_limit="1Mb", containers="w")

try:
    create_tar()
    test("python")
    test("portoctl")
    test("python", "/porto%w")
    test("portoctl", "/porto%w")

except Exception as ex:
    raise ex

finally:
    v.Destroy()
    try_remove("file_layer")
    shutil.rmtree(test_path)
