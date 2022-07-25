import porto
import subprocess
from test_common import *

conn = porto.Connection()

try:
    ConfigurePortod('test-extra_properties', """
    container {
        extra_properties {
            filter: "abc"
            properties {
                name: "command"
                value: "sleep 123"
            }
            properties {
                name: "max_respawns"
                value: "10"
            }
        }
    }""")

    a = conn.Run('abc')
    ExpectEq('command;max_respawns', a['extra_properties'])
    ExpectEq('sleep 123', a['command'])
    ExpectEq('10', a['max_respawns'])
    a.Destroy()

    ConfigurePortod('test-extra_properties', """
    container {
        extra_properties {
            filter: "abc"
            properties {
                name: "command"
                value: "sleep 123"
            }
        }
        extra_properties {
            filter: "***"
            properties {
                name: "max_respawns"
                value: "1024"
            }
        }
    }""")

    a = conn.Run('abc')
    ExpectEq('command;max_respawns', a['extra_properties'])
    ExpectEq('sleep 123', a['command'])
    ExpectEq('1024', a['max_respawns'])

    # Set property manually. It must be removed from extra_properties
    a.SetProperty('max_respawns', '1025')
    ExpectEq('command', a['extra_properties'])
    ExpectEq('sleep 123', a['command'])
    ExpectEq('1025', a['max_respawns'])

    a.Stop()
    a.Start()

    ExpectEq('command', a['extra_properties'])
    ExpectEq('sleep 123', a['command'])
    ExpectEq('1025', a['max_respawns'])
    a.Destroy()

    a = conn.Run('abc', command='sleep 321')
    ExpectEq('max_respawns', a['extra_properties'])
    ExpectEq('sleep 321', a['command'])
    ExpectEq('1024', a['max_respawns'])
    a.Destroy()

    a = conn.Run('abcd', weak=False)
    ExpectEq('max_respawns', a['extra_properties'])
    ExpectNe('sleep 123', a['command'])
    ExpectEq('1024', a['max_respawns'])

    ConfigurePortod('test-extra_properties', "")

    ExpectEq('max_respawns', a['extra_properties'])

    a.Stop()
    a.Start()

    ExpectEq('', a['extra_properties'])
    ExpectNe('sleep 123', a['command'])
    ExpectNe('1024', a['max_respawns'])

    a.Destroy()

    # add not supported extra_property
    fatals = int(conn.GetProperty('/', 'porto_stat[fatals]'))

    ConfigurePortod('test-extra_properties', """
    container {
        extra_properties {
            filter: "abc"
            properties {
                name: "command123"
                value: "sleep 123"
            }
        }
        extra_properties {
            filter: "***"
            properties {
                name: "max_respawns321"
                value: "1024"
            }
            properties {
                name: "capabilities[SYS_ADMIN]"
                value: "true"
            }
        }
    }""")

    a = conn.Run('abc')
    ExpectEq('capabilities[SYS_ADMIN]', a['extra_properties'])
    ExpectEq(True, a['capabilities[SYS_ADMIN]'])

    a.Stop()
    a.SetProperty('capabilities[SYS_ADMIN]','false')
    a.Start()

    ExpectEq('', a['extra_properties'])
    ExpectEq(False, a['capabilities[SYS_ADMIN]'])
    a.Destroy()

    a = conn.Create('abc')
    a.SetProperty('capabilities[SYS_ADMIN]','false')
    a.Start()
    ExpectEq('', a['extra_properties'])
    ExpectEq(False, a['capabilities[SYS_ADMIN]'])
    a.Destroy()

    ExpectEq(fatals + 2, int(conn.GetProperty('/', 'porto_stat[fatals]')))
    subprocess.call([portod, 'clearstat', 'fatals'])
    subprocess.call([portod, 'clearstat', 'errors'])
    ExpectEq(0, int(conn.GetProperty('/', 'porto_stat[fatals]')))
    ExpectEq(0, int(conn.GetProperty('/', 'porto_stat[errors]')))

finally:
    ConfigurePortod('test-extra_properties', "")
