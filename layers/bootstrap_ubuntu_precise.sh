debootstrap --foreign --exclude=ubuntu-minimal,resolvconf --arch amd64 precise . http://mirror.yandex.ru/ubuntu

# Do not create devices
tar cz -T /dev/null > debootstrap/devices.tar.gz

# Do not mount/umount anything
tee -a debootstrap/functions <<EOF
mount () { warning "" "skip mount \$*"; }
umount () { warning "" "skip umount \$*"; }
EOF
