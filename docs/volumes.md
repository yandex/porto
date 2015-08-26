# OVERVIEW #
Porto provides "volumes" abstraction to manage disk space. Each volume is identified by full mount path. A volume can be linked to one or more containers, unlinked volume will be destroyed automatically.
After creation a volume is linked to the container that created the volume ("/" for host). Then the container can create new links and optionally unlink the volume from itself.

You can ether manually declare a mounting path for a new volume or delegate this task to Porto:
```
$ portoctl vcreate </path/to/volume>
```
or
```
$ path=$(portoctl vcreate -A)
$ echo $path
```
Please, follow the following rules when specifying the path manually:
* use an absolute path (e.g. /home/user/volumes/my\_volume)
* a path should specify an existing directory
* you should have write permissions to this directory

# Single-layered volumes #
The following commands will create a volume with specified disk quota (5Gb) and use them as a root filesystem of a new contaianer:
```
$ path=$(portoctl vcreate -A space_limit=5G)
$ sudo portoctl exec bootstrap command="debootstrap --exclude=ubuntu-minimal,resolvconf --arch amd64 precise $path http://mirror.yandex.ru/ubuntu"
$ sudo chown $USER $path
$ portoctl exec ubuntu command="/bin/cat /etc/lsb-release" root="$path"
DISTRIB_ID=Ubuntu
DISTRIB_RELEASE=12.04
DISTRIB_CODENAME=precise
DISTRIB_DESCRIPTION="Ubuntu 12.04 LTS"
$ portoctl vunlink $path
```
Disk quotas are implemented via using loopback device or, if available, ext4 project quotas (this functionality requires offstream patches for Linux kernel).
If you want to automatically destroy the linked volume when the container is destroyed, use the following commands:
```
$ path=$(portoctl vcreate -A)
$ sudo portoctl exec bootstrap command="debootstrap --exclude=ubuntu-minimal,resolvconf --arch amd64 precise $path http://mirror.yandex.ru/ubuntu"
$ sudo chown $USER $path
$ portoctl run ubuntu command="sleep 1000" root="$path"
$ portoctl vlist
Volume                                     Limit    Used   Avail Use% Containers
/place/porto_volumes/1/volume               5.0G  190.5M    4.8G   4% /
# Link volume with container "ubuntu":
$ portoctl vlink $path ubuntu
# Unlink volume from root container (every volume has a link to container from which it was created)
$ portoctl vunlink $path /
```
You can see, that the volume is now linked to the "ubuntu" container.
```
$ portoctl vlist
Volume                                     Limit    Used   Avail Use% Containers
/place/porto_volumes/1/volume                  -   62.7G    1.6T   4% ubuntu
$ portoctl destroy ubuntu
$ portoctl vlist
Volume                                     Limit    Used   Avail Use% Containers
```
You can also create a new volume using an already existing directory and bind them to a container:
```
$ mkdir /tmp/portoctl-test
$ touch /tmp/portoctl-test/hello
$ portoctl vcreate /tmp/portoctl-test space_limit=5G
$ portoctl exec ls command="ls -la test" bind="/tmp/portoctl-test test"
total 8
drwxrwxr-x 2 stfomichev dpt_yandex_search_tech_searchinfradev_linux 4096 Aug 10 18:56 .
drwxr-xr-x 3 stfomichev dpt_yandex_search_tech_searchinfradev_linux 4096 Aug 10 18:59 ..
-rw-rw-r-- 1 stfomichev dpt_yandex_search_tech_searchinfradev_linux    0 Aug 10 18:56 hello
$ portoctl vunlink /tmp/portoctl-test
```
# Multi-layered volumes #
If your Linux kernel supports OverlayFS (in general case, you need 3.18+ kernel), you can create multi-layered volumes.
Each layer consists of either absolute path to a directory with data, or a name of a layer in an internal layer storage. Internal layer storage is managed by portoctl layer command.

To create a volume using two directories /tmp/top and /tmp/bottom, use the following commands:
```
$ mkdir /tmp/top /tmp/bottom
$ touch /tmp/top/top /tmp/bottom/bottom
$ path=$(portoctl vcreate -A layers="/tmp/top; /tmp/bottom")
$ ls $path
bottom  top
$ portoctl vunlink $path
```
The internal layer storage can be used to extract filesystem images from tarballs in case, when a user doesn't have enough rights on a system. Porto will extract files in a safe-manner (caring about suid bits, for example).
An example is shown below:
```
$ tar czf bottom.tar -C /tmp/bottom bottom
$ tar czf top.tar -C /tmp/top top
$ portoctl layer -I top $PWD/top.tar
$ portoctl layer -I bottom $PWD/bottom.tar
$ portoctl layer -L
top
bottom
$ path=$(portoctl vcreate -A layers="top; bottom")
$ ls $path
bottom  top
$ portoctl vunlink $path
```
A user has to destroy layer manually (there is no link/unlink functionality for layers, only for volumes):
```
$ portoctl layer -L
top
bottom
$ portoctl layer -R top
$ portoctl layer -R bottom
$ portoctl layer -L
```
A tarball with a layer can be in one of two following formats:
* overlayfs
* aufs (used by Docker)
If you want to use existing Docker images, you have two options:
* you can import each aufs layer as a separate layer, or
* you can merge them sequentially into one combined layer (please, refer to the portoctl layer command built-in help).
You can find some additional information about using layers in docker2porto script, which is intended to run Docker images by Porto.
# Persistency #
* If a volume was created with auto-generated path, the data on this volume will be destroyed automatically with the volume (when the last link to the volume is dropped).
* Otherwise, if a user has specified the path manually, Porto doesn't destroy data at this path.

A user can create a new volume with existing data:
```
$ portoctl vcreate -A storage=/path/to/storage
```
