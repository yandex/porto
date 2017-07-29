#!/bin/sh

set -ex

: ${COMPRESS=tgz}
: ${NET=NAT}
# ${NET=macvlan eth* eth0; autoconf eth0}
: ${PORTOCTL=portoctl}
: ${DST=$(pwd)}

cd $(dirname "$0")

rm -f "$DST/bootstrap.$COMPRESS"
sudo $PORTOCTL build -o "$DST/bootstrap.$COMPRESS" \
	-B bootstrap_ubuntu_xenial.sh \
	-S common-dns.sh \
	-S common-hosts.sh \
	-S base_ubuntu_xenial.sh \
	-S common-debootstrap.sh net="$NET"

$PORTOCTL layer -R bootstrap || true
$PORTOCTL layer -I bootstrap "$DST/bootstrap.$COMPRESS"

rm -f "$DST/ubuntu-precise.$COMPRESS"
$PORTOCTL build -l bootstrap -o "$DST/ubuntu-precise.$COMPRESS" \
	-B bootstrap_ubuntu_precise.sh \
	-S common-dns.sh \
	-S common-hosts.sh \
	-S base_ubuntu_precise.sh \
	-S common-misc.sh \
	-S common-openssh.sh \
	-S common-devel.sh \
	-S common-cleanup.sh net="$NET"

$PORTOCTL layer -R ubuntu-precise || true
$PORTOCTL layer -I ubuntu-precise "$DST/ubuntu-precise.$COMPRESS"

rm -f "$DST/ubuntu-xenial.$COMPRESS"
$PORTOCTL build -l bootstrap -o "$DST/ubuntu-xenial.$COMPRESS" \
	-B bootstrap_ubuntu_xenial.sh \
	-S common-dns.sh \
	-S common-hosts.sh \
	-S base_ubuntu_xenial.sh \
	-S common-misc.sh \
	-S common-openssh.sh \
	-S common-devel.sh \
	-S common-cleanup.sh net="$NET"

$PORTOCTL layer -R ubuntu-xenial || true
$PORTOCTL layer -I ubuntu-xenial "$DST/ubuntu-xenial.$COMPRESS"

rm -f "$DST/debian-jessie.$COMPRESS"
$PORTOCTL build -l bootstrap -o "$DST/debian-jessie.$COMPRESS" \
	-B bootstrap_debian_jessie.sh \
	-S common-dns.sh \
	-S common-hosts.sh \
	-S base_debian_jessie.sh \
	-S common-misc.sh \
	-S common-openssh.sh \
	-S common-devel.sh \
	-S common-cleanup.sh net="$NET"

$PORTOCTL layer -R debian-jessie || true
$PORTOCTL layer -I debian-jessie "$DST/debian-jessie.$COMPRESS"

rm -f "$DST/debian-stretch.$COMPRESS"
$PORTOCTL build -l bootstrap -o "$DST/debian-stretch.$COMPRESS" \
	-B bootstrap_debian_stretch.sh \
	-S common-dns.sh \
	-S common-hosts.sh \
	-S base_debian_stretch.sh \
	-S common-misc.sh \
	-S common-openssh.sh \
	-S common-devel.sh \
	-S common-cleanup.sh net="$NET"

$PORTOCTL layer -R debian-stretch || true
$PORTOCTL layer -I debian-stretch "$DST/debian-stretch.$COMPRESS"
