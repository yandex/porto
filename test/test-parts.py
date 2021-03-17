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


part1 = ['uid_handling',
         'oom',
         'tc-classes',
         'tc-rebuild',
         'locate-process',
         'hugetlb',
         'coredump',
         'htb-restore',
         'portod_cli',
         'stats',
         'net-sched1']

part2 = ['net-sched2',
         'mem-overcommit',
         'portod_stop',]

part3 = ['cleanup_portod',
         'portotest',
         'cleanup_logs',
         'portod_start',
         'api',
         'python3-api',
         'wait',
         'python3-wait',
         'ct-state',
         'knobs',
         'std-streams',
         'labels',
         'ulimit',
         'place',
         'mount',
         'aufs',
         'symlink',
         'systemd',
         'os',
         'jobs',
         'portoctl-exec',
         'portoctl-wait',
         'self-container',
         'portoctl-attach']

part4 = ['leaks',
         'fuzzer']

part5 = ['fuzzer_soft',
         'unpriv-cred',
         'isolation',
         'security',
         'hijack',
         'net']

part6 = ['volume_places',
         'volume_links',
         'volume-restore',
         'mem-recharge',
         'mem_limit_total',
         'legacy-root-loop',
         'dirty-limit',
         'cpu_limit',
         'mem_limit',
         'volume_queue',
         'volume_sync']

part7 = ['volume_backends',
         'performance']

part8 = ['prev_release_upgrade',
         'recovery']

for test in part1 + part2 + part3 + part4 + part5 + part6 + part7 + part8:
    test_names.remove(test)

for i in range(1, 9):
    test_names.remove('part{}'.format(i))

# Put remaining tests in part8
part8 += test_names


def run_test(test_name):
    ATTEMPTS = 2
    for i in range(ATTEMPTS):
        # restart portod to remove containers/volumes
        if test == 'portod_start':
            subprocess.check_call([portod, 'stop'])
        else:
            subprocess.check_call([portod, 'restart'])

        try:
            subprocess.check_call(['ctest', '-R', '^{}$'.format(test), '-V'])
            return
        except Exception as e:
            if i != ATTEMPTS - 1:
                continue
            else:
                raise e
    

for test in eval('part{}'.format(sys.argv[1])):
    run_test(test)
    if test not in ['portod_stop', 'cleanup_portod']:
        c = porto.Connection()
        fatals = int(c.GetProperty('/', 'porto_stat[fatals]'))
        assert 0 == fatals
