debootstrap --variant=minbase --include=debootstrap stable . http://mirror.yandex.ru/debian/
rm -f var/cache/apt/archives/*.deb
ln -s gutsy usr/share/debootstrap/scripts/wily
ln -s gutsy usr/share/debootstrap/scripts/xenial
