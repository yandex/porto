Porto
=====

# OVERVIEW #

Porto is a Linux containers management system, developed by Yandex.
The main goal of Porto is to provide a convenient reliable interface over several
Linux kernel mechanism such as cgroups, namespaces, mounts, networking etc.
Porto is intended to be used as a base for building large infrastructure projects.
Porto provides protobuf-based interface via unix socket. C++ and Python APIs are included.
A command line tool (portoctl) for managing Porto-containers is also provided.

Porto has the following key-features:
* **Container hierarchies:** you can manage nested groups of containers, limit theirs
aggregate resource usage and share resources between containers in a flexible manner.
* **Container namespaces:** you can put any container-management software into container
without any modifications.
* **Flexible isolation:** you can control which resources are isolated. You can easily integrate
Porto into your existing infrastructure.

# BUILDING #

```
$ git submodule init
$ git submodule update
$ cmake .
$ make
$ make install DESTDIR=/usr/local
```

# RUNNING #

Porto requires only Linux kernel 3.4+, although some functionality requires
newer kernels (for instance, 3.18+ for OverlayFs) and even offstream patches.

```
$ sudo portod &
$ portoctl run my_container command='echo "Hello, world!"'
$ portoctl wait my_container
$ portoctl get my_container stdout
$ portoctl get my_container exit_status
$ portoctl destroy my_container
```
or
```
$ portoctl exec my_container command='echo "Hello, world!"'
```

# DOCUMENTATION #
* [Quick start](docs/quick.md)
* [Concepts](docs/concepts.md)
* [Containers](docs/containers.md)
* [Container namespaces](docs/namespaces.md)
* [Container properties and data](docs/properties.md)
* [Managing physical resources](docs/limits.md)
* [Filesystem isolation](docs/mounts.md)
* [Volumes](docs/volumes.md)
* [Networking](docs/networking.md)
* [C++ API](src/api/cpp/libporto.hpp)
* [Python API](src/api/python/porto/api.py)
* [How to contribute?](docs/devel.md)
