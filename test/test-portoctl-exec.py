import subprocess
from test_common import *

cp = subprocess.run(['ldd', portoctl], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
deps = cp.stdout.decode('utf-8')
if len(deps) == 0:
    deps = cp.stderr.decode('utf-8')

ExpectEq(len(list(filter(len, deps.split('\n')))), 1)
ExpectEq(deps.find('libc'), -1)

ExpectEq(subprocess.call([portoctl, 'exec', 'test', 'command=true'],
                         stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE),
         0)

ExpectEq(subprocess.call([portoctl, 'exec', 'test', 'command=false'],
                         stderr=subprocess.DEVNULL,
                         stdout=subprocess.PIPE),
         1)

ExpectEq(subprocess.call([portoctl, 'exec', 'test'],
                         stdin=subprocess.DEVNULL,
                         stderr=subprocess.PIPE,
                         stdout=subprocess.PIPE),
         0)

ExpectEq(subprocess.check_output([portoctl, 'exec', 'test', 'command=seq 3'],
                                 stdin=subprocess.PIPE,
                                 stderr=subprocess.PIPE),
         b'1\n2\n3\n')
