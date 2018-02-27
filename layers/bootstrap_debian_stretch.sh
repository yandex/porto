debootstrap --foreign --variant=minbase --include systemd-sysv --arch amd64 stretch . http://mirror.yandex.ru/debian/

# Do not mount/umount anything
tee -a debootstrap/functions <<EOF
mount () { warning "" "skip mount \$*"; }
umount () { warning "" "skip umount \$*"; }
EOF
