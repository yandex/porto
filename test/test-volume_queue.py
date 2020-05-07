#!/usr/bin/python

import os
import shutil
import tarfile
import time
import traceback
import porto
from test_common import *
from multiprocessing import Process

DIR = "/tmp/test-volume_queue"
TAR_PATH = DIR + "/sleepy_tar.sh"

def process_func():
    ExpectEq(Catch(c.ImportLayer, "file_layer2", DIR + "/file_layer.tar"), None)

# check that volume requests are handled in a separate queue,
# that is independent from the layer queue
def Test():
    #Prepare dummy layer
    f = open(DIR + "/file.txt", "w")
    f.write("1234567890")
    f.close()
    t = tarfile.open(name=DIR + "/file_layer.tar", mode="w")
    t.add(DIR + "/file.txt", arcname="file.txt")
    t.close()

    # import first layer
    ExpectEq(Catch(c.ImportLayer, "file_layer1", DIR + "/file_layer.tar"), None)

    # create 'sleepy_tar.sh'
    open(TAR_PATH, "w").write("""
#!/bin/bash

sleep 5
tar "$@"
"""
    );
    os.chmod(TAR_PATH, 0744)

    # shrink io-workers count and create delay on layer unpacking
    ConfigurePortod('test-volume_queue', """
daemon {
	io_threads: 1
	tar_path: "%s"
}
""" % TAR_PATH)

    # start importing second layer
    process = Process(target = process_func)
    process.start()

    backoff = 0.1 # sec
    timeout = 10  # sec
    timeout_steps = int(timeout / backoff)
    # wait until porto will start importing 2nd layer
    while not (os.path.exists("/place/porto_layers/_import_file_layer2")):
        if os.path.exists("/place/porto_layers/file_layer2"):
            raise Exception("Porto somehow completed creation of second layer earlier than planned")

        time.sleep(backoff)
        timeout_steps -= 1
        if (timeout_steps == 0):
            raise Exception("Timeout exceeded!")

    # check that volume creation handled in a separate queue...
    ExpectEq(Catch(c.CreateVolume, backend="native", space_limit="100M"), None)
    # ...and that the first layer is available
    ExpectEq(Catch(c.CreateVolume, layers=["file_layer1"]), None)

    # check that creation of second layer is delayed and queued in io-queue (since there is only one worker-thread)
    ExpectEq(Catch(c.CreateVolume, layers=["file_layer2"]), porto.exceptions.LayerNotFound)

    process.join()
    # check than second layer can be successfully used after forked process finished
    ExpectEq(Catch(c.CreateVolume, layers=["file_layer2"]), None)

AsRoot()

c = porto.Connection(timeout=300)

try:
    shutil.rmtree(DIR)
except:
    pass

os.mkdir(DIR)

ret = 0

try:
    Test()
except BaseException as e:
    print traceback.format_exc()
    ret = 1

ConfigurePortod('test-volume_queue', "")

for v in c.ListVolumes():
    try:
        v.Unlink()
    except:
        pass

try:
    c.RemoveLayer("file_layer1")
    c.RemoveLayer("file_layer2")
except:
    pass

shutil.rmtree(DIR)

sys.exit(ret)
