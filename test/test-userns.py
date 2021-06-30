import porto
from test_common import *

conn = porto.Connection()

def CheckUserNs(userns=True, **kwargs):
    a = conn.Run('a', userns=userns, user='1044', wait=0, root_volume={'layers': ['ubuntu-xenial']}, **kwargs)

    b = conn.Run('a/b', wait=5, virt_mode='job', isolate=False, user='1044', command='mount -t tmpfs tmpfs /tmp')
    ExpectEq('0' if userns else '1', b['exit_code'])
    b.Destroy()

    a.Destroy()

CheckUserNs(userns=False)
CheckUserNs(virt_mode='app')
CheckUserNs(virt_mode='os')
