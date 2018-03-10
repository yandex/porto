COMPRESS=tgz
NET=NAT
# NET=macvlan eth* eth0; autoconf eth0
PORTOCTL=portoctl

define build
	${PORTOCTL} build -o "$1" $2 net="${NET}"
	-${PORTOCTL} layer -R "${1:.${COMPRESS}=}"
	${PORTOCTL} layer -I "${1:.${COMPRESS}=}" "$1"
endef

define second_layer
${1}-${2}.${COMPRESS}:
$(call build,${1}-${2}.${COMPRESS}, -L $1 -S common-update.sh -S $3 -S common-cleanup.sh)
endef

all: put_authorized_keys.sh bootstrap.${COMPRESS} ubuntu-xenial.${COMPRESS} ubuntu-xenial-devel.${COMPRESS}

clean:
	rm -f put_authorized_keys.sh *.${COMPRESS}

put_authorized_keys.sh:
	printf "tee -a /root/.ssh/authorized_keys <<EOF\n$(shell cat ${HOME}/.ssh/id_rsa.pub)\nEOF\n" > $@

bootstrap.${COMPRESS}:
	sudo $(call build,$@, \
		-B bootstrap_ubuntu_xenial.sh \
		-b bootstrap2_ubuntu_xenial.sh \
		-S common-dns.sh \
		-S common-hosts.sh \
		-S base_ubuntu_xenial.sh \
		-S common-timezone.sh \
		-S common-locale.sh \
		-S common-debootstrap.sh \
		-S common-cleanup.sh \
	)

ubuntu-precise.${COMPRESS}:
	$(call build,$@, \
		-l bootstrap \
		-B bootstrap_ubuntu_precise.sh \
		-S common-dns.sh \
		-S common-hosts.sh \
		-S base_ubuntu_precise.sh \
		-S common-timezone.sh \
		-S common-locale.sh \
		-S common-misc.sh \
		-S common-openssh.sh \
		-S put_authorized_keys.sh \
		-S common-cleanup.sh \
	)


ubuntu-xenial.${COMPRESS}:
	$(call build,$@, \
		-l bootstrap \
		-B bootstrap_ubuntu_xenial.sh \
		-b bootstrap2_ubuntu_xenial.sh \
		-S common-dns.sh \
		-S common-hosts.sh \
		-S base_ubuntu_xenial.sh \
		-S common-timezone.sh \
		-S common-locale.sh \
		-S common-misc.sh \
		-S common-openssh.sh \
		-S put_authorized_keys.sh \
		-S common-cleanup.sh \
	)

ubuntu-bionic.${COMPRESS}:
	$(call build,$@, \
		-l bootstrap \
		-B bootstrap_ubuntu_bionic.sh \
		-b bootstrap2_ubuntu_bionic.sh \
		-S common-dns.sh \
		-S common-hosts.sh \
		-S base_ubuntu_bionic.sh \
		-S common-timezone.sh \
		-S common-locale.sh \
		-S common-misc.sh \
		-S common-openssh.sh \
		-S put_authorized_keys.sh \
		-S common-cleanup.sh \
	)

debian-stretch.${COMPRESS}:
	$(call build,$@, \
		-l bootstrap \
		-B bootstrap_debian_stretch.sh \
		-b bootstrap2_debian_stretch.sh \
		-S common-dns.sh \
		-S common-hosts.sh \
		-S base_debian_stretch.sh \
		-S common-timezone.sh \
		-S common-misc.sh \
		-S common-openssh.sh \
		-S put_authorized_keys.sh \
		-S common-cleanup.sh \
	)

$(eval $(call second_layer,ubuntu-precise,devel,common-devel.sh))
$(eval $(call second_layer,ubuntu-xenial,devel,common-devel.sh))
$(eval $(call second_layer,ubuntu-bionic,devel,common-devel.sh))
$(eval $(call second_layer,debian-stretch,devel,common-devel.sh))
