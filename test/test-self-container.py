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
ExpectEq(subprocess.check_output([portoctl, 'exec', 'test', 'porto_namespace=',
                                'command=' + portoctl + ' find 1'],
                                stdin=subprocess.PIPE, stderr=subprocess.PIPE), 'test\n')

ExpectEq(subprocess.check_output([portoctl, 'exec', 'test', 'porto_namespace=test/',
                                'command=' + portoctl + ' find 1'],
                                stdin=subprocess.PIPE, stderr=subprocess.PIPE), '/porto/test\n')

ExpectEq(subprocess.check_output([portoctl, 'exec', 'test', 'porto_namespace=',
                                'command=' + portoctl + ' exec self/test porto_namespace= command=\"' + portoctl + ' find 1\"'],
                                stdin=subprocess.PIPE, stderr=subprocess.PIPE), 'test/test\n')

ExpectEq(subprocess.check_output([portoctl, 'exec', 'test', 'porto_namespace=test/',
                                'command=' + portoctl + ' exec test porto_namespace= command=\"' + portoctl + ' find 1\"'],
                                stdin=subprocess.PIPE, stderr=subprocess.PIPE), 'test\n')

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
