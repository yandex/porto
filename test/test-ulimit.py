from test_common import *

import sys
import os
import porto
import subprocess

def ExpectUlimit(a, key, val):
    line = subprocess.check_output(['grep', key, "/proc/{}/limits".format(a['root_pid'])])
    ExpectEq(' '.join(line.split()), key + ' ' + val)

c = porto.Connection()

a = c.Run("a")
ExpectUlimit(a, 'Max cpu time', 'unlimited unlimited seconds')
ExpectUlimit(a, 'Max file size', 'unlimited unlimited bytes')
ExpectUlimit(a, 'Max data size', 'unlimited unlimited bytes')
ExpectUlimit(a, 'Max stack size', '8388608 unlimited bytes')
ExpectUlimit(a, 'Max core file size', '0 unlimited bytes') # our
ExpectUlimit(a, 'Max resident set', 'unlimited unlimited bytes')
# ExpectUlimit(a, 'Max processes', '') vary
ExpectUlimit(a, 'Max open files', '8192 1048576 files') # our
ExpectUlimit(a, 'Max locked memory', '8388608 unlimited bytes') # our
ExpectUlimit(a, 'Max address space', 'unlimited unlimited bytes')
ExpectUlimit(a, 'Max file locks', 'unlimited unlimited locks')
# ExpectUlimit(a, 'Max pending signals', '') vary
ExpectUlimit(a, 'Max msgqueue size', '819200 819200 bytes')
ExpectUlimit(a, 'Max nice priority', '0 0')
ExpectUlimit(a, 'Max realtime priority', '0 0')
ExpectUlimit(a, 'Max realtime timeout', 'unlimited unlimited us')
a.Destroy()

a = c.Run("a", memory_limit="1G")
ExpectUlimit(a, 'Max locked memory', '1056964608 unlimited bytes')
a.Destroy()

a = c.Run("a", memory_limit="1G", ulimit="memlock: 12345678")
ExpectUlimit(a, 'Max locked memory', '12345678 12345678 bytes')
a.Destroy()

b = c.Run("b", memory_limit="1G")

a = c.Run("b/a", memory_limit="1G")
ExpectUlimit(a, 'Max locked memory', '1056964608 unlimited bytes')
a.Destroy()

b.Destroy()


def check_core_ulimit(pid, unlimited_count):
    pr = subprocess.check_output(['prlimit', '-p', pid, '--core'])
    assert unlimited_count == pr.count("unlimited")

try:
    a = c.Run("a", weak=False, command='sleep 20', ulimit='core: unlimited')
    pid = a.GetProperty("root_pid")

    check_core_ulimit(pid, 2)

    subprocess.check_call(['prlimit', '-p', pid, '--core=0'])
    check_core_ulimit(pid, 0)

    ReloadPortod()

    check_core_ulimit(pid, 0)

finally:
    a.Destroy()
