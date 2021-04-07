#!/usr/bin/python
import sys
import os
import uuid
import subprocess

target = sys.argv[2]

# Create new volume to deal with place policy
volume = subprocess.check_output(['/portoctl', 'vcreate', '-A']).strip()
random = uuid.uuid4().hex
inner_place_path = os.path.join(volume, random)
outer_place_path = os.path.join(volume.replace('/porto/volume_', '/place/porto_volumes/'), 'native', random)
print('volume path: %s' % volume)
print('inner place path: %s' % inner_place_path)
print('outer place path: %s' % outer_place_path)

os.makedirs(inner_place_path)

if sys.argv[1] == 'layer':
    # Export Layer
    os.makedirs(os.path.join(inner_place_path, 'porto_layers'))
    os.symlink(target, os.path.join(inner_place_path, 'porto_layers', 'test'))
    subprocess.check_call(['/portoctl', 'raw', 'exportLayer { volume: "" tarball: "/layer.tar.gz" layer: "test" place: "'+ outer_place_path  +'" }'])

elif sys.argv[1]  == 'storage':
    # Export Storage
    os.makedirs(os.path.join(inner_place_path, 'porto_storage'))
    os.symlink(target, os.path.join(inner_place_path, 'porto_storage', 'test'))
    subprocess.check_call(['/portoctl', 'raw', 'exportStorage { tarball: "/layer.tar.gz" name: "test" place: "'+ outer_place_path  +'" }'])
