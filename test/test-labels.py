#!/usr/bin/python

import os
import time
import porto
from test_common import *

c = porto.Connection()

a = c.Run('a')

ExpectProp(a, 'labels', '')

assert Catch(c.SetLabel, 'a', '!', '.') == porto.exceptions.InvalidLabel
assert Catch(c.SetLabel, 'a', 'TEST.!', '.') == porto.exceptions.InvalidLabel
assert Catch(c.SetLabel, 'a', 'a', '.') == porto.exceptions.InvalidLabel
assert Catch(c.SetLabel, 'a', 'a.a', '.') == porto.exceptions.InvalidLabel
assert Catch(c.SetLabel, 'a', 'A.a', '.') == porto.exceptions.InvalidLabel
assert Catch(c.SetLabel, 'a', 'A' * 17 + '.a', '.') == porto.exceptions.InvalidLabel
assert Catch(c.SetLabel, 'a', 'AAAAAAAA.' + 'a' * 150, '.') == porto.exceptions.InvalidLabel
assert Catch(c.SetLabel, 'a', 'AAAAAAAA.a', ' ') == porto.exceptions.InvalidLabel
assert Catch(c.SetLabel, 'a', 'AAAAAAAA.a', 'a' * 5000) == porto.exceptions.InvalidLabel
assert Catch(c.SetLabel, 'a', 'PORTO.a', '.') == porto.exceptions.InvalidLabel

c.SetLabel('a', 'A' * 16 + '.a', '.')
c.SetLabel('a', 'A' * 16 + '.a', '')

c.SetLabel('a', 'A' * 16 + '.' + 'a' * 111, '.')
c.SetLabel('a', 'A' * 16 + '.' + 'a' * 111, '')

for i in range(100):
    c.SetLabel('a', 'TEST.' + str(i), '.')

assert Catch(c.SetLabel, 'a', 'TEST.a', '.') == porto.exceptions.ResourceNotAvailable

for i in range(100):
    c.SetLabel('a', 'TEST.' + str(i), '')

ExpectProp(a, 'labels', '')

assert Catch(a.GetLabel, 'TEST.a') == porto.exceptions.LabelNotFound
ExpectProp(a, 'labels', '')
assert Catch(a.GetProperty, 'TEST.a') == porto.exceptions.LabelNotFound
assert Catch(a.GetProperty, 'labels[TEST.a]') == porto.exceptions.LabelNotFound
ExpectEq(c.FindLabel('TEST.a'), [])
assert Catch(c.WaitLabels, ['***'], ['TEST.*'], timeout=0) == porto.exceptions.WaitContainerTimeout
assert Catch(c.WaitLabels, ['a'], ['TEST.*'], timeout=0) == porto.exceptions.WaitContainerTimeout

c.SetLabel('a', 'TEST.a', '.')
ExpectEq(a.GetLabel('TEST.a'), '.')
ExpectProp(a, 'labels', 'TEST.a: .')
ExpectProp(a, 'labels[TEST.a]', '.')
ExpectProp(a, 'TEST.a', '.')

w = c.WaitLabels(['***'], ['TEST.*'], timeout=0)
ExpectEq(w['name'], 'a')
ExpectEq(w['state'], 'meta')
ExpectEq(w['label'], 'TEST.a')
ExpectEq(w['value'], '.')

ExpectEq(c.WaitLabels(['a'], ['TEST.*'], timeout=0)['name'], 'a')

ExpectEq(c.FindLabel('TEST.a'), [{'name':'a', 'label':'TEST.a', 'value': '.', 'state':'meta'}])
ExpectEq(c.FindLabel('*.*'), [{'name':'a', 'label':'TEST.a', 'value': '.', 'state':'meta'}])
ExpectEq(c.FindLabel('TEST.b'), [])
ExpectEq(c.FindLabel('TEST.a', mask="b"), [])
ExpectEq(c.FindLabel('TEST.a', state="stopped"), [])
ExpectEq(c.FindLabel('TEST.a', value="2"), [])
ExpectEq(c.FindLabel('TEST.a*', mask="a*", state="meta", value="."), [{'name':'a', 'label':'TEST.a', 'value': '.', 'state':'meta'}])

c.SetLabel('a', 'TEST.a', '')
ExpectProp(a, 'labels', '')
assert Catch(a.GetProperty, 'TEST.a') == porto.exceptions.LabelNotFound
assert Catch(a.GetProperty, 'labels[TEST.a]') == porto.exceptions.LabelNotFound
ExpectEq(c.FindLabel('TEST.a'), [])
assert Catch(c.WaitLabels, ['***'], ['TEST.*'], timeout=0) == porto.exceptions.WaitContainerTimeout

assert Catch(a.SetLabel, 'TEST.a', '.', 'N') == porto.exceptions.LabelNotFound

a.SetLabel('TEST.a', 'N', '')
ExpectProp(a, 'TEST.a', 'N')

a.SetLabel('TEST.a', 'Y', 'N')
ExpectProp(a, 'TEST.a', 'Y')

assert Catch(a.SetLabel, 'TEST.a', 'Y', 'N') == porto.exceptions.Busy
ExpectProp(a, 'TEST.a', 'Y')

a.SetLabel('TEST.a', '', 'Y')
assert Catch(a.GetProperty, 'TEST.a') == porto.exceptions.LabelNotFound


assert Catch(a.IncLabel, 'TEST.a', add=0) == porto.exceptions.LabelNotFound
a.SetLabel('TEST.a', 'a')
assert Catch(a.IncLabel, 'TEST.a') == porto.exceptions.InvalidValue
a.SetLabel('TEST.a', '')
ExpectEq(a.IncLabel('TEST.a'), 1)
ExpectEq(a.IncLabel('TEST.a', 2), 3)
ExpectEq(a.IncLabel('TEST.a', -2), 1)
assert Catch(a.IncLabel, 'TEST.a', 2**63-1) == porto.exceptions.InvalidValue
ExpectEq(a.IncLabel('TEST.a', 2**63-2), 2**63-1)
ExpectEq(a.IncLabel('TEST.a', -(2**63-1)), 0)
ExpectEq(a.IncLabel('TEST.a', -1), -1)
assert Catch(a.IncLabel, 'TEST.a', -(2**63)) == porto.exceptions.InvalidValue
ExpectEq(a.IncLabel('TEST.a', 1), 0)
ExpectEq(a.IncLabel('TEST.a', -(2**63)), -(2**63))
a.SetLabel('TEST.a', str(2**64))
assert Catch(a.IncLabel, 'TEST.a', -1) == porto.exceptions.InvalidValue
a.SetLabel('TEST.a', str(-(2**64)))
assert Catch(a.IncLabel, 'TEST.a', 1) == porto.exceptions.InvalidValue
a.SetLabel('TEST.a', '')


b = c.Run('a/b')

a['TEST.a'] = 'a'
b['TEST.b'] = 'b'

ExpectProp(a, 'TEST.a', 'a')
ExpectProp(b, '.TEST.a', 'a')
ExpectProp(b, 'TEST.b', 'b')
ExpectEq(b.GetLabel('.TEST.a'), 'a')

assert Catch(b.GetProperty, 'TEST.a') == porto.exceptions.LabelNotFound
assert Catch(a.GetProperty, 'TEST.b') == porto.exceptions.LabelNotFound
assert Catch(a.GetProperty, '.TEST.b') == porto.exceptions.LabelNotFound

ExpectEq(c.FindLabel('TEST.a'), [{'name':'a', 'label':'TEST.a', 'value':'a', 'state':'meta'}])
ExpectEq(c.FindLabel('.TEST.a'), [{'name':'a', 'label':'.TEST.a', 'value':'a', 'state':'meta'}, {'name':'a/b', 'label':'.TEST.a', 'value':'a', 'state':'meta'}])
ExpectEq(c.FindLabel('TEST.b'), [{'name':'a/b', 'label':'TEST.b', 'value':'b', 'state':'meta'}])
ExpectEq(c.FindLabel('.TEST.*'), [])

a.Destroy()
