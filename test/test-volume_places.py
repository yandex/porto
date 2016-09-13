#!/usr/bin/python

import os
import shutil
import tarfile
import time

import porto
from test_common import *

if os.getuid() != 0:
    SwitchRoot()

c = porto.Connection(timeout=300)

DIR = "/tmp/test-volumes"
PLACE_DIR = DIR + "-place"

try:
    shutil.rmtree(DIR)
except:
    pass

try:
    shutil.rmtree(PLACE_DIR)
except:
    pass

os.mkdir(DIR)
os.mkdir(PLACE_DIR)

#Prepare dirs and check non-compliant dirs
assert Catch(c.CreateVolume, path=None, place=PLACE_DIR) == porto.exceptions.InvalidValue

#Prepare dummy layer also
f = open(DIR + "/file.txt", "w")
f.write("1234567890")
f.close()
t = tarfile.open(name=DIR + "/file_layer.tar", mode="w")
t.add(DIR + "/file.txt", arcname="file.txt")
t.close()


os.mkdir(PLACE_DIR + "/porto_volumes")
assert Catch(c.ImportLayer, "file_layer",
             DIR + "/file_layer.tar", place=PLACE_DIR) == porto.exceptions.InvalidValue
assert Catch(c.CreateVolume, path=None,
             place=PLACE_DIR) == porto.exceptions.InvalidValue
os.mkdir(PLACE_DIR + "/porto_layers")
os.mkdir(DIR + "/a")

#Finally, checking functions
assert Catch(c.CreateVolume, path=DIR + "/a", place=PLACE_DIR, backend="native", layers=["ubuntu-precise"]) == porto.exceptions.LayerNotFound

v = c.CreateVolume(path=None, layers=["ubuntu-precise"], backend="plain")
v.Export(DIR + "/tmp_ubuntu_precise.tar")
c.ImportLayer("place-ubuntu-precise", DIR + "/tmp_ubuntu_precise.tar", place=PLACE_DIR)
assert Catch(c.FindLayer, "place-ubuntu-precise") == porto.exceptions.LayerNotFound

# Check MergeLayer
l = c.FindLayer("place-ubuntu-precise", place=PLACE_DIR)
l.Merge(DIR + "/file_layer.tar")

os.unlink(DIR + "/tmp_ubuntu_precise.tar")
v.Unlink("/")

#Should also fail because of foreign layer vise versa
assert Catch(c.CreateVolume, path=DIR + "/a", backend="native", layers=["place-ubuntu-precise"]) == porto.exceptions.LayerNotFound

#Check volume is working properly
v = c.CreateVolume(path=DIR + "/a", place=PLACE_DIR, backend="native", layers=["place-ubuntu-precise"])

place_volumes = os.listdir(PLACE_DIR + "/porto_volumes")
assert len(place_volumes) == 1

cont = c.Create("test")
cont.SetProperty("command", "bash -c \"echo -n 789987 > /123321.txt\"")
cont.SetProperty("root", v.path)
cont.Start()
cont.Wait()
cont.Stop()

f = open(PLACE_DIR + "/porto_volumes/" + place_volumes[0] + "/native/123321.txt", "r")
assert f.read() == "789987"

cont.SetProperty("command", "cat /file.txt")
cont.Start()
cont.Wait()
assert cont.Get(["stdout"])["stdout"] == "1234567890"
cont.Stop()

#Check what will be if we rename our place

assert len(c.ListLayers(place=PLACE_DIR)) == 1
os.renames(PLACE_DIR, PLACE_DIR+"1")
assert Catch(c.ListLayers, place=PLACE_DIR) == porto.exceptions.UnknownError
os.renames(PLACE_DIR + "1", PLACE_DIR)
assert len(c.ListLayers(place=PLACE_DIR)) == 1

v.Export(DIR + "/tmp_back_ubuntu_precise.tar")

Catch(c.RemoveLayer, "place-ubuntu-precise-tmp")
c.ImportLayer("place-ubuntu-precise-tmp", DIR + "/tmp_back_ubuntu_precise.tar")

v = c.CreateVolume(path=None, layers=["place-ubuntu-precise-tmp"])
v.Unlink("/")

#Check place cleanup

os.kill(int(open("/run/portoloop.pid", "r").read()), 2)
time.sleep(5)

c = porto.Connection(timeout=60)

assert len(c.ListLayers(place=PLACE_DIR)) == 1
assert len(c.ListVolumes()) == 0
place_volumes = os.listdir(PLACE_DIR + "/porto_volumes")
assert len(place_volumes) == 0

c.RemoveLayer("place-ubuntu-precise-tmp")
shutil.rmtree(DIR)
shutil.rmtree(PLACE_DIR)
