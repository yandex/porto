debootstrap --foreign --variant=minbase --exclude systemd-sysv --include upstart-sysv --arch amd64 xenial . http://mirror.yandex.ru/ubuntu

# Use upstart for /sbin/init
sed -i -e 's/systemd-sysv /upstart-sysv /g' debootstrap/required

# Do not create devices
tar cz -T /dev/null > debootstrap/devices.tar.gz

# Do not mount/umount anything
tee -a debootstrap/functions <<EOF
mount () { warning "" "skip mount \$*"; }
umount () { warning "" "skip umount \$*"; }
EOF
