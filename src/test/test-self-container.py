import porto
import subprocess
import test_common
from test_common import *

DropPrivileges()
conn = porto.Connection()
assert conn.GetData('self', 'absolute_name') == "/"
assert subprocess.check_output(['portoctl', 'exec', 'test', 'command=portoctl get self absolute_name'],
                                stdin=subprocess.PIPE, stderr=subprocess.PIPE) == '/porto/test\n'
