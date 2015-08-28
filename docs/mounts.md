By default container shares filesystem with host.
This might be changed via root property, which isolates container from the host filesystem.

* bind - bind host directory into container, syntax: <host path> <container path> [ro|rw]; ...
* bind\_dns - bind host /etc/resolv.conf and /etc/hosts into container; may be used for app containers which have isolated filesystem but want to use host network
* root - container root directory (man 1 chroot)
* root\_readonly - mount root directory as readonly
* cwd - container working directory; cwd is relative to root, i.e. firt chroot(root), then chdir(cwd); by default:
  - if root = /, then /place/porto/<container>
  - if root != /, then /

# Examples

```
mkdir /tmp/root
mkdir /tmp/shared
debootstrap --exclude=ubuntu-minimal,resolvconf --arch amd64 trusty /tmp/root http://mirror.yandex.ru/ubuntu
portoctl exec trusty command=/bin/bash root=/tmp/root bind_dns=true bind='/tmp/shared /shared ro'
```
