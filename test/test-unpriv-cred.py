import porto
import pwd
import grp
import sys
import os

from test_common import *

Case="Case 1: alice creates and starts under himself"

AsAlice()
c = porto.Connection()
c.Connect()
r = c.Create("test-a")
r.SetProperty("command", "ls")
r.Start()
ExpectEq(r.Wait(), "test-a")
ExpectEq(r.GetProperty("user"), "porto-alice")
ExpectEq(r.GetProperty("group"), "porto-alice")
print "%s OK!" %(Case)
r.Destroy()
AsRoot()

Case="Case 2: alice sets bob, catching exception"

AsAlice()
c = porto.Connection()
c.Connect()
r = c.Create("test-a")
r.SetProperty("command", "ls")
r.SetProperty("user", "porto-bob")
ExpectEq(Catch(r.Start), porto.exceptions.PermissionError)
r.Destroy()
print "%s OK!" %(Case)
AsRoot()

Case="Case 3: alice starts container created by bob, catching exception"

AsBob()
c = porto.Connection()
c.Connect()
r = c.Create("test-a")
r.SetProperty("command", "ls")
AsRoot()
AsAlice()
c = porto.Connection()
c.Connect()
r = c.Find("test-a")
ExpectEq(Catch(r.Start), porto.exceptions.PermissionError)
AsRoot()
AsBob()
c = porto.Connection()
c.Connect()
r = c.Find("test-a")
r.Start()
ExpectEq(r.Wait(), "test-a")
ExpectEq(r.GetProperty("user"), "porto-bob")
ExpectEq(r.GetProperty("group"), "porto-bob")
r.Destroy()
print "%s OK!" %(Case)
AsRoot()

Case="Case 4: root sets bob and alice-group (bob not in alice-group), root can do everything"

c = porto.Connection()
c.Connect()
r = c.Create("test-a")
r.SetProperty("user", "porto-bob")
r.SetProperty("group", "porto-alice")
r.SetProperty("command", "ls")
r.Start()
ExpectEq(r.Wait(), "test-a")
ExpectEq(r.GetProperty("user"), "porto-bob")
ExpectEq(r.GetProperty("group"), "porto-alice")
print "%s OK!" %(Case)
r.Destroy()
