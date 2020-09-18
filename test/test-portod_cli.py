import os
import subprocess
from test_common import *

AsRoot()

TMPDIR = "/tmp/test-portod-cli"
TMPDIR_BIN = TMPDIR + "/portod_patched"
PORTOD_PATH = "/run/portod"

try:
    os.mkdir(TMPDIR)
except:
    pass

assert subprocess.check_output([portod, 'status']) == "running\n"

try:
    result = subprocess.check_output([portod, 'start'], stderr=subprocess.STDOUT)
except subprocess.CalledProcessError as e:
    assert e.returncode == 1
    assert e.output == "Another instance of portod is running!\n"

subprocess.check_call([portod, 'stop'])

try:
    subprocess.check_output([portod, 'status'])
except subprocess.CalledProcessError as e:
    assert e.returncode == 1
    assert e.output == "stopped\n"

subprocess.check_call([portod, 'start'])
assert subprocess.check_output([portod, 'status']) == "running\n"
assert os.readlink(PORTOD_PATH) == portod

subprocess.check_call([portod, 'restart'])
assert subprocess.check_output([portod, 'status']) == "running\n"

assert len(subprocess.check_output([portod, 'dump'])) != 0

subprocess.check_call([portod, 'reload'])
assert subprocess.check_output([portod, 'status']) == "running\n"
assert os.readlink(PORTOD_PATH) == portod

assert os.readlink(PORTOD_PATH) == os.path.abspath(portod)

subprocess.check_call(["cp", portod, TMPDIR_BIN])

subprocess.check_output([TMPDIR_BIN, "upgrade"])
assert subprocess.check_output([portod, 'status']) == "running\n"
assert os.readlink(PORTOD_PATH) == os.path.abspath(TMPDIR_BIN)


subprocess.check_output([portod, "upgrade"])
assert subprocess.check_output([portod, 'status']) == "running\n"
assert os.readlink(PORTOD_PATH) == os.path.abspath(portod)

os.unlink(TMPDIR_BIN)

#Invalid symlink re-exec test

os.unlink(PORTOD_PATH)
os.symlink("/bin/ls", PORTOD_PATH)
try:
    subprocess.check_output([portod, "reload"], stderr=subprocess.STDOUT)
    raise BaseException("Succesful reload with {} set to {} ?".format(PORTOD_PATH, "/bin/ls"))
except subprocess.CalledProcessError as e:
    assert e.returncode == 1

try:
    subprocess.check_output([portod, "status"])
    raise BaseException("Status running with {} previously set to {} ?".format(PORTOD_PATH, "/bin/ls"))
except subprocess.CalledProcessError as e:
    assert e.output == "unknown\n"
    assert e.returncode == 1

subprocess.check_output([portod, "start"])
assert os.readlink(PORTOD_PATH) == portod
assert subprocess.check_output([portod, 'status']) == "running\n"

os.rmdir(TMPDIR)
