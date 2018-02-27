umask 0022

tee etc/apt/sources.list <<EOF
deb http://mirror.yandex.ru/debian stretch main contrib non-free
deb http://mirror.yandex.ru/debian stretch-updates main contrib non-free
deb http://mirror.yandex.ru/debian-security stretch/updates main contrib non-free
EOF

apt-get update

export DEBIAN_FRONTEND="noninteractive"
apt-get --yes --no-install-recommends dist-upgrade
