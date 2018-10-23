import subprocess
from test_common import *

AsAlice()

ExpectEq(subprocess.call([portoctl, 'run', 'test-a', 'command=true']), 0)

ExpectEq(subprocess.check_output([portoctl, 'wait', 'test-a']), 'test-a\n')

ExpectEq(subprocess.check_output([portoctl, 'wait', 'test-*']), 'test-a\n')

ExpectEq(subprocess.call([portoctl, 'destroy', 'test-a']), 0)
