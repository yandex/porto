debootstrap --foreign --variant=minbase --include systemd-sysv,tzdata,locales --arch amd64 bionic . http://mirror.yandex.ru/ubuntu

# Do not mknod/mount/umount anything
tee -a debootstrap/functions <<EOF
mknod () { warning "" "skip mknod \$*"; }
mount () { warning "" "skip mount \$*"; }
umount () { warning "" "skip umount \$*"; }
EOF
