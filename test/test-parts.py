from test_common import *

import subprocess
import porto
import sys

assert len(sys.argv) == 2

tests = subprocess.check_output(['ctest', '-N'])

test_names = []

for line in tests.split('\n'):
    line = line.strip().split(':')
    if len(line) == 2 and line[0].startswith('Test'):
        test_names.append(line[1].strip())


part1 = {'uid_handling':2,
         'oom':25,
         'tc-classes':10,
         'tc-rebuild':2,
         'locate-process':2,
         'hugetlb':2,
         'coredump':2,
         'htb-restore':2,
         'portod_cli':3,
         'stats':1,
         'net-sched1':200}

part2 = {'net-sched2':200,
         'mem-overcommit':1,
         'portod_stop':1}

part3 = {'cleanup_portod':1,
         'portotest':90,
         'cleanup_logs':1,
         'portod_start':1,
         'api':15,
         'python3-api':95,
         'wait':1,
         'python3-wait':1,
         'ct-state':2,
         'knobs':1,
         'std-streams':3,
         'labels':1,
         'ulimit':1,
         'place':1,
         'mount':1,
         'aufs':1,
         'symlink':1,
         'systemd':1,
         'os':6,
         'jobs':8,
         'portoctl-exec':1,
         'portoctl-wait':1,
         'portoctl-pull-sandbox':130,
         'self-container':2,
         'portoctl-attach':1}

part4 = {'leaks':180,
         'fuzzer':80}

part5 = {'fuzzer_soft':120,
         'unpriv-cred':1,
         'isolation':1,
         'security':11,
         'hijack':1,
         'net':6}

part6 = {'volume_places':65,
         'volume_links':3,
         'volume-restore':2,
         'mem-recharge':2,
         'mem_limit_total':1,
         'legacy-root-loop':2,
         'dirty-limit':1,
         'cpu_limit':20,
         'cpu-jail':20,
         'mem_limit':65,
         'volume_queue':100,
         'volume_sync':20}

part7 = {'volume_backends':55,
         'recovery':40,
         'prev_release_upgrade':50,
         'performance':115}

part8 = {'spec':4,
         'python3-spec':4,
         'python3-retriability':160,
         'docker':30}

for test in part1.keys() + part2.keys() + part3.keys() + part4.keys() + part5.keys() + part6.keys() + part7.keys() + part8.keys():
    test_names.remove(test)

for i in range(1, 9):
    test_names.remove('part{}'.format(i))

# Put remaining tests in part8 with default timeout 10s
for test in test_names:
    part8[test] = 10


def run_test(test_name, timeout):
    timeout += 50

    ATTEMPTS = 2
    for i in range(ATTEMPTS):
        # clean configs
        subprocess.check_call(['rm', '-rf', 'rm -rf /etc/portod.conf.d/*'])
        # restart portod to remove containers/volumes
        if test == 'portod_start':
            subprocess.check_call([portod, 'stop'])
        else:
            subprocess.check_call([portod, 'restart'])

        try:
            subprocess.check_call(['ctest', '-R', '^{}$'.format(test_name), '-V', '--timeout', str(timeout)])
            return
        except Exception as e:
            if i != ATTEMPTS - 1:
                continue
            else:
                raise e

for test, timeout in eval('part{}.items()'.format(sys.argv[1])):
    run_test(test, timeout)
    if test not in ['portod_stop', 'cleanup_portod']:
        c = porto.Connection()
        fatals = int(c.GetProperty('/', 'porto_stat[fatals]'))
        assert 0 == fatals
