import porto
import subprocess

conn = porto.Connection()

expected_errors = int(conn.GetData('/', 'porto_stat[errors]'))
expected_warnings = int(conn.GetData('/', 'porto_stat[warnings]'))

# get managed links
links = [l.split(':')[0].strip() for l in conn.GetData('/', 'net_bytes').split(';') if l]
assert links

# check and delete qdisc
for link in links:
    assert subprocess.check_output(['tc', 'qdisc', 'show', 'dev', link]).startswith('qdisc htb')
    subprocess.check_call(['tc', 'qdisc', 'del', 'root', 'dev', link])
    assert not subprocess.check_output(['tc', 'qdisc', 'show', 'dev', link]).startswith('qdisc htb')
    expected_warnings += 1

# start test container
test = conn.Create('test')
test.SetProperty('command', 'true')
test.Start()
test.Wait()
assert test.GetData('exit_status') == '0'
test.Destroy()

# recheck qdisc
for link in links:
    assert subprocess.check_output(['tc', 'qdisc', 'show', 'dev', link]).startswith('qdisc htb')

assert int(conn.GetData('/', 'porto_stat[errors]')) == expected_errors
assert int(conn.GetData('/', 'porto_stat[warnings]')) == expected_warnings
