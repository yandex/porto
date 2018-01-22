# Do not create devices in package makedev
cp -a /dev/null /dev/.devfsd

/debootstrap/debootstrap --second-stage

umask 0022

tee etc/apt/sources.list <<EOF
deb http://mirror.yandex.ru/ubuntu xenial main restricted universe multiverse
deb http://mirror.yandex.ru/ubuntu xenial-updates main restricted universe multiverse
deb http://mirror.yandex.ru/ubuntu xenial-security main restricted universe multiverse
EOF

systemctl mask console-getty.service

export DEBIAN_FRONTEND="noninteractive"

APT_GET="apt-get --yes --no-install-recommends"

apt-get update

$APT_GET dist-upgrade
