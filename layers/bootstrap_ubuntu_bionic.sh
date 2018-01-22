debootstrap --foreign --variant=minbase --include systemd-sysv --arch amd64 bionic . http://mirror.yandex.ru/ubuntu

# Do not mount/umount anything
tee -a debootstrap/functions <<EOF
mount () { warning "" "skip mount \$*"; }
umount () { warning "" "skip umount \$*"; }
EOF
