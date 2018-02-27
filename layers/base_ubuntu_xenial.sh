umask 0022

tee etc/apt/sources.list <<EOF
deb http://mirror.yandex.ru/ubuntu xenial main restricted universe multiverse
deb http://mirror.yandex.ru/ubuntu xenial-updates main restricted universe multiverse
deb http://mirror.yandex.ru/ubuntu xenial-security main restricted universe multiverse
EOF

systemctl mask console-getty.service

apt-get update

export DEBIAN_FRONTEND="noninteractive"
apt-get --yes --no-install-recommends dist-upgrade
