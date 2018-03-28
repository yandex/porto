from test_common import *
import porto

c = porto.Connection()

a = c.Create("a")
ExpectEq(c.WaitContainers(["a"]), "a")
a.Destroy()

a = c.Run("a", command="true")
ExpectEq(c.WaitContainers(["a"]), "a")
a.Destroy()

events = []
def wait_event(name, state, when):
    ExpectEq((name, state), events.pop(0))

c.AsyncWait(["a"], wait_event)

events=[('a', 'stopped'), ('a', 'starting'), ('a', 'running'), ('a', 'stopping'), ('a', 'stopped'), ('a', 'destroyed')]
a = c.Run("a", command="true")
a.Destroy()
ExpectEq(events, [])

events=[('a', 'stopped'), ('a', 'starting'), ('a', 'running'), ('a', 'running'), ('a', 'stopping'), ('a', 'stopped'), ('a', 'destroyed')]
a = c.Run("a", weak=False, command="sleep 1000")
ReloadPortod()
a.Destroy()
ExpectEq(events, [])
