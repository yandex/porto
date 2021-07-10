import subprocess
from test_common import *

AsAlice()

assert subprocess.call([portoctl, 'run', 'test-a', 'command=true']) == 0

assert subprocess.check_output([portoctl, 'wait', 'test-a']) == 'test-a\n'

assert subprocess.check_output([portoctl, 'wait', 'test-*']) == 'test-a\n'

assert subprocess.call([portoctl, 'destroy', 'test-a']) == 0

# test async wait with target state
assert subprocess.call([portoctl, 'run', 'test-a', 'command=sleep 1']) == 0
res = subprocess.check_output([portoctl, 'wait', '-A', '-S', 'dead'])
assert len(res.strip().split('\n')) == 1
assert res.count('dead\ttest-a\texit_code = 0') == 1

assert subprocess.call([portoctl, 'destroy', 'test-a']) == 0
