# Do not create devices in package makedev
cp -a /dev/null /dev/.devfsd

/debootstrap/debootstrap --second-stage

umask 0022

tee etc/apt/sources.list <<EOF
deb http://mirror.yandex.ru/ubuntu xenial main restricted universe multiverse
deb http://mirror.yandex.ru/ubuntu xenial-updates main restricted universe multiverse
deb http://mirror.yandex.ru/ubuntu xenial-security main restricted universe multiverse
EOF

# Handle SIGPWR
tee etc/init/power-status-changed.conf <<EOF
start on power-status-changed
exec /sbin/shutdown -h now
EOF

# Do not mount anything at boot
sed -e 's/^/#/g' -i lib/init/fstab

apt-get update

export DEBIAN_FRONTEND="noninteractive"
apt-get --yes --no-install-recommends dist-upgrade
