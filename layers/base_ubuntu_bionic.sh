umask 0022

tee etc/apt/sources.list <<EOF
deb http://mirror.yandex.ru/ubuntu bionic main restricted universe multiverse
deb http://mirror.yandex.ru/ubuntu bionic-updates main restricted universe multiverse
deb http://mirror.yandex.ru/ubuntu bionic-security main restricted universe multiverse
EOF

systemctl mask console-getty.service

export DEBIAN_FRONTEND="noninteractive"

APT_GET="apt-get --yes --no-install-recommends"

apt-get update

$APT_GET dist-upgrade
