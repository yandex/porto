PortoPy
--------

To initiate connection, simply do(portod must be started)::

    >>> from porto import PortoAPI
    >>> rpc = PortoAPI()
    >>> rpc.connect()


To create container *test*::

    >>> print rpc.Create('test')

List all properties::

    >>> print rpc.Plist()

List data::

    >>> print rpc.Dlist()

Get *command* property of container *test*::

    >>> print rpc.GetProperty('test', 'command')

List containers::

    >>> print rpc.List()

Destroy container *test*::

    >>> print rpc.Destroy('test')

Close connection::

    >>> rpc.Close()

