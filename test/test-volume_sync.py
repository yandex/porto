#!/usr/bin/python

import time
import threading
import porto
import subprocess
from test_common import *
import random

RestartPortod()

def vunlink(num):
    c = porto.Connection()
    try:
        c.CreateVolume(None, layers=['ubuntu-precise'], backend='plain')
    except Exception as e:
        return e

try:
    for i in xrange(1, 10):
        t = threading.Thread(target=vunlink, args=(i,))
        t.start()

        time.sleep(0.5 + random.randint(-4, 4) * 0.05)
        c = porto.Connection()
        try:
            c.UnlinkVolume('/place/porto_volumes/%d/volume' % i, '***')

        except (porto.exceptions.VolumeNotFound, porto.exceptions.VolumeNotReady):
            pass

        except Exception as e:
            raise e

        t.join()

finally:
    RestartPortod()
