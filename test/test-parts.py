from test_common import *

import subprocess
import sys

assert len(sys.argv) == 2

tests = subprocess.check_output(['ctest', '-N'])

test_names = []

for line in tests.split('\n'):
    line = line.strip().split(':')
    if len(line) == 2 and line[0].startswith('Test'):
        test_names.append(line[1].strip())


part1 = ['uid_handling',
         'unpriv-cred',
         'isolation',
         'security',
         'hijack',
         'net',
         'tc-classes',
         'tc-rebuild',
         'locate-process',
         'prev_release_upgrade',
         'oom',
         'hugetlb',
         'coredump',
         'volume-restore',
         'htb-restore']

part2 = ['portotest',
         'legacy-root-loop',
         'net-sched']

part3 = ['mem-overcommit',
         'mem_limit_total',
         'dirty-limit',
         'cpu_limit']

part4 = ['leaks']

part5 = ['cleanup_portod',
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

part6 = ['mem_limit',
         'volume_backends',
         'volume_places',
         'volume_links',
         'volume_sync',
         'portod_cli',
         'recovery']

part7 = ['performance',
         'fuzzer_soft',
         'fuzzer',
         'stats',
         'portod_stop']

# TO FIX
broken_tests = ['devices', 'mem-recharge', 'volume_queue']

for test in part1 + part2 + part3 + part4 + part5 + part6 + part7 + broken_tests:
    test_names.remove(test)

for i in range(1, 9):
    test_names.remove('part{}'.format(i))

# Put remaining tests in part8
part8 = test_names

for test in eval('part{}'.format(sys.argv[1])):
    if test == 'portod_start':
        subprocess.check_call([portod, 'stop'])
    subprocess.check_call(['ctest', '-R', '^{}$'.format(test), '-V'])
