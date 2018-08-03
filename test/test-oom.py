#!/usr/bin/python

import os
import time
import porto
from test_common import *

c = porto.Connection()

r = c.Find('/')

total_oom = int(r['oom_kills_total'])


a = c.Run("a", command="true", memory_limit="64M", wait=1)

ExpectEq(a['state'], 'dead')
ExpectEq(a['exit_code'], '0')
ExpectEq(a['oom_killed'], False)
ExpectEq(a['oom_kills'], '0')
ExpectEq(a['oom_kills_total'], '0')
ExpectEq(r['oom_kills_total'], str(total_oom))

a.Destroy()


a = c.Run("a", command="bash -c 'while true; do stress -m 1 ; done'", memory_limit="64M", wait=1)

ExpectEq(a['state'], 'dead')
ExpectEq(a['exit_code'], '-99')
ExpectEq(a['oom_killed'], True)
ExpectEq(a['oom_kills'], '1')
ExpectEq(a['oom_kills_total'], '1')

total_oom += 1
ExpectEq(r['oom_kills_total'], str(total_oom))

a.Destroy()


a = c.Run("a", command="bash -c 'while true; do stress -m 1 ; done'", memory_limit="64M", oom_is_fatal=False)

a.Wait(timeout_s=1)

ExpectEq(a['state'], 'running')
ExpectNe(a['oom_kills'], '0')
ExpectNe(a['oom_kills'], '1')
ExpectNe(a['oom_kills_total'], '0')
ExpectNe(a['oom_kills_total'], '1')

ExpectNe(r['oom_kills_total'], str(total_oom))

a.Destroy()


total_oom = int(r['oom_kills_total'])


a = c.Run("a", command="stress -m 1", virt_mode="os", memory_limit="64M", wait=1)
ExpectEq(a['state'], 'dead')
ExpectEq(a['exit_code'], '-99')
ExpectEq(a['oom_killed'], True)
ExpectEq(a['oom_kills'], '1')
ExpectEq(a['oom_kills_total'], '1')

total_oom += 1
ExpectEq(r['oom_kills_total'], str(total_oom))

a.Destroy()


a = c.Run("a", command="stress -m 1", memory_limit="64M", respawn=True, max_respawns=2, respawn_delay=0.1)

time.sleep(1)
ExpectEq(a['state'], 'dead')
ExpectEq(a['respawn_count'], '2')
ExpectEq(a['exit_code'], '-99')
ExpectEq(a['oom_killed'], True)
ExpectEq(a['oom_kills'], '3')
ExpectEq(a['oom_kills_total'], '3')

total_oom += 3
ExpectEq(r['oom_kills_total'], str(total_oom))

a.Destroy()


a = c.Run("a", memory_limit="64M")
b = c.Run("a/b", command="stress -m 1", memory_limit="128M", wait=1)

ExpectEq(b['state'], 'dead')
ExpectEq(b['exit_code'], '-99')
ExpectEq(b['oom_killed'], True)

ExpectEq(a['state'], 'dead')
ExpectEq(a['exit_code'], '-99')
ExpectEq(a['oom_killed'], True)
ExpectEq(a['oom_kills_total'], '1')

ExpectEq(int(a['oom_kills']) + int(b['oom_kills']), 1)

# Race: Speculative OOM could be detected in a or a/b
time.sleep(1)

ExpectEq(a.GetProperty('oom_kills', sync=True), '0')
ExpectEq(b['oom_kills'], '1')
ExpectEq(b['oom_kills_total'], '1')

total_oom += 1
ExpectEq(r['oom_kills_total'], str(total_oom))

b.Destroy()
a.Destroy()



a = c.Run("a", memory_limit="128M")

b = c.Run("a/b", command="stress -m 1", memory_limit="64M", wait=1)

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


b.Stop()
b.Start()
b.WaitContainer(1)

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


b.Destroy()
b = c.Run("a/b", command="stress -m 1", memory_limit="64M", wait=1)

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
