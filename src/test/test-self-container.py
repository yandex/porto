import porto
import subprocess

conn = porto.Connection()
assert conn.GetData('self', 'absolute_name') == "/"
assert subprocess.check_output(['portoctl', 'exec', 'test', 'command=portoctl get self absolute_name']) == '/porto/test\n'
