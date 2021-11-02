from test_common import *

import porto

conn = porto.Connection()

w = conn.Create("w", weak=True)

root_volume = conn.CreateVolume(backend='overlay', layers=['ubuntu-precise'], containers='w')
volume = conn.CreateVolume(backend='plain', containers='w')

a = conn.Run('a', root=root_volume.path, bind='{} /bin/portoctl ro'.format(portoctl))
volume.Link(a, target='/storage')

b = conn.Run('a/b', wait=5, command='true', io_limit='.storage: 1024')
ExpectEq('0', b['exit_code'])
b.Destroy()

b = conn.Run('a/b', wait=5, command='portoctl exec test command="true" io_limit="/storage: 1024"')
ExpectEq('0', b['exit_code'])
b.Destroy()

volume.Unlink(a)

a.Destroy()
w.Destroy()
