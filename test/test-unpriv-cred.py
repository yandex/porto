import porto
import pwd
import grp
import sys
import os

from test_common import *

Case="Case 1: bob sets charlie from porto-containers"

AsBob()
c = porto.Connection()
c.Connect()
r = c.Create("test")
ExpectEq(r.GetProperty("user"), "porto-bob")
ExpectEq(r.GetProperty("group"), "porto-bob")
r.SetProperty("group", "porto-charlie")
ExpectEq(Catch(r.Start), porto.exceptions.PermissionError)
r.SetProperty("user", "porto-charlie")
r.SetProperty("command", "ls")
r.Start()
ExpectEq(r.Wait(), "test")
ExpectEq(r.GetProperty("user"), "porto-charlie")
ExpectEq(r.GetProperty("group"), "porto-charlie")
print "%s OK!" %(Case)
r.Destroy()
AsRoot()

Case="Case 2: alice sets david from alice-containers"

AsAlice()
c = porto.Connection()
c.Connect()
r = c.Create("test")
ExpectEq(r.GetProperty("user"), "porto-alice")
ExpectEq(r.GetProperty("group"), "porto-alice")
r.SetProperty("group", "porto-david")
ExpectEq(Catch(r.Start), porto.exceptions.PermissionError)
r.SetProperty("user", "porto-david")
r.SetProperty("group", "porto-david")
r.SetProperty("command", "ls")
r.Start()
ExpectEq(r.Wait(), "test")
ExpectEq(r.GetProperty("user"), "porto-david")
ExpectEq(r.GetProperty("group"), "porto-david")
print "%s OK!" %(Case)
r.Destroy()
AsRoot()

Case="Case 3: bob creates and starts under himself, bob (not in *-containers)"

AsBob()
c = porto.Connection()
c.Connect()
r = c.Create("test")
r.SetProperty("command", "ls")
r.Start()
ExpectEq(r.Wait(), "test")
ExpectEq(r.GetProperty("user"), "porto-bob")
ExpectEq(r.GetProperty("group"), "porto-bob")
print "%s OK!" %(Case)
r.Destroy()
AsRoot()

Case="Case 4: alice sets bob (not in *-containers), catching exception"

AsAlice()
c = porto.Connection()
c.Connect()
r = c.Create("test")
r.SetProperty("command", "ls")
r.SetProperty("user", "porto-bob")
ExpectEq(Catch(r.Start), porto.exceptions.PermissionError)
r.Destroy()
print "%s OK!" %(Case)
AsRoot()

Case="Case 5: bob sets charlie and alice-group (charlie not in alice-group), catching exception"

AsBob()
c = porto.Connection()
c.Connect()
r = c.Create("test")
r.SetProperty("user", "porto-charlie")
r.SetProperty("group", "porto-alice")
ExpectEq(Catch(r.Start), porto.exceptions.PermissionError)
print "%s OK!" %(Case)
r.Destroy()
AsRoot()

Case="Case 6: root sets bob and alice-group (bob not in alice-group), root can do everything"
c = porto.Connection()
c.Connect()
r = c.Create("test")
r.SetProperty("user", "porto-bob")
r.SetProperty("group", "porto-alice")
r.SetProperty("command", "ls")
r.Start()
ExpectEq(r.Wait(), "test")
ExpectEq(r.GetProperty("user"), "porto-bob")
ExpectEq(r.GetProperty("group"), "porto-alice")
print "%s OK!" %(Case)
r.Destroy()

Case="Case 7: alice starts container created by bob (bob not in porto-containers and porto-alice-containers), catching exception"

AsBob()
c = porto.Connection()
c.Connect()
r = c.Create("test")
r.SetProperty("command", "ls")
AsRoot()
AsAlice()
c = porto.Connection()
c.Connect()
r = c.Find("test")
ExpectEq(Catch(r.Start), porto.exceptions.PermissionError)
AsRoot()
AsBob()
c = porto.Connection()
c.Connect()
r = c.Find("test")
r.Start()
ExpectEq(r.Wait(), "test")
ExpectEq(r.GetProperty("user"), "porto-bob")
ExpectEq(r.GetProperty("group"), "porto-bob")
r.Destroy()
print "%s OK!" %(Case)
AsRoot()
