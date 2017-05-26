rm -f etc/resolv.conf
# https://dns.yandex.ru
tee etc/resolv.conf <<EOF
nameserver 77.88.8.8
nameserver 77.88.8.1
nameserver 2a02:6b8::feed:0ff
nameserver 2a02:6b8:0:1::feed:0ff
EOF
