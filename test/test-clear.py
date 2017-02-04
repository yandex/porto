#!/usr/bin/python -u

import os
import porto
import tarfile
import shutil
import time

from test_common import *

AsRoot()

TMPDIR="/tmp/test_clear"
LAYERS="/place/porto_layers"
NAME="test-layer"

WIDTHS = [1, 1, 1, 1, 1, 1, 12, 1, 1, 1, 1, 36, 1, 1, 1, 1, 1, 4, 1, 1, 4, 4, 0]

def CreateRecursive(path, k):
    if k >= len(WIDTHS):
        return

    width = WIDTHS[k]

    if width == 0:
        open(path + "/file.txt", "w").write(str(k))
    else:
        for i in range(0, WIDTHS[k]):
            new_path = path + "/" + str(i)
            os.mkdir(new_path)
            CreateRecursive(new_path, k + 1)

c = porto.Connection(timeout=10)

try:
    os.mkdir(TMPDIR)
except OSError:
    shutil.rmtree(TMPDIR)
    os.mkdir(TMPDIR)

open("{}/{}".format(TMPDIR, "file.txt"), "w").write("1234567890")

t = tarfile.open(name="{}/{}".format(TMPDIR, "layer.tar"), mode="w")
t.add("{}/{}".format(TMPDIR, "file.txt"), arcname = "file.txt")
t.close()

os.unlink("{}/{}".format(TMPDIR, "file.txt"))

BASE = "{}/{}".format(LAYERS, NAME)

if NAME in [layer.name for layer in c.ListLayers()]:
    print "Removing existing layer at: {}".format(BASE)
    shutil.rmtree(BASE)

c.ImportLayer(NAME, "{}/{}".format(TMPDIR, "layer.tar"))

assert os.stat(BASE) != None
assert os.stat("{}/file.txt".format(BASE)) != None

print "Preparing special directory tree of depth: {} ... ".format(len(WIDTHS))
start_ts = time.time()
CreateRecursive(BASE, 0)
stop_ts = time.time()
print "Prepare took {} s".format(stop_ts - start_ts)

creation = stop_ts - start_ts

print "Removing layer... "
start_ts = time.time()
c.RemoveLayer(NAME)
stop_ts = time.time()
print "Removal took {} s".format(stop_ts - start_ts)

removal = stop_ts - start_ts

os.unlink("{}/{}".format(TMPDIR, "layer.tar"))
os.rmdir(TMPDIR)
