import porto
import subprocess
import os

conn = porto.Connection()
assert conn.GetData('self', 'absolute_name') == "/"
assert subprocess.check_output(['./portoctl', 'exec', 'test', 'command=' + os.getcwd() + '/portoctl get self absolute_name'],
                                stdin=subprocess.PIPE, stderr=subprocess.PIPE) == '/porto/test\n'
