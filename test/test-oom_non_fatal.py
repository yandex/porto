#!/usr/bin/python

import os
import porto
from test_common import *

NAME = os.path.basename(__file__)

ANON_LIMIT_MB = 128
LIMIT_MB = 256
DURATION = 2000 #ms

c = porto.Connection(timeout=30)
ct = c.CreateWeakContainer(NAME + "-ct")

print "\nChecking event accounting for not oom_is_fatal containers"

print "\nChecking non-fatal OOM inside"

ct.SetProperty("command", "bash -c \'stress -q -m 1 --vm-bytes 256M; sleep 1;"\
                          "stress -q -m 1 --vm-bytes 256M; sleep 1; echo -n OK;\'")

ct.SetProperty("memory_limit", "256M")
ct.SetProperty("anon_limit", "128M")
ct.SetProperty("oom_is_fatal", "false")
ct.Start()

assert ct.Wait(DURATION * 2) == ct.name, "container running time twice exceeded "\
                                         "expected duration {}".format(DURATION)

ExpectPropGe(ct, "porto_stat[container_oom]", 2)
ExpectProp(ct, "stdout", "OK")

ct.Stop()

print "\nChecking not oom_is_fatal app OOM"

ct.SetProperty("command", "stress -q -m 1 --vm-bytes 256M")
ct.Start()

assert ct.Wait(DURATION * 2) == ct.name, "container running time twice exceeded "\
                                         "expected duration {}".format(DURATION)

ExpectPropGe(ct, "porto_stat[container_oom]", 1)
ct.Stop()

print "\nChecking not oom_is_fatal init OOM"

ct.SetProperty("virt_mode", "os")
ct.Start()

assert ct.Wait(DURATION * 2) == ct.name, "container running time twice exceeded "\
                                         "expected duration {}".format(DURATION)

ExpectPropGe(ct, "porto_stat[container_oom]", 1)
