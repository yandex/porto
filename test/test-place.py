import porto
from test_common import *

c = porto.Connection()

a = c.Run('a', place_limit="/place: 1G")
ExpectEq(a['place_usage'], "")
ExpectEq(a['place_limit'], "/place: 1073741824")

v = c.CreateVolume(space_limit='1G', owner_container='a')
ExpectEq(a['place_usage'], "/place: 1073741824; total: 1073741824")
ExpectEq(Catch(c.CreateVolume, space_limit='1G', owner_container='a'), porto.exceptions.ResourceNotAvailable)
ExpectEq(Catch(c.CreateVolume, space_limit='0', owner_container='a'), porto.exceptions.ResourceNotAvailable)
ExpectEq(Catch(c.CreateVolume, owner_container='a'), porto.exceptions.ResourceNotAvailable)
v.Destroy()

ExpectEq(a['place_usage'], "/place: 0; total: 0")
ExpectEq(Catch(c.CreateVolume, owner_container='a'), porto.exceptions.ResourceNotAvailable)
ExpectEq(Catch(c.CreateVolume, space_limit='0', owner_container='a'), porto.exceptions.ResourceNotAvailable)
ExpectEq(Catch(c.CreateVolume, backend='plain', owner_container='a'), porto.exceptions.ResourceNotAvailable)
ExpectEq(Catch(c.CreateVolume, space_limit='2G', owner_container='a'), porto.exceptions.ResourceNotAvailable)

v = c.CreateVolume(space_limit='1M', owner_container='a')
ExpectEq(a['place_usage'], "/place: 1048576; total: 1048576")
v.Destroy()

a.Destroy()
