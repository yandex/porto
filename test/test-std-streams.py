import porto
from test_common import *

c = porto.Connection()

blksize = os.stat(__file__).st_blksize

# write less than 'stdout_limit' and 'st_blksize' with various 'stdout_limit's
a = c.Run('test', command="echo Hello", stdout_limit=blksize/10)
a.Wait()
assert a.GetProperty("stdout") == "Hello\n"
a.Destroy()

a = c.Run('test', command="echo Hello", stdout_limit=blksize)
a.Wait()
assert a.GetProperty("stdout") == "Hello\n"
a.Destroy()

a = c.Run('test', command="echo Hello", stdout_limit=blksize*10)
a.Wait()
assert a.GetProperty("stdout") == "Hello\n"
a.Destroy()


# write between 'stdout_limit' and 'st_blksize', with 'stdout_limit' lower than 'st_blksize'
cmd = "echo "
res = ""
for i in range(0, blksize/4 * 3):
    cmd += "a"
    res += "a"

stdout_limit = blksize / 2
a = c.Run('test', command=cmd, stdout_limit=stdout_limit)
a.Wait()

# via 'GetProperty()' we get 'stdout' that is truncated to 'stdout_limit'
# even if undeflying file is not yet truncated and bigger than 'stdout_limit'
out = a.GetProperty("stdout")

# so, check getting of truncated property:
res_trunc = res[len(res)-stdout_limit+1:] + "\n"
assert out == res_trunc

# and check underlying untruncated file:
with open(a.GetProperty("cwd") + "/" + a.GetProperty("stdout_path"), "r") as stdout_file:
    assert stdout_file.read() == res + "\n"

a.Destroy()

#check std stream read limit (default 16Mb)
ct = c.CreateWeakContainer("test")
vol = c.CreateVolume(private="test-std-stream", containers=ct.name)
ct.SetProperty("cwd", vol.path)
ct.SetProperty("stdout_limit", 1024 * 1024 * 32)
ct.SetProperty("command", "cat test")

stdout_part1 = 'a' * 16 * 1024 * 1024
stdout_part2 = 'b' * 16 * 1024 * 1024

f = open(vol.path + "/test", 'w')
f.write(stdout_part1 + stdout_part2)
f.close()

ct.Start()
ct.Wait()

stdout_value = ct.GetProperty("stdout")
assert stdout_value == stdout_part2

stdout_value = ct.GetProperty("stdout[0]")
assert stdout_value == stdout_part1

stdout_value = ct.GetProperty("stdout[{}]".format(8 << 20)) #8Mb
assert stdout_value == stdout_part1[8 << 20:] + stdout_part2[:8 << 20]

stdout_value = ct.GetProperty("stdout[:{}]".format(8 << 20))
assert stdout_value == stdout_part2[:8 << 20]

stdout_value = ct.GetProperty("stdout[:{}]".format(50 << 20)) #50Mb
assert stdout_value == stdout_part2

stdout_value = ct.GetProperty("stdout[{}:{}]".format(12 << 20, 8 << 20))
assert stdout_value == stdout_part1[12 << 20:] + stdout_part2[:4 << 20]


ct.Destroy()

# check fifo pipe for stdin
ct = c.CreateWeakContainer('test')

vol = c.CreateVolume(layers=['ubuntu-xenial'], containers='test')
ct.SetProperty("root", vol.path)

ct.Start()

a = c.Run('test/a', wait=5, command='mkfifo /pipe')
ExpectEq(a['exit_code'], '0')
a.Destroy()

b = c.Run('test/b', wait=5, command='sleep 1', stdin_path='/pipe')
ExpectEq(b['exit_code'], '0')
b.Destroy()

ct.Destroy()
