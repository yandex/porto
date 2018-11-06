import os
import json
import porto
import time
from test_common import *

conn = porto.Connection()

base = conn.Create("test-io-limit", weak=True)
v = conn.CreateVolume(containers=base.name)
base['cwd'] = v.path
base.Start()

fio_command='fio --name=test --size=100m --runtime=3 --time_based=1 --output-format=json '

io_read='io_read[/place]'
io_write='io_write[/place]'
io_rops='io_ops[/place r]'
io_wops='io_ops[/place w]'

porto_props=[io_read, io_write, io_rops, io_wops]

porto_result = dict()
fio_result = dict()

def run_fio(case):
    global porto_result, fio_result

    print fio_command, case

    porto_before=base.Get(porto_props)
    time_before = time.time()

    a = conn.Run('test-io-limit/test', command=fio_command + case, wait=60)

    time_after = time.time()
    porto_after=base.Get(porto_props)

    fio_result = json.loads(a['stdout'])['jobs'][0]
    a.Destroy()

    dt = time_after - time_before

    porto_result = dict()
    for k in porto_props:
        porto_result[k] = int(porto_after[k]) - int(porto_before.get(k, 0))

    print 'fio    read', fio_result['read']['bw'] / 1024., 'MB/s',
    print fio_result['read']['iops'], 'iops'
    print 'fio   write', fio_result['write']['bw'] / 1024., 'MB/s',
    print fio_result['write']['iops'], 'iops'

    print 'porto  read', porto_result[io_read] / 2.**20 / dt, 'MB/s',
    print porto_result[io_rops] / dt, 'iops'
    print 'porto write', porto_result[io_write] / 2.**20 / dt, 'MB/s',
    print porto_result[io_wops] / dt, 'iops',
    print
    print


print
print " - warmup"
print

run_fio('--rw=rw --end_fsync=1')


print
print " - io_limit = /place: 10M"
print

base['io_limit'] = "/place: 10M"

run_fio('--rw=read')
ExpectRange(fio_result['read']['bw'] / 1024, 0, 11)

run_fio('--rw=write --end_fsync=1')

run_fio('--rw=rw --end_fsync=1')

run_fio('--rw=read --direct=1')
ExpectRange(fio_result['read']['bw'] / 1024, 0, 11)

run_fio('--rw=write --direct=1')
ExpectRange(fio_result['write']['bw'] / 1024, 0, 11)

run_fio('--rw=rw --direct=1')

run_fio('--rw=write --fdatasync=1')
ExpectRange(fio_result['write']['bw'] / 1024, 0, 11)


print
print " - io_ops_limit = /place: 10"
print

base['io_limit'] = ""
base['io_ops_limit'] = "/place: 10"

run_fio('--rw=randread')
ExpectRange(fio_result['read']['iops'], 0, 11)

run_fio('--rw=randwrite --end_fsync=1')

run_fio('--rw=rw --end_fsync=1')

run_fio('--rw=randread --direct=1')
ExpectRange(fio_result['read']['iops'], 0, 11)

run_fio('--rw=randwrite --direct=1')
ExpectRange(fio_result['write']['iops'], 0, 11)

run_fio('--rw=randrw --direct=1')
ExpectRange(fio_result['read']['iops'], 0, 11)
ExpectRange(fio_result['write']['iops'], 0, 11)

run_fio('--rw=randwrite --fdatasync=1')
ExpectRange(fio_result['write']['iops'], 0, 11)
