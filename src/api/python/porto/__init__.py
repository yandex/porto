"""Porto python API

Example:

import porto

conn = porto.Connection()
container = conn.Run("test", command="sleep 5")
container.Wait()
print container['status']
container.Destroy()

"""

from . import exceptions
from .api import Connection
