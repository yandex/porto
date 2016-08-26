/debootstrap/debootstrap --second-stage

umask 0022

tee etc/apt/sources.list <<EOF
deb http://mirror.yandex.ru/ubuntu precise main restricted universe multiverse
deb http://mirror.yandex.ru/ubuntu precise-security main restricted universe multiverse
deb http://mirror.yandex.ru/ubuntu precise-updates main restricted universe multiverse
EOF

tee etc/hosts <<EOF
127.0.0.1       localhost
::1             localhost ip6-localhost ip6-loopback
fe00::0         ip6-localnet
ff00::0         ip6-mcastprefix
ff02::1         ip6-allnodes
ff02::2         ip6-allrouters
EOF

# Fix start procps, ignore sysctl errors
sed -e 's#\(| sysctl -e -p -$\)#\1 || true#' -i etc/init/procps.conf

export DEBIAN_FRONTEND="noninteractive"

APT_GET="apt-get --yes --no-install-recommends"

apt-get update

$APT_GET dist-upgrade
