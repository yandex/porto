umask 0022

tee etc/apt/sources.list <<EOF
deb http://mirror.yandex.ru/debian jessie main contrib non-free
deb http://mirror.yandex.ru/debian jessie-updates main contrib non-free
deb http://mirror.yandex.ru/debian-security jessie/updates main contrib non-free
EOF

tee etc/hosts <<EOF
127.0.0.1       localhost
::1             localhost ip6-localhost ip6-loopback
fe00::0         ip6-localnet
ff00::0         ip6-mcastprefix
ff02::1         ip6-allnodes
ff02::2         ip6-allrouters
EOF

export DEBIAN_FRONTEND="noninteractive"

APT_GET="apt-get --yes --no-install-recommends"

apt-get update

$APT_GET dist-upgrade

$APT_GET install cron less procps psmisc python vim wget
