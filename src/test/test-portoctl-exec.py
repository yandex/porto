import subprocess
import test_common
from test_common import *

DropPrivileges()

null = file('/dev/null', 'r+')

assert subprocess.call(['portoctl', 'exec', 'test', 'command=true'],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE) == 0

assert subprocess.call(['portoctl', 'exec', 'test', 'command=false'],
                       stderr=null, stdout=subprocess.PIPE) == 1

assert subprocess.check_output(['portoctl', 'exec', 'test', 'command=seq 3'],
                               stdin=subprocess.PIPE, stderr=subprocess.PIPE) == '1\n2\n3\n'
