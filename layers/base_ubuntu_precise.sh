/debootstrap/debootstrap --second-stage

umask 0022

tee etc/apt/sources.list <<EOF
deb http://mirror.yandex.ru/ubuntu precise main restricted universe multiverse
deb http://mirror.yandex.ru/ubuntu precise-security main restricted universe multiverse
deb http://mirror.yandex.ru/ubuntu precise-updates main restricted universe multiverse
EOF

# Handle SIGPWR
tee etc/init/power-status-changed.conf <<EOF
start on power-status-changed
exec /sbin/shutdown -h now
EOF

apt-get update

export DEBIAN_FRONTEND="noninteractive"
apt-get --yes --no-install-recommends dist-upgrade

# Do not mount anything at boot
sed -e 's/^/#/g' -i lib/init/fstab

# Fix start procps, ignore sysctl errors
sed -e 's#\(| sysctl -e -p -$\)#\1 || true#' -i etc/init/procps.conf
