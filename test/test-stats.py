import porto
from test_common import *

c = porto.Connection(timeout=10)
root_stats = c.GetProperty("/", "porto_stat").split(';')

print "Porto stats:"

for s in root_stats:
    pair = s.split(':')
    print "{} : {}".format(pair[0], pair[1])

