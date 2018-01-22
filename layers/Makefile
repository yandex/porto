COMPRESS=tgz
NET=NAT
# NET=macvlan eth* eth0; autoconf eth0
PORTOCTL=portoctl

define build
sudo ${PORTOCTL} build -o "$1.${COMPRESS}" $2 net="${NET}"
-${PORTOCTL} layer -R "$1"
${PORTOCTL} layer -I $1 "$1.${COMPRESS}"
endef

all: bootstrap ubunu-xenial

clean:
	rm -f *.${COMPRESS}

bootstrap:
	$(call build,$@, \
		-B bootstrap_ubuntu_xenial.sh \
		-S common-dns.sh \
		-S common-hosts.sh \
		-S base_ubuntu_xenial.sh \
		-S common-debootstrap.sh \
		-S common-cleanup.sh \
	)

ubuntu-precise:
	$(call build,$@, \
		-B bootstrap_ubuntu_precise.sh \
		-S common-dns.sh \
		-S common-hosts.sh \
		-S base_ubuntu_precise.sh \
		-S common-misc.sh \
		-S common-openssh.sh \
		-S common-devel.sh \
		-S common-cleanup.sh \
	)


ubuntu-xenial:
	$(call build,$@, \
		-B bootstrap_ubuntu_xenial.sh \
		-S common-dns.sh \
		-S common-hosts.sh \
		-S base_ubuntu_xenial.sh \
		-S common-misc.sh \
		-S common-openssh.sh \
		-S common-devel.sh \
		-S common-cleanup.sh \
	)

ubuntu-bionic:
	$(call build,$@, \
		-B bootstrap_ubuntu_bionic.sh \
		-b bootstrap2_ubuntu_bionic.sh \
		-S common-dns.sh \
		-S common-hosts.sh \
		-S base_ubuntu_bionic.sh \
		-S common-misc.sh \
		-S common-openssh.sh \
		-S common-devel.sh \
		-S common-cleanup.sh \
	)
