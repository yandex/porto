umask 0022

apt-get --yes --no-install-recommends install openssh-server

# Remove ssh host keys and regenerate at first boot
rm -f /etc/ssh/ssh_host_*key*
tee etc/cron.d/ssh-reconfigure <<EOF
@reboot root test -e /porto_build || ( /usr/sbin/dpkg-reconfigure openssh-server && rm /etc/cron.d/ssh-reconfigure )
EOF

umask 0077
mkdir -p /root/.ssh
touch /root/.ssh/authorized_keys
