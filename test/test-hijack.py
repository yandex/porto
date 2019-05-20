import os
import porto
import datetime
from test_common import *

def Hijack(iterations=1):
    c = porto.Connection()
    a = c.Run('test', command='sleep 86400', isolate='true', enable_porto='isolate')
    for i in range(1, iterations + 1):
        print('{} hijack iteration {}/{}'.format(datetime.datetime.now(), i, iterations))
        b = c.Run('test/hijack', command=os.getcwd() + '/hijack', isolate='false', stdout_path='/dev/fd/1', stderr_path='/dev/fd/2')
        b.Wait()
        assert b.GetProperty('exit_code') == "1"
        b.Destroy()
    a.Destroy()

ConfigurePortod('test-hijack', """
container {
    ptrace_on_start: true
}
"""
)

Hijack()

ConfigurePortod('test-hijack', "")
