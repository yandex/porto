import porto
from test_common import *
import os
import stat
import time
import subprocess

os.umask(0022)

aufs_tar = '/tmp/test-aufs.tar'
layer_name = 'test-aufs'
layer_path = '/place/porto_layers/' + layer_name

conn = porto.Connection()

aufs = conn.CreateVolume(backend='plain')

file(aufs.path + '/file', 'w')

file(aufs.path + '/removed-file', 'w')
file(aufs.path + '/.wh.removed-file', 'w')

os.symlink('foo', aufs.path + '/symlink')

os.symlink('foo', aufs.path + '/removed-symlink')
file(aufs.path + '/.wh.removed-symlink', 'w')

os.mkdir(aufs.path + '/dir')

os.mkdir(aufs.path + '/removed-dir')
os.mkdir(aufs.path + '/removed-dir/removed-subdir')
file(aufs.path + '/.wh.removed-dir', 'w')

os.mknod(aufs.path + '/device', stat.S_IFCHR|0444, os.makedev(1, 1))

os.mknod(aufs.path + '/removed-device', stat.S_IFCHR|0444, os.makedev(1, 1))
file(aufs.path + '/.wh.removed-device', 'w')

os.mkdir(aufs.path + '/opaque-dir')
file(aufs.path + '/opaque-dir/file', 'w')
file(aufs.path + '/opaque-dir/.wh..wh..opq', 'w')

if os.path.exists(aufs_tar):
    os.unlink(aufs_tar)

aufs.Export(aufs_tar)

aufs.Unlink()

try:
    conn.RemoveLayer(layer_name)
except:
    pass

conn.ImportLayer(layer_name, aufs_tar)

for dirpath, dir_names, file_names in os.walk(layer_path):
    for name in dir_names + file_names:
        assert not name.startswith('.wh.'), "Unhandled AUFS whiteout {}".format(name)

ExpectFile(layer_path + '/file', mode=stat.S_IFREG|0644)

ExpectFile(layer_path + '/removed-file', mode=stat.S_IFCHR, dev=0)
ExpectFile(layer_path + '/.wh.removed-file', mode=None)

ExpectFile(layer_path + '/symlink', mode=stat.S_IFLNK|0777)

ExpectFile(layer_path + '/removed-symlink', mode=stat.S_IFCHR, dev=0)
ExpectFile(layer_path + '/.wh.removed-symlink', mode=None)

ExpectFile(layer_path + '/dir', mode=stat.S_IFDIR|0755)

ExpectFile(layer_path + '/removed-dir', mode=stat.S_IFCHR, dev=0)
ExpectFile(layer_path + '/.wh.removed-dir', mode=None)

ExpectFile(layer_path + '/device', mode=stat.S_IFCHR|0444, dev=os.makedev(1, 1))

ExpectFile(layer_path + '/removed-device', mode=stat.S_IFCHR, dev=0)
ExpectFile(layer_path + '/.wh.removed-device', mode=None)

ExpectFile(layer_path + '/opaque-dir', mode=stat.S_IFDIR|0755)
ExpectFile(layer_path + '/opaque-dir/file', mode=stat.S_IFREG|0644)
ExpectFile(layer_path + '/opaque-dir/.wh..wh..opq', mode=None)

ExpectEq(subprocess.check_output(['getfattr', '--only-values', '-n', 'trusted.overlay.opaque', layer_path + '/opaque-dir' ]), 'y')

conn.RemoveLayer(layer_name)
