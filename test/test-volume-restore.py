#!/usr/bin/python -u

import os
import porto
import sys
from test_common import *

c = porto.Connection()

v = c.CreateVolume()
a = c.Run("a", weak=False)
v.Link(a)
os.unlink("/run/porto/kvs/" + a['id'])
ReloadPortod()
ExpectEq(Catch(c.Find, "a"), porto.exceptions.ContainerDoesNotExist)
c.FindVolume(v.path)
v.Unlink()
