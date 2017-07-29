Porto
=====

[![Build Status](https://travis-ci.org/yandex/porto.svg?branch=master)](https://travis-ci.org/yandex/porto)

# OVERVIEW #

Porto is a yet another Linux container management system, developed by Yandex.
The main goal is providing single entry point for several Linux subsystems
such as cgroups, namespaces, mounts, networking, etc.

Key Features:
* **Nested virtualization**   - containers could be put into containers
* **Unified access**          - containers could use porto service too
* **Flexible configuration**  - all container parameters are optional
* **Reliable service**        - porto upgrades without restarting containers

Porto is intended to be a base for large infrastructure projects.
Container management software build on top of porto could be transparently
enclosed inside porto container.

Porto provides a protobuf interface via an Unix socket.
Command line tool (portoctl) and C++, Python and Go APIs are included.

# BUILDING #

```
$ dpkg-buildpackage -b
$ sudo dpkg -i ../yandex-porto_*.deb
```
or
```
$ sudo apt-get install protobuf-compiler libprotobuf-dev libnl-3-dev libnl-route-3-dev libncurses5-dev
$ cmake .
$ make
$ make install DESTDIR=/usr/local
```

# RUNNING #

Porto requires Linux kernel 3.18 and optionally some offstream patches.

```
$ sudo groupadd porto
$ sudo sudo adduser $USER porto
$ sudo portod start
$ portoctl exec hello command='echo "Hello, world!"'
```

# DOCUMENTATION #
* [Porto manpage](porto.md)
