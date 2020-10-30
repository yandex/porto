import porto
import random
import subprocess
import time

from test_common import *
from threading import Thread

conn = porto.Connection()

ConfigurePortod('test-retriability', """
daemon {
    portod_shutdown_timeout: 2,
    request_handling_delay_ms: 6000,
}""")

def TestCmd(command, restart_portod=False):
    failed = False

    def RunCmd():
        conn2 = porto.Connection(auto_reconnect=True)
        try:
            eval('conn2.{}'.format(command))
        except:
            nonlocal failed
            failed = True

    cmd = Thread(target=RunCmd)
    cmd.start()

    time.sleep(2)
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

ConfigurePortod('test-retriability', "")
