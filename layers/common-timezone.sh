: ${TIMEZONE=Europe/Moscow}

echo "${TIMEZONE}" > /etc/timezone

# https://bugs.launchpad.net/ubuntu/+source/tzdata/+bug/1554806
ln -fs /usr/share/zoneinfo/"${TIMEZONE}" /etc/localtime

dpkg-reconfigure -f noninteractive tzdata
