#!/usr/bin/python

import os
import shutil
import tarfile
import time
import traceback
import porto
from test_common import *

AsRoot()

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

def Test():
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
    os.mkdir(PLACE_DIR + "/porto_storage")
    os.mkdir(DIR + "/a")

    #Finally, checking functions
    assert Catch(c.CreateVolume, path=DIR + "/a", place=PLACE_DIR, backend="native", layers=["ubuntu-precise"]) == porto.exceptions.LayerNotFound

    v = c.CreateVolume(path=None, layers=["ubuntu-precise"], backend="plain")
    v.Export(DIR + "/tmp_ubuntu_precise.tar")
    c.ImportLayer("place-ubuntu-precise", DIR + "/tmp_ubuntu_precise.tar", place=PLACE_DIR)
    assert Catch(c.FindLayer, "place-ubuntu-precise") == porto.exceptions.LayerNotFound

    l = c.FindLayer("place-ubuntu-precise", place=PLACE_DIR)

    assert l.GetPrivate() == ""
    l.SetPrivate("XXXX")
    assert l.GetPrivate() == "XXXX"

    l.Merge(DIR + "/file_layer.tar", private_value="YYYY")
    assert l.GetPrivate() == "YYYY"

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
    cont.Destroy()

    #prepare more dirs
    os.mkdir(PLACE_DIR + "/1")
    os.mkdir(PLACE_DIR + "/1/2")
    os.mkdir(PLACE_DIR + "/1/2/place1")
    os.mkdir(PLACE_DIR + "/1/2/place1/porto_layers")
    os.mkdir(PLACE_DIR + "/1/2/place1/porto_storage")

    #prepare test function
    def TestWildcards(container_name, place, allowed=True):
        cont = c.Create(container_name)
        cont.SetProperty("place", place)
        cont.SetProperty("command", portoctl + ' layer -P {}'.format(PLACE_DIR + "/1/2/place1"))
        if allowed:
            cont.Start()
            cont.Wait()
            assert not cont.Get(["stderr"])["stderr"]
        else:
            assert Catch(cont.Start) == porto.exceptions.Permission
        cont.Destroy()

    #Check wildcard
    cont = c.Create("test")
    cont.SetProperty("place", "***")
    cont.SetProperty("command", portoctl + ' layer -L')
    cont.Start()
    cont.Wait()
    assert "SocketError" not in cont.Get(["stderr"])["stderr"]
    cont.Destroy()

    TestWildcards("test", "/place/***")
    TestWildcards("test", "/place/***/***")
    TestWildcards("test", "/place/***/***/***")

    #Wildcard allowed only at the end of the place
    TestWildcards("test", "/place/***/place1", False)
    TestWildcards("test", "***/place", False)

    #Check wildcards in container hierarchy

    #prepare tester class
    class WildcarsInHierarchyTester(object):
        def __init__(self):
            self.parent_places = []
            self.allowed_child_places = []
            self.not_allowed_child_places = []

        def AddParentPlace(self, parent_place):
            self.parent_places.append(parent_place)

        def AddChildPlace(self, child_place, allowed=True):
            if allowed:
                self.allowed_child_places.append(child_place)
            else:
                self.not_allowed_child_places.append(child_place)

        def Test(self):
           for parent_place in self.parent_places:
               cont = c.Create("parent") 
               cont.SetProperty("place", parent_place)
               cont.SetProperty("command", ' sleep 5')
               cont.SetProperty("env", "PYTHONPATH=/porto/src/api/python")
               cont.Start()

               for place in self.allowed_child_places:
                   TestWildcards("parent/child", place)
               for place in self.not_allowed_child_places:
                   TestWildcards("parent/child", place, False)

               cont.Wait()
               assert not cont.Get(["stderr"])["stderr"]
               cont.Destroy()


    hierarchyTester = WildcarsInHierarchyTester()
    hierarchyTester.AddParentPlace(PLACE_DIR + "/1/2/***")
    hierarchyTester.AddParentPlace(PLACE_DIR + "/1/2/***/***")
    hierarchyTester.AddParentPlace(PLACE_DIR + "/1/2/***/***/***")

    hierarchyTester.AddChildPlace(PLACE_DIR + "/1/2/place1")
    hierarchyTester.AddChildPlace(PLACE_DIR + "/1/2/***")
    hierarchyTester.AddChildPlace(PLACE_DIR + "/1/2/***/***")
    hierarchyTester.AddChildPlace(PLACE_DIR + "/1/2/***/***/***")
    hierarchyTester.AddChildPlace(PLACE_DIR + "/1/2", False)
    hierarchyTester.AddChildPlace(PLACE_DIR + "/***", False)
    hierarchyTester.AddChildPlace("***", False)
    hierarchyTester.AddChildPlace("/place/***", False)
    hierarchyTester.AddChildPlace("***/place1", False)
    hierarchyTester.AddChildPlace("/place/***/place1", False)
    hierarchyTester.Test()

    hierarchyTester = WildcarsInHierarchyTester()
    hierarchyTester.AddParentPlace("***")
    hierarchyTester.AddParentPlace(PLACE_DIR + "***")
    hierarchyTester.AddParentPlace(PLACE_DIR + "***/***")
    hierarchyTester.AddParentPlace(PLACE_DIR + "***/***/***")

    hierarchyTester.AddChildPlace(PLACE_DIR + "/1/2/place1")
    hierarchyTester.AddChildPlace(PLACE_DIR + "/1/2/***")
    hierarchyTester.AddChildPlace(PLACE_DIR + "/1/2/***/***")
    hierarchyTester.AddChildPlace(PLACE_DIR + "/1/2/***/***/***")
    hierarchyTester.AddChildPlace(PLACE_DIR + "/1/2")
    hierarchyTester.AddChildPlace(PLACE_DIR + "/***")
    hierarchyTester.AddChildPlace(PLACE_DIR + "/***/***")
    hierarchyTester.AddChildPlace("***/place1", False)
    hierarchyTester.AddChildPlace("/place/***/place1", False)
    hierarchyTester.Test()

    #Check what will be if we rename our place

    assert len(c.ListLayers(place=PLACE_DIR)) == 1
    os.renames(PLACE_DIR, PLACE_DIR+"1")
    assert Catch(c.ListLayers, place=PLACE_DIR) == porto.exceptions.UnknownError
    os.renames(PLACE_DIR + "1", PLACE_DIR)
    assert len(c.ListLayers(place=PLACE_DIR)) == 1

    v.Export(DIR + "/tmp_back_ubuntu_precise.tar")

    v.Unlink()

    c.RemoveLayer("place-ubuntu-precise", place=PLACE_DIR)

    assert len(os.listdir(PLACE_DIR + "/porto_volumes")) == 0
    assert len(c.ListLayers(place=PLACE_DIR)) == 0
    assert len(os.listdir(PLACE_DIR + "/porto_layers")) == 0


ret = 0

try:
    Test()
except BaseException as e:
    print traceback.format_exc()
    ret = 1

for r in c.ListContainers():
    try:
        r.Destroy()
    except:
        pass

for v in c.ListVolumes():
    try:
        v.Unlink()
    except:
        pass

try:
    c.RemoveLayer("place-ubuntu-precise", place=PLACE_DIR)
except:
    pass

shutil.rmtree(DIR)
shutil.rmtree(PLACE_DIR)

sys.exit(ret)
