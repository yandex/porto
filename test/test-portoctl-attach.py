import subprocess
from test_common import *

assert subprocess.check_output([portoctl, 'exec', 'test', 'command=bash -c "' + portoctl + ' create test/test; ' + portoctl + ' set test/test isolate false; ' + portoctl +' start test/test; bash -c \\"' + portoctl + ' get self absolute_name; ' + portoctl + ' attach test/test \\\$\\\$; ' + portoctl + ' get self absolute_name\\""'],
                               stdin=subprocess.PIPE, stderr=subprocess.PIPE) == '/porto/test\n/porto/test/test\n'
