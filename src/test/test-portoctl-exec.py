import subprocess

null = file('/dev/null', 'r+')

assert subprocess.call(['portoctl', 'exec', 'test', 'command=true']) == 0

assert subprocess.call(['portoctl', 'exec', 'test', 'command=false'], stderr=null) == 1

assert subprocess.check_output(['portoctl', 'exec', 'test', 'command=seq 3']) == '1\n2\n3\n'
