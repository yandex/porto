import porto
import random
import subprocess
import time

from test_common import *
from threading import Thread

conn = porto.Connection()

ConfigurePortod('test-retriability', """
daemon {
    portod_shutdown_timeout: 1,
    request_handling_delay_ms: 3000,
}""")

def TestCmd(command, restart_portod=False, fail=False):
    failed = False

    def RunCmd():
        conn2 = porto.Connection(auto_reconnect=True)
        nonlocal failed
        try:
            eval('conn2.{}'.format(command))
            if fail:
                failed = True
        except:
            if not fail:
                failed = True
            else:
                failed = (sys.exc_info()[0] != porto.exceptions.SocketError)

    cmd = Thread(target=RunCmd)
    cmd.start()

    time.sleep(1)
    if restart_portod:
        porto_cmd = random.choice(['upgrade', 'reload'])
        print('Make ' + porto_cmd)
        subprocess.call([portod, porto_cmd])

    cmd.join()
    assert not failed


TestCmd('ListVolumes()')

TestCmd('ListVolumes()', True)

TestCmd("Create('abc')", True)

assert len(conn.ListContainers()) == 1

TestCmd("Destroy('abc')", True)
assert len(conn.ListContainers()) == 0

TestCmd('CreateVolume()')
TestCmd('CreateVolume()', True)

volumes = conn.ListVolumes()
assert len(volumes) == 2

TestCmd("UnlinkVolume('{}')".format(volumes[0]), True)

TestCmd("UnlinkVolume('{}')".format(volumes[1]))

v = conn.CreateVolume()
subprocess.call(['dd', 'if=/dev/urandom', 'of=' + str(v) + '/foo', 'bs=1M', 'count=512'])

subprocess.call(['rm', '-f', '/tmp/layer.tar.gz'])

# export layer and upgrade portod. It must finished success
TestCmd("ExportLayer('{}', place='{}', tarball='/tmp/layer.tar.gz')".format(v.path, v.place), True)

# import layer and upgrade portod. Helper must be killed after portod_shutdown_timeout
TestCmd("ImportLayer('layer', '/tmp/layer.tar.gz')", True, True)
assert Catch(conn.FindLayer, 'layer') == porto.exceptions.LayerNotFound

ConfigurePortod('test-retriability', "")
