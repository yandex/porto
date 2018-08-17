from test_common import *
import porto
import subprocess

def SetSize(count):
    open("/proc/sys/vm/nr_hugepages", 'w').write(count)

conn = porto.Connection()
root = conn.Find('/')

if not os.path.exists('/dev/hugepages'):
    os.mkdir('/dev/hugepages')
    if subprocess.call(['mount', '-t', 'hugetlbfs', 'hugetlbfs', '/dev/hugepages']):
        os.rmdir('/dev/hugepages')

if os.path.exists('/dev/hugepages/dummy'):
    os.unlink('/dev/hugepages/dummy')

ExpectEq(root['hugetlb_usage'], '0')

SetSize('0')

ExpectEq(root['hugetlb_limit'], '0')

total = root['memory_limit']

SetSize('1')

ExpectEq(root['hugetlb_limit'], '2097152')
ExpectEq(root['hugetlb_usage'], '0')

ExpectEq(root['memory_limit'], str(int(total) - 2097152))

a = conn.Run('a', command="fallocate -l 2m /dev/hugepages/dummy", wait=1)

ExpectEq(a['state'], 'dead')
ExpectEq(a['exit_code'], '0')

ExpectEq(a['hugetlb_limit'], '0')
ExpectEq(a['hugetlb_usage'], '2097152')

ExpectEq(root['hugetlb_limit'], '2097152')
ExpectEq(root['hugetlb_usage'], '2097152')

a.Destroy()

ExpectEq(root['hugetlb_limit'], '2097152')
ExpectEq(root['hugetlb_usage'], '2097152')

SetSize('0')

ExpectEq(root['hugetlb_limit'], '2097152')
ExpectEq(root['hugetlb_usage'], '2097152')

SetSize('1')

os.unlink('/dev/hugepages/dummy')

ExpectEq(root['hugetlb_limit'], '2097152')
ExpectEq(root['hugetlb_usage'], '0')

a = conn.Run('a', command="fallocate -l 2m /dev/hugepages/dummy", wait=1, hugetlb_limit=1)

ExpectEq(a['state'], 'dead')
ExpectNe(a['exit_code'], '0')

ExpectEq(a['hugetlb_limit'], '1')
ExpectEq(a['hugetlb_usage'], '0')

ExpectEq(root['hugetlb_limit'], '2097152')
ExpectEq(root['hugetlb_usage'], '0')

a.Destroy()

a = conn.Run('a', command="fallocate -l 2m /dev/hugepages/dummy", wait=1, hugetlb_limit=2097152)

ExpectEq(a['state'], 'dead')
ExpectEq(a['exit_code'], '0')

ExpectEq(a['hugetlb_limit'], '2097152')
ExpectEq(a['hugetlb_usage'], '2097152')

ExpectEq(root['hugetlb_limit'], '2097152')
ExpectEq(root['hugetlb_usage'], '2097152')

a.Destroy()

os.unlink('/dev/hugepages/dummy')

SetSize('0')


total = root['memory_limit']

ExpectEq(root['memory_guarantee_total'], '0')

ExpectEq(Catch(conn.Run, 'a', memory_guarantee=total), porto.exceptions.ResourceNotAvailable)

AsAlice()
alice_conn = porto.Connection()

a = alice_conn.Run('a', memory_guarantee=int(total) - 2**31)

ExpectEq(root['memory_guarantee_total'], str(int(total) - 2**31))

ExpectEq(Catch(alice_conn.Run, 'b', memory_guarantee='4k'), porto.exceptions.ResourceNotAvailable)

ExpectEq(Catch(conn.Run, 'b', memory_guarantee='4k'), porto.exceptions.ResourceNotAvailable)

b = alice_conn.Run('b')
b.Destroy()

AsRoot()

SetSize('1')

ExpectEq(Catch(conn.Run, 'b', memory_guarantee='4k'), porto.exceptions.ResourceNotAvailable)

b = conn.Run('b')
b.Destroy()

ExpectEq(Catch(alice_conn.Run, 'b'), porto.exceptions.ResourceNotAvailable)

b = alice_conn.Run('a/b')
b.Destroy()

ExpectEq(Catch(alice_conn.SetProperty, 'a', 'memory_guarantee', int(total) - 2**31 + 1), porto.exceptions.ResourceNotAvailable)

alice_conn.SetProperty('a', 'memory_guarantee', int(total) - 2**31 - 1)
alice_conn.SetProperty('a', 'memory_guarantee', 0)

ExpectEq(root['memory_guarantee_total'], '0')

AsRoot()

SetSize('0')

a.Destroy()
