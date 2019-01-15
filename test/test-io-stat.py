import porto
from test_common import *
import os

conn = porto.Connection()

last = {}

a = conn.Create('a', weak=True)
v = conn.CreateVolume(containers='a')
a['cwd'] = v.path
a.Start()

def grow(prop):
    v = int(a[prop])
    d = v - last.get(prop, 0)
    last[prop] = v
    print "  ", prop, d
    return d

has_fs = os.path.exists("/sys/fs/cgroup/memory/memory.fs_bps_limit")

sane_blk = 'blkio,memory' in file('/proc/self/cgroup').read()

# direct io into hole goes throgh writeback
# without sane blk writeback goes to root cgroup

# io time works with cfq or with offstream patch
has_time = False
if os.path.exists("/sys/block/vda"):
    has_time = not os.path.exists("/sys/block/vda/mq")
else:
    has_time = not os.path.exists("/sys/fs/cgroup/blkio/blkio.bfq.io_service_bytes")
if os.path.exists("/sys/fs/cgroup/blkio/blkio.throttle.io_service_time_recursive"):
    has_time = True

def check_time(test=True):
    if has_time:
        if test:
            ExpectNe(grow('io_time[hw]'), 0)
            ExpectNe(grow('io_wait[hw]'), 0)
        else:
            grow('io_time[hw]')
            grow('io_wait[hw]')


print "- warmup 4k"
w = conn.Run('a/w', command="dd if=/dev/zero of=test bs=4k count=1 conv=fsync", wait=5)
w.Destroy()

grow('io_read[hw]')
grow('io_write[hw]')
grow('io_ops[hw]')

if has_fs:
    grow('io_read[fs]')
    grow('io_write[fs]')
    grow('io_ops[fs]')

if has_time:
    grow('io_time[hw]')
    grow('io_wait[hw]')


print "- write 4k"

w = conn.Run('a/w', command="dd if=/dev/zero of=test bs=4k count=1 conv=fsync", wait=5)
w.Destroy()

ExpectEq(grow('io_read[hw]'), 0)

if sane_blk:
    ExpectRange(grow('io_write[hw]'), 4096, 4096 * 10)
    ExpectRange(grow('io_ops[hw]'), 1, 10)
else:
    ExpectRange(grow('io_write[hw]'), 0, 4096 * 10)
    ExpectRange(grow('io_ops[hw]'), 0, 10)

check_time(sane_blk)

if has_fs:
    ExpectEq(grow('io_read[fs]'), 0)
    ExpectEq(grow('io_write[fs]'), 4096)
    ExpectEq(grow('io_ops[fs]'), 0) # Writeback


print "- rewrite 4k"

w = conn.Run('a/w', command="dd if=/dev/zero of=test bs=4k count=1 conv=notrunc conv=fsync", wait=5)
w.Destroy()

ExpectEq(grow('io_read[hw]'), 0)

if sane_blk:
    ExpectRange(grow('io_write[hw]'), 4096, 4096 * 2)
    ExpectRange(grow('io_ops[hw]'), 1, 2)
else:
    ExpectRange(grow('io_write[hw]'), 0, 4096 * 2)
    ExpectRange(grow('io_ops[hw]'), 0, 2)

check_time(sane_blk)

if has_fs:
    ExpectEq(grow('io_read[fs]'), 0)
    ExpectEq(grow('io_write[fs]'), 4096)
    ExpectEq(grow('io_ops[fs]'), 0) # Writeback


print "- dsync rewrite 4k"

w = conn.Run('a/w', command="dd if=/dev/zero of=test bs=4k count=1 conv=notrunc oflag=dsync", wait=5)
w.Destroy()

ExpectEq(grow('io_read[hw]'), 0)

if sane_blk:
    ExpectEq(grow('io_write[hw]'), 4096)
    ExpectRange(grow('io_ops[hw]'), 1, 2)
else:
    ExpectRange(grow('io_write[hw]'), 0, 4096)
    ExpectRange(grow('io_ops[hw]'), 0, 1)

check_time(sane_blk)

if has_fs:
    ExpectEq(grow('io_read[fs]'), 0)
    ExpectEq(grow('io_write[fs]'), 4096)
    ExpectEq(grow('io_ops[fs]'), 0) # Writeback


print "- direct write 4k"

w = conn.Run('a/w', command="dd if=/dev/zero of=test bs=4k count=1 oflag=direct", wait=5)
w.Destroy()

ExpectEq(grow('io_read[hw]'), 0)

if sane_blk:
    ExpectEq(grow('io_write[hw]'), 4096)
    ExpectEq(grow('io_ops[hw]'), 1)
else:
    ExpectRange(grow('io_write[hw]'), 0, 4096)
    ExpectRange(grow('io_ops[hw]'), 0, 1) # Hole

check_time(sane_blk)

if has_fs:
    ExpectEq(grow('io_read[fs]'), 0)
    ExpectEq(grow('io_write[fs]'), 4096)
    ExpectRange(grow('io_ops[fs]'), 0, 1) # Hole


print "- direct rewrite 4k"

w = conn.Run('a/w', command="dd if=/dev/zero of=test bs=4k count=1 oflag=direct conv=notrunc", wait=5)
w.Destroy()

ExpectEq(grow('io_read[hw]'), 0)
ExpectEq(grow('io_write[hw]'), 4096)
ExpectEq(grow('io_ops[hw]'), 1)

check_time()

if has_fs:
    ExpectEq(grow('io_read[fs]'), 0)
    ExpectEq(grow('io_write[fs]'), 4096)
    ExpectEq(grow('io_ops[fs]'), 1)


print "- direct read 4k"

w = conn.Run('a/w', command="dd if=test of=/dev/null bs=4k count=1 iflag=direct", wait=5)
w.Destroy()

ExpectEq(grow('io_read[hw]'), 4096)
ExpectEq(grow('io_write[hw]'), 0)
ExpectEq(grow('io_ops[hw]'), 1)

check_time()

if has_fs:
    ExpectEq(grow('io_read[fs]'), 4096)
    ExpectEq(grow('io_write[fs]'), 0)
    ExpectEq(grow('io_ops[fs]'), 1)


print "- cached read 4k"

w = conn.Run('a/w', command="dd if=test of=/dev/null bs=4k count=1", wait=5)
w.Destroy()

ExpectEq(grow('io_read[hw]'), 4096)
ExpectEq(grow('io_write[hw]'), 0)
ExpectEq(grow('io_ops[hw]'), 1)

check_time()

if has_fs:
    ExpectEq(grow('io_read[fs]'), 4096)
    ExpectEq(grow('io_write[fs]'), 0)
    ExpectEq(grow('io_ops[fs]'), 1)


print "- cached reread 4k"

w = conn.Run('a/w', command="dd if=test of=/dev/null bs=4k count=1", wait=5)
w.Destroy()

ExpectEq(grow('io_read[hw]'), 0)
ExpectEq(grow('io_write[hw]'), 0)
ExpectEq(grow('io_ops[hw]'), 0)

ExpectEq(grow('io_time[hw]'), 0)
ExpectEq(grow('io_wait[hw]'), 0)

if has_fs:
    ExpectEq(grow('io_read[fs]'), 0)
    ExpectEq(grow('io_write[fs]'), 0)
    ExpectEq(grow('io_ops[fs]'), 0)



print "- write 400k"

w = conn.Run('a/w', command="dd if=/dev/zero of=test bs=4k count=100 conv=fsync", wait=5)
w.Destroy()

ExpectEq(grow('io_read[hw]'), 0)

if sane_blk:
    ExpectRange(grow('io_write[hw]'), 409600, 409600 + 4096)
    ExpectRange(grow('io_ops[hw]'), 1, 100)
else:
    ExpectRange(grow('io_write[hw]'), 0, 409600 + 4096)
    ExpectRange(grow('io_ops[hw]'), 0, 100)

check_time(sane_blk)

if has_fs:
    ExpectEq(grow('io_read[fs]'), 0)
    ExpectEq(grow('io_write[fs]'), 409600)
    ExpectEq(grow('io_ops[fs]'), 0) # Writeback


print "- rewrite 400k"

w = conn.Run('a/w', command="dd if=/dev/zero of=test bs=4k count=100 conv=fsync conv=notrunc", wait=5)
w.Destroy()

ExpectEq(grow('io_read[hw]'), 0)

if sane_blk:
    ExpectRange(grow('io_write[hw]'), 409600, 409600 + 4096)
    ExpectRange(grow('io_ops[hw]'), 1, 100)
else:
    ExpectRange(grow('io_write[hw]'), 0, 409600 + 4096)
    ExpectRange(grow('io_ops[hw]'), 0, 100)

check_time(sane_blk)

if has_fs:
    ExpectEq(grow('io_read[fs]'), 0)
    ExpectEq(grow('io_write[fs]'), 409600)
    ExpectEq(grow('io_ops[fs]'), 0) # Writeback


print "- dsync rewrite 400k"

w = conn.Run('a/w', command="dd if=/dev/zero of=test bs=4k count=100 oflag=dsync conv=notrunc", wait=5)
w.Destroy()

ExpectEq(grow('io_read[hw]'), 0)

if sane_blk:
    ExpectRange(grow('io_write[hw]'), 409600, 409600)
    ExpectRange(grow('io_ops[hw]'), 100, 200)
else:
    ExpectRange(grow('io_write[hw]'), 0, 409600)
    ExpectRange(grow('io_ops[hw]'), 0, 100)

check_time(sane_blk)

if has_fs:
    ExpectEq(grow('io_read[fs]'), 0)
    ExpectEq(grow('io_write[fs]'), 409600)
    ExpectEq(grow('io_ops[fs]'), 0) # Writeback


print "- direct write 400k"

w = conn.Run('a/w', command="dd if=/dev/zero of=test bs=4k count=100 oflag=direct", wait=5)
w.Destroy()

ExpectEq(grow('io_read[hw]'), 0)

if sane_blk:
    ExpectEq(grow('io_write[hw]'), 409600)
    ExpectEq(grow('io_ops[hw]'), 100)
else:
    ExpectRange(grow('io_write[hw]'), 4096 * 99, 409600)
    ExpectRange(grow('io_ops[hw]'), 99, 100) # Hole

check_time(sane_blk)

if has_fs:
    ExpectEq(grow('io_read[fs]'), 0)
    ExpectEq(grow('io_write[fs]'), 409600)
    ExpectRange(grow('io_ops[fs]'), 99, 100) # Hole


print "- direct rewrite 400k"

w = conn.Run('a/w', command="dd if=/dev/zero of=test bs=4k count=100 oflag=direct conv=notrunc", wait=5)
w.Destroy()

ExpectEq(grow('io_read[hw]'), 0)
ExpectEq(grow('io_write[hw]'), 409600)
ExpectEq(grow('io_ops[hw]'), 100)

check_time()

if has_fs:
    ExpectEq(grow('io_read[fs]'), 0)
    ExpectEq(grow('io_write[fs]'), 409600)
    ExpectEq(grow('io_ops[fs]'), 100)


print "- direct read 400k"

w = conn.Run('a/w', command="dd if=test of=/dev/null bs=4k count=100 iflag=direct", wait=5)
w.Destroy()

ExpectEq(grow('io_read[hw]'), 409600)
ExpectEq(grow('io_write[hw]'), 0)
ExpectEq(grow('io_ops[hw]'), 100)

check_time()

if has_fs:
    ExpectEq(grow('io_read[fs]'), 409600)
    ExpectEq(grow('io_write[fs]'), 0)
    ExpectEq(grow('io_ops[fs]'), 100)


print "- cached read 400k"

w = conn.Run('a/w', command="dd if=test of=/dev/null bs=4k count=100", wait=5)
w.Destroy()

ExpectEq(grow('io_read[hw]'), 409600)
ExpectEq(grow('io_write[hw]'), 0)
ExpectRange(grow('io_ops[hw]'), 1, 100)

check_time()

if has_fs:
    ExpectEq(grow('io_read[fs]'), 409600)
    ExpectEq(grow('io_write[fs]'), 0)
    ExpectRange(grow('io_ops[fs]'), 1, 100)


print "- cached reread 400k"

w = conn.Run('a/w', command="dd if=test of=/dev/null bs=4k count=100", wait=5)
w.Destroy()

ExpectEq(grow('io_read[hw]'), 0)
ExpectEq(grow('io_write[hw]'), 0)
ExpectEq(grow('io_ops[hw]'), 0)

ExpectEq(grow('io_time[hw]'), 0)
ExpectEq(grow('io_wait[hw]'), 0)

if has_fs:
    ExpectEq(grow('io_read[fs]'), 0)
    ExpectEq(grow('io_write[fs]'), 0)
    ExpectEq(grow('io_ops[fs]'), 0)
