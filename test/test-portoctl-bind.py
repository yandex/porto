from test_common import *
import porto
import subprocess

conn = porto.Connection(timeout=30)
w = conn.Create("w", weak=True)
v = conn.CreateVolume(layers=["ubuntu-xenial"], containers='w')

def test(enable_porto, command, stdout, **kwargs):
    exit_code = 0
    if enable_porto == 'false':
        exit_code = 2

    a = conn.Run('a', wait=1, root=v.path, command=command,
                 enable_porto=enable_porto, weak=True, **kwargs)
    ExpectEq(a['exit_code'], str(exit_code))
    ExpectEq(a['stdout'], stdout)
    a.Destroy()

# volume is the same so we check enable_porto=false initially
test('false', 'ls /usr/sbin/portoctl', '')
ExpectException(test, porto.exceptions.InvalidCommand, 'false', 'portoctl --version', '')
test('isolate', 'ls /usr/sbin/portoctl', '/usr/sbin/portoctl\n', root_readonly=True)
test('isolate', 'ls /usr/sbin/portoctl', '/usr/sbin/portoctl\n')
test('isolate', 'portoctl --version', subprocess.check_output(['portoctl', '--version']).decode("utf-8"))
