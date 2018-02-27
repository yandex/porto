# Do not create devices in package makedev
cp -a /dev/null /dev/.devfsd

/debootstrap/debootstrap --second-stage
