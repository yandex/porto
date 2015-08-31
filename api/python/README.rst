PortoPy
--------

To initiate connection, simply do(portod must be started)::

    >>> from porto import Connection
    >>> rpc = Connection()
    >>> rpc.connect()

To create container *test*::

    >>> container = rpc.Create('test')

List all properties::

    >>> print rpc.Plist()

List data::

    >>> print rpc.Dlist()

Get *command* property of container *test*::

    >>> print rpc.GetProperty('test', 'command')
    >>> print container.GetProperty('command')  # the same

Get all properties of container::

    >>> print container.GetProperties()

List containers::

    >>> print rpc.List()

Destroy container *test*::

    >>> rpc.Destroy('test')

Close connection::

    >>> rpc.disconnect()
