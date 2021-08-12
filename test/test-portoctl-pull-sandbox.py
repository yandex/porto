import porto
import shutil
import subprocess
from test_common import *

# Flaky in sandbox task. Test disabled.
sys.exit(0)

# script in /usr/lib/porto creates during installation via 'make install' or 'dpkg -i'
subprocess.call(['mkdir', '/usr/lib/porto', '-p'])
shutil.copyfile(portobin + '/src/portoctl-pull-sandbox', '/usr/lib/porto/portoctl-pull-sandbox')
subprocess.call(['chmod', '+x', '/usr/lib/porto/portoctl-pull-sandbox'])

# 'portoctl pull-sandbox' needs portoctl in /usr/sbin/portoctl
remove_portoctl = False
if not os.path.exists('/usr/sbin/portoctl'):
    remove_portoctl = True
    shutil.copyfile(portoctl, '/usr/sbin/portoctl')
    subprocess.call(['chmod', '+x', '/usr/sbin/portoctl'])

c = porto.Connection()
layers = [x.name for x in c.ListLayers()]

try:
    assert subprocess.call([portoctl, 'pull-sandbox', 'rtc-xenial/os']) == 0
    assert subprocess.call([portoctl, 'pull-sandbox', 'infra/environments/rtc-xenial/app-layer/layer.tar.gz']) == 0
    assert subprocess.call([portoctl, 'pull-sandbox', '--raw', '--type ARCADIA_PROJECT_TGZ -A released=stable -A arcadia_path=infra/environments/rtc-bionic/os-layer/layer.tar.gz']) == 0

    assert subprocess.call([portoctl, 'pull-sandbox', '-o', 'layer1234', '--raw', '--type ARCADIA_PROJECT_TGZ -A released=stable -A arcadia_path=infra/environments/rtc-bionic/app-layer/layer.tar.gz']) == 0
    assert c.FindLayer('layer1234').name == 'layer1234'

    assert len(layers) + 4 == len(c.ListLayers())

finally:
    if remove_portoctl:
        os.remove('/usr/sbin/portoctl')

    for layer in c.ListLayers():
        if layer.name not in layers:
            layer.Remove()
