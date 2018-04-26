export DEBIAN_FRONTEND="noninteractive"
apt-get --yes install 'debootstrap' '^(ubuntu-archive-keyring|ubuntu-keyring)$' 'debian-archive-keyring' 'debian-keyring'

[ -e usr/share/debootstrap/scripts/wily ] || ln -s gutsy usr/share/debootstrap/scripts/wily
[ -e usr/share/debootstrap/scripts/xenial ] || ln -s gutsy usr/share/debootstrap/scripts/xenial

# Ignore nodev for root
sed -e 's/check_sane_mount/true/' -i usr/sbin/debootstrap
