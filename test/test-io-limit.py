from test_common import *

import porto

conn = porto.Connection()

w = conn.Create("w", weak=True)

root_volume = conn.CreateVolume(backend='overlay', layers=['ubuntu-precise'], containers='w')
volume = conn.CreateVolume(backend='plain', containers='w')

a = conn.Run('a', root=root_volume.path, bind='{} /bin/portoctl ro'.format(portoctl))
volume.Link(a, target='/storage')

def run_ct(name, command='true', **porto_kwargs):
    return conn.Run(name, command=command, wait=5, enable_porto='isolate', **porto_kwargs)

# /place exists only on host
b = run_ct('a/b', io_limit='/place: 104857600')
ExpectEq('0', b['exit_code'])
b.Destroy()

b = run_ct('a/b', 'portoctl exec c command="true" io_limit="/place: 104857600"')
ExpectEq('0', b['exit_code'])
b.Destroy()

# /storage exists only in container
ExpectException(run_ct, porto.exceptions.InvalidValue, 'a/b', io_limit='/storage: 104857600')

b = run_ct('a/b', 'portoctl exec c command="true" io_limit="/storage: 104857600"')
ExpectEq('1', b['exit_code'])
ExpectEq('Cannot start container: InvalidValue:(Disk not found: /storage)\n', b['stderr'])
b.Destroy()

# with dot resolve occurs from container chroot
ExpectException(run_ct, porto.exceptions.InvalidValue, 'a/b', io_limit='.place: 104857600')

b = run_ct('a/b', 'portoctl exec c command="true" io_limit=".place: 104857600"')
ExpectEq('1', b['exit_code'])
ExpectEq('Cannot start container: InvalidValue:(Disk not found: .place)\n', b['stderr'])
b.Destroy()

b = run_ct('a/b', io_limit='.storage: 104857600')
ExpectEq('0', b['exit_code'])
b.Destroy()

b = run_ct('a/b', 'portoctl exec c command="true" io_limit=".storage: 104857600"')
ExpectEq('0', b['exit_code'])
b.Destroy()

volume.Unlink(a)

a.Destroy()
w.Destroy()
