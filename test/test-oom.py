#!/usr/bin/python

import os
import time
import porto
from test_common import *

ConfigurePortod('test-oom', """
container {
    memory_high_limit_proportion: 1
}
""")

c = porto.Connection()
stress_memory = "bash -c 'while true; do stress -m 1 ; done'"

try:
    c.Destroy("test-oom")
except:
    pass

r = c.Find('/')

initial_oom_count = int(r['oom_kills_total'])

total_oom = initial_oom_count


# no oom

a = c.Run("test-oom", command="true", memory_limit="256M", wait=1)

ExpectEq(a['state'], 'dead')
ExpectEq(a['exit_code'], '0')
ExpectEq(a['oom_killed'], False)
ExpectEq(a['oom_kills'], '0')
ExpectEq(a['oom_kills_total'], '0')
ExpectEq(r['oom_kills_total'], str(total_oom))

a.Destroy()


# simple oom

a = c.Run("test-oom", command=stress_memory, memory_limit="256M", wait=5)

ExpectEq(a['state'], 'dead')
ExpectEq(a['exit_code'], '-99')
ExpectEq(a['oom_killed'], True)
ExpectEq(a['oom_kills'], '1')
ExpectEq(a['oom_kills_total'], '1')

total_oom += 1
ExpectEq(r['oom_kills_total'], str(total_oom))

a.Destroy()


# resore oom event

m = c.Run("test-oom", memory_limit="256M", weak=False)

ReloadPortod()

total_oom = initial_oom_count
ExpectEq(r['oom_kills_total'], str(total_oom))

a = c.Run("test-oom/a", command=stress_memory, wait=5)

ExpectEq(a['state'], 'dead')
ExpectEq(a['oom_killed'], True)
ExpectEq(a['exit_code'], '-99')

ExpectEq(a['oom_kills'], '0')
ExpectEq(a['oom_kills_total'], '0')

ExpectEq(m['state'], 'dead')
ExpectEq(m['oom_killed'], True)
ExpectEq(m['exit_code'], '-99')

ExpectEq(m['oom_kills'], '1')
ExpectEq(m['oom_kills_total'], '1')

total_oom += 1
ExpectEq(r['oom_kills_total'], str(total_oom))

m.Destroy()


# non fatal oom

a = c.Run("test-oom", command=stress_memory, memory_limit="256M", oom_is_fatal=False)

a.Wait(timeout_s=5)

ExpectEq(a['state'], 'running')
ExpectNe(a['oom_kills'], '0')
ExpectNe(a['oom_kills'], '1')
ExpectNe(a['oom_kills_total'], '0')
ExpectNe(a['oom_kills_total'], '1')

ExpectNe(r['oom_kills_total'], str(total_oom))

a.Destroy()


total_oom = int(r['oom_kills_total'])


# os move oom

a = c.Run("test-oom", command=stress_memory, virt_mode="os", memory_limit="256M", wait=5)
ExpectEq(a['state'], 'dead')
ExpectEq(a['exit_code'], '-99')
ExpectEq(a['oom_killed'], True)
ExpectEq(a['oom_kills'], '1')
ExpectEq(a['oom_kills_total'], '1')

total_oom += 1
ExpectEq(r['oom_kills_total'], str(total_oom))

a.Destroy()


# respawn after oom

a = c.Run("test-oom", command=stress_memory, memory_limit="256M", respawn=True, max_respawns=2, respawn_delay='2s')

while a['state'] != 'dead':
    a.Wait()
time.sleep(5)

ExpectEq(a['state'], 'dead')
ExpectEq(a['respawn_count'], '2')
ExpectEq(a['exit_code'], '-99')
ExpectEq(a['oom_killed'], True)
ExpectLe(2, int(a['oom_kills']))
ExpectLe(2, int(a['oom_kills_total']))

total_oom += 3
ExpectEq(r['oom_kills_total'], str(total_oom))

a.Destroy()


# oom at parent

a = c.Run("test-oom", memory_limit="256M")
b = c.Run("test-oom/b", command=stress_memory, memory_limit="512M", wait=5)

ExpectEq(b['state'], 'dead')
ExpectEq(b['exit_code'], '-99')
ExpectEq(b['oom_killed'], True)

ExpectEq(a['state'], 'dead')
ExpectEq(a['exit_code'], '-99')
ExpectEq(a['oom_killed'], True)
ExpectEq(a['oom_kills_total'], '1')

ExpectEq(int(a['oom_kills']) + int(b['oom_kills']), 1)

# Race: Speculative OOM could be detected in a or test-oom/b
#       (kernel stuff)
deadline = time.time() + 30
oom_speculative = 0

while time.time() < deadline and oom_speculative == 0:
    oom_speculative = int(a.GetProperty('oom_kills', sync=True)) + int(b.GetProperty('oom_kills', sync=True))
    time.sleep(1)

ExpectLe(1, oom_speculative)
ExpectLe(1, int(a['oom_kills_total']))

total_oom += 1
ExpectEq(r['oom_kills_total'], str(total_oom))

b.Destroy()
a.Destroy()


# oom at child

a = c.Run("test-oom", memory_limit="512M")

b = c.Run("test-oom/b", command=stress_memory, memory_limit="256M", wait=5)

ExpectEq(b['state'], 'dead')
ExpectEq(b['exit_code'], '-99')
ExpectEq(b['oom_killed'], True)
ExpectEq(b['oom_kills'], '1')
ExpectEq(b['oom_kills_total'], '1')

ExpectEq(a['state'], 'meta')
ExpectEq(a['oom_kills'], '0')
ExpectEq(a['oom_kills_total'], '1')

total_oom += 1
ExpectEq(r['oom_kills_total'], str(total_oom))


# second oom after restart

b.Stop()
b.Start()
b.WaitContainer(5)

ExpectEq(b['state'], 'dead')
ExpectEq(b['exit_code'], '-99')
ExpectEq(b['oom_killed'], True)
ExpectEq(b['oom_kills'], '1')
ExpectEq(b['oom_kills_total'], '2')

ExpectEq(a['state'], 'meta')
ExpectEq(a['oom_kills'], '0')
ExpectEq(a['oom_kills_total'], '2')

total_oom += 1
ExpectEq(r['oom_kills_total'], str(total_oom))


# third oom at child after recreate

b.Destroy()
b = c.Run("test-oom/b", command=stress_memory, memory_limit="256M", wait=5)

ExpectEq(b['state'], 'dead')
ExpectEq(b['exit_code'], '-99')
ExpectEq(b['oom_killed'], True)
ExpectEq(b['oom_kills'], '1')
ExpectEq(b['oom_kills_total'], '1')

ExpectEq(a['state'], 'meta')
ExpectEq(a['oom_kills'], '0')
ExpectEq(a['oom_kills_total'], '3')

total_oom += 1
ExpectEq(r['oom_kills_total'], str(total_oom))


b.Destroy()
a.Destroy()
