import subprocess
from test_common import *

output = subprocess.check_output([portoctl, 'exec', 'test',
                               'command=bash -c "' + portoctl + ' create self/test; ' +
                               portoctl + ' set self/test isolate false; ' +
                               portoctl + ' start self/test; bash -c \\"' +
                               portoctl + ' get self absolute_name; ' +
                               portoctl + ' attach self/test \\\$\\\$; ' +
                               portoctl + ' get self absolute_name\\""'],
                               stdin=subprocess.PIPE, stderr=subprocess.PIPE)
assert output == '/porto/test\n/porto/test/test\n', "unexpected output: " + output
