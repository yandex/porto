import porto
import subprocess
from test_common import *

AsAlice()
conn = porto.Connection()
ExpectEq(conn.GetData('self', 'absolute_name'), "/")

# First level
ExpectEq(subprocess.check_output([portoctl, 'exec', 'test', 'command=' + portoctl + ' get self absolute_name'],
                                stdin=subprocess.PIPE, stderr=subprocess.PIPE), '/porto/test\n')

ExpectEq(subprocess.check_output([portoctl, 'exec', '/porto/test', 'command=' + portoctl + ' get self absolute_name'],
                                stdin=subprocess.PIPE, stderr=subprocess.PIPE), '/porto/test\n')

ExpectEq(subprocess.check_output([portoctl, 'exec', 'self/test', 'command=' + portoctl + ' get self absolute_name'],
                                stdin=subprocess.PIPE, stderr=subprocess.PIPE), '/porto/test\n')

# Second level
ExpectEq(subprocess.check_output([portoctl, 'exec', 'test', 'porto_namespace=',
                                'command=' + portoctl + ' exec test/test command=\"' + portoctl + ' get self absolute_name\"'],
                                stdin=subprocess.PIPE, stderr=subprocess.PIPE), '/porto/test/test\n')

ExpectEq(subprocess.check_output([portoctl, 'exec', '/porto/test', 'porto_namespace=',
                                'command=' + portoctl + ' exec /porto/test/test command=\"' + portoctl + ' get self absolute_name\"'],
                                stdin=subprocess.PIPE, stderr=subprocess.PIPE), '/porto/test/test\n')

ExpectEq(subprocess.check_output([portoctl, 'exec', 'self/test', 'porto_namespace=',
                                'command=' + portoctl + ' exec self/test command=\"' + portoctl + ' get self absolute_name\"'],
                                stdin=subprocess.PIPE, stderr=subprocess.PIPE), '/porto/test/test\n')

# Namespace
ExpectEq(subprocess.check_output([portoctl, 'exec', 'test', 'porto_namespace=test/',
                                'command=' + portoctl + ' exec test command=\"' + portoctl + ' get self absolute_name\"'],
                                stdin=subprocess.PIPE, stderr=subprocess.PIPE), '/porto/test/test\n')

ExpectEq(subprocess.check_output([portoctl, 'exec', 'test', 'porto_namespace=test/',
                                'command=' + portoctl + ' exec self/test command=\"' + portoctl + ' get self absolute_name\"'],
                                stdin=subprocess.PIPE, stderr=subprocess.PIPE), '/porto/test/test\n')

ExpectEq(subprocess.check_output([portoctl, 'exec', 'test', 'porto_namespace=test/',
                                'command=' + portoctl + ' exec /porto/test/test command=\"' + portoctl + ' get self absolute_name\"'],
                                stdin=subprocess.PIPE, stderr=subprocess.PIPE), '/porto/test/test\n')

# Find

tmp_path = '/tmp/find_script.sh'

try:
	f = open(tmp_path, "w")
	f.write("#!/bin/bash\nsleep 1\n" + portoctl + ' find 1\n')
	f.close()
	os.chmod(tmp_path, 0755)

	ExpectEq(subprocess.check_output([portoctl, 'exec', 'test', 'porto_namespace=',
                                'command=bash -c \"sleep 1 ; ' + portoctl + ' find 1\"'],
                                stdin=subprocess.PIPE, stderr=subprocess.PIPE), 'self\n')

	ExpectEq(subprocess.check_output([portoctl, 'exec', 'test', 'porto_namespace=test/',
                                'command=bash -c \"sleep 1 ; ' + portoctl + ' find 1\"'],
                                stdin=subprocess.PIPE, stderr=subprocess.PIPE), 'self\n')

	ExpectEq(subprocess.check_output([portoctl, 'exec', 'test', 'porto_namespace=',
                                'command=' + portoctl + ' exec self/test porto_namespace= command=\"' + tmp_path + '\"'],
                                stdin=subprocess.PIPE, stderr=subprocess.PIPE), 'self\n')

	ExpectEq(subprocess.check_output([portoctl, 'exec', 'test', 'porto_namespace=test/',
                                'command=' + portoctl + ' exec test porto_namespace= command=\"' + tmp_path + '\"'],
                                stdin=subprocess.PIPE, stderr=subprocess.PIPE), 'self\n')
finally:
	os.unlink(tmp_path)

# Isolate
ExpectEq(subprocess.check_output([portoctl, 'exec', 'test', 'enable_porto=isolate',
                                'command=' + portoctl + ' get self absolute_namespace'],
                                stdin=subprocess.PIPE, stderr=subprocess.PIPE), '/porto/test/\n')

ExpectEq(subprocess.check_output([portoctl, 'exec', 'test',
                                'command=' + portoctl + ' exec self/test enable_porto=isolate command=\"' + portoctl + ' get self absolute_namespace\"'],
                                stdin=subprocess.PIPE, stderr=subprocess.PIPE), '/porto/test/test/\n')

ExpectEq(subprocess.check_output([portoctl, 'exec', 'test', 'enable_porto=isolate',
                                'command=' + portoctl + ' exec test command=\"' + portoctl + ' get self absolute_name\"'],
                                stdin=subprocess.PIPE, stderr=subprocess.PIPE), '/porto/test/test\n')

ExpectEq(subprocess.check_output([portoctl, 'exec', 'test', 'enable_porto=isolate',
                                'command=' + portoctl + ' exec self/test command=\"' + portoctl + ' get self absolute_name\"'],
                                stdin=subprocess.PIPE, stderr=subprocess.PIPE), '/porto/test/test\n')

ExpectEq(subprocess.check_output([portoctl, 'exec', 'test', 'enable_porto=isolate',
                                'command=' + portoctl + ' exec /porto/test/test command=\"' + portoctl + ' get self absolute_name\"'],
                                stdin=subprocess.PIPE, stderr=subprocess.PIPE), '/porto/test/test\n')

# Isolate/Self-isolate
ExpectEq(Catch(subprocess.check_output, [portoctl, 'exec', 'test', 'enable_porto=isolate',
                                    'command=' + portoctl + ' set self private test'],
                                    stdin=subprocess.PIPE, stderr=subprocess.PIPE),
                                    subprocess.CalledProcessError)

ExpectEq(Catch(subprocess.check_output, [portoctl, 'exec', 'test', 'enable_porto=self-isolate',
                                    'command=' + portoctl + ' set self private test'],
                                    stdin=subprocess.PIPE, stderr=subprocess.PIPE), None)
