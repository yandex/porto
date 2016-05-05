import subprocess

assert subprocess.call(['portoctl', 'run', 'test-a', 'command=true']) == 0

assert subprocess.check_output(['portoctl', 'wait', 'test-a']) == 'test-a\n'

assert subprocess.call(['portoctl', 'destroy', 'test-a']) == 0
