from test_common import *
import porto
import os
import subprocess
import shutil

test_path = "/tmp/test"

conn = porto.Connection(timeout=30)
w = conn.Create("w", weak=True)

try:
    shutil.rmtree(test_path)
except:
    pass
os.mkdir(test_path)


def run(cmd):
    subprocess.check_call(cmd.split())


def vcheck(volume, type):
    if type == 'host':
        try:
            run("{} vcheck {}".format(portoctl, volume.path))
        except subprocess.CalledProcessError as ex:
            if ex.returncode == 9:
                raise porto.exceptions.NotSupported()

    elif type == 'container':
        a = conn.Run('a', wait=1, root=volume.path, command='/bin/portoctl vcheck .',
                     enable_porto='isolate', bind='{} /bin/portoctl ro'.format(portoctl), weak=True)
        res = int(a['exit_code'])
        a.Destroy()
        if res == 9:
            raise porto.exceptions.NotSupported()

    elif type == 'volume':
        volume.Check()

    else:
        raise Exception("Wrong value of vcheck type: {}".format(vcheck))


def test_broken_quota(backend, limit, type):
    print("Backend:", backend)
    print("Limit:", limit)
    print("Type:", type)
    parameters = {
        'backend': backend,
        'containers': 'w'
    }
    if limit:
        parameters['space_limit'] = limit
    if backend == 'overlay':
        parameters['layers'] = ["ubuntu-xenial"]
    else:
        parameters['path'] = test_path

    v = conn.CreateVolume(**parameters)
    if not limit:
        ExpectException(vcheck, porto.exceptions.NotSupported, v, type)
        v.Destroy()
        print()
        return

    # get storage path for overlay volume
    id = v.GetProperty("id")
    path = v.path
    if backend == "overlay":
        path = "/place/porto_volumes/{}/overlay".format(id)

    if os.path.exists("{}/kek".format(v.path)):
        os.rmdir("{}/kek".format(v.path))

    # create new inode to broke quota
    run("project_quota off {}".format(path))
    os.mkdir("{}/kek".format(v.path))
    run("project_quota on {}".format(path))

    # invoke vcheck and test quota before and after vcheck
    print("Test before:")
    ExpectException(run, subprocess.CalledProcessError, "project_quota test {}".format(path))
    print("Invoke vcheck")
    vcheck(v, type)
    print("Test after:")
    run("project_quota test {}".format(path))

    os.rmdir("{}/kek".format(v.path))
    v.Destroy()
    print()


try:
    test_broken_quota('quota', '1Mb', 'host')
    test_broken_quota('native', '1Mb', 'host')
    test_broken_quota('native', None, 'host')
    test_broken_quota('overlay', '1Mb', 'host')
    test_broken_quota('overlay', None, 'host')

    test_broken_quota('overlay', '1Mb', 'container')
    test_broken_quota('overlay', None, 'container')

    test_broken_quota('quota', '1Mb', 'volume')
    test_broken_quota('native', '1Mb', 'volume')
    test_broken_quota('native', None, 'volume')
    test_broken_quota('overlay', '1Mb', 'volume')
    test_broken_quota('overlay', None, 'volume')

except Exception as ex:
    raise ex

finally:
    shutil.rmtree(test_path)
