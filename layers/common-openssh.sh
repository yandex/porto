umask 0022

apt-get --yes --no-install-recommends install openssh-server

# Remove ssh host keys and regenerate at first boot
rm -f /etc/ssh/ssh_host_*key*
tee etc/cron.d/ssh-reconfigure <<EOF
@reboot root /usr/sbin/dpkg-reconfigure openssh-server && rm /etc/cron.d/ssh-reconfigure
EOF
