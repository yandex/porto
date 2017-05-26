apt-get clean
find var/cache/apt/archives -iname '*.deb' -delete
find var/lib/apt/lists -type f -delete
find var/cache/apt -iname '*.bin' -delete

find tmp -mindepth 1 -delete

rm -f etc/hostname
: > etc/hostname

ln -sf ../proc/self/mounts etc/mtab

find var/log -iname '*.gz' -delete
find var/log -iname '*.[0-9]' -delete
find var/log -type f -print0 | xargs -0 -t tee < /dev/null

: > /root/.bash_history
