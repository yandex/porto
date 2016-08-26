apt-get clean
rm -f /var/cache/apt/archives/*.deb
rm -rf /var/lib/apt/lists/*
rm -f /var/cache/apt/*.bin
rm -rf /tmp/*

: > /etc/hostname
: > /etc/resolv.conf
: > /etc/mtab

find /var/log -iname '*.gz' -delete
find /var/log -iname '*.1' -delete
find /var/log -type f -print0 | xargs -0 -t tee < /dev/null

: > /root/.bash_history
