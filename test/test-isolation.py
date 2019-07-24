import porto
from test_common import *

c = porto.Connection()

ExpectEq(ProcStatus('self', "CapBnd"), "0000003fffffffff")

# root user

a = c.Run("a")
pid = a['root_pid']

ExpectNe(ProcStatus(pid, "NSpid"), pid)
ExpectEq(ProcStatus(pid, "CapInh"), "0000000000000000")
ExpectEq(ProcStatus(pid, "CapPrm"), "00000000a9ec77fb")
ExpectEq(ProcStatus(pid, "CapEff"), "00000000a9ec77fb")
ExpectEq(ProcStatus(pid, "CapBnd"), "00000000a9ec77fb")
ExpectEq(ProcStatus(pid, "CapAmb"), "0000000000000000")

a.Destroy()

a = c.Run("a", virt_mode='host', command="sleep 10000")
pid = a['root_pid']

ExpectEq(ProcStatus(pid, "NSpid"), pid)
ExpectEq(ProcStatus(pid, "CapInh"), "0000000000000000")
ExpectEq(ProcStatus(pid, "CapPrm"), "0000003fffffffff")
ExpectEq(ProcStatus(pid, "CapEff"), "0000003fffffffff")
ExpectEq(ProcStatus(pid, "CapBnd"), "0000003fffffffff")
ExpectEq(ProcStatus(pid, "CapAmb"), "0000000000000000")

a.Destroy()

# non root user
AsAlice()

c = porto.Connection()

a = c.Run("a")
pid = a['root_pid']

ExpectNe(ProcStatus(pid, "NSpid"), pid)
ExpectEq(ProcStatus(pid, "CapInh"), "0000000000000000")
ExpectEq(ProcStatus(pid, "CapPrm"), "0000000000000000")
ExpectEq(ProcStatus(pid, "CapEff"), "0000000000000000")
ExpectEq(ProcStatus(pid, "CapBnd"), "00000000a9ec77fb")
ExpectEq(ProcStatus(pid, "CapAmb"), "0000000000000000")

a.Destroy()

a = c.Run("a", virt_mode='host', command="sleep 10000")
pid = a['root_pid']

ExpectEq(ProcStatus(pid, "NSpid"), pid)
ExpectEq(ProcStatus(pid, "CapInh"), "0000000000000000")
ExpectEq(ProcStatus(pid, "CapPrm"), "0000000000000000")
ExpectEq(ProcStatus(pid, "CapEff"), "0000000000000000")
ExpectEq(ProcStatus(pid, "CapBnd"), "0000003fffffffff")
ExpectEq(ProcStatus(pid, "CapAmb"), "0000000000000000")

a.Destroy()

a = c.Run("a", isolate='false', root_volume={"layers": ["ubuntu-precise"]})
pid = a['root_pid']

ExpectEq(ProcStatus(pid, "NSpid"), pid)
ExpectEq(ProcStatus(pid, "CapInh"), "0000000000000000")
ExpectEq(ProcStatus(pid, "CapPrm"), "0000000000000000")
ExpectEq(ProcStatus(pid, "CapEff"), "0000000000000000")
ExpectEq(ProcStatus(pid, "CapBnd"), "00000000a80425db")
ExpectEq(ProcStatus(pid, "CapAmb"), "0000000000000000")

a.Destroy()

a = c.Run("a", net='none', memory_limit='1G', root_volume={"layers": ["ubuntu-precise"]})
pid = a['root_pid']

ExpectNe(ProcStatus(pid, "NSpid"), pid)
ExpectEq(ProcStatus(pid, "CapInh"), "0000000000000000")
ExpectEq(ProcStatus(pid, "CapPrm"), "0000000000000000")
ExpectEq(ProcStatus(pid, "CapEff"), "0000000000000000")
ExpectEq(ProcStatus(pid, "CapBnd"), "00000000a80c75fb")
ExpectEq(ProcStatus(pid, "CapAmb"), "0000000000000000")

a.Destroy()

# virt_mode=host restrictions

a = c.Run("a")
ExpectEq(Catch(c.Run, "a/b", virt_mode='host'), porto.exceptions.Permission)
a.Destroy()

ExpectEq(Catch(c.Run, "a", virt_mode='host', isolate='true'), porto.exceptions.InvalidValue)
ExpectEq(Catch(c.Run, "a", virt_mode='host', root="/a"), porto.exceptions.InvalidValue)
ExpectEq(Catch(c.Run, "a", virt_mode='host', bind="/a /b"), porto.exceptions.InvalidValue)
