umask 0022

tee etc/apt/sources.list <<EOF
deb http://mirror.yandex.ru/debian stretch main contrib non-free
deb http://mirror.yandex.ru/debian stretch-updates main contrib non-free
deb http://mirror.yandex.ru/debian-security stretch/updates main contrib non-free
EOF

tee -a etc/inittab <<EOF
p0::powerfail:/sbin/init 0
EOF

export DEBIAN_FRONTEND="noninteractive"

APT_GET="apt-get --yes --no-install-recommends"

apt-get update

apt-get update

$APT_GET dist-upgrade
