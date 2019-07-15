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
