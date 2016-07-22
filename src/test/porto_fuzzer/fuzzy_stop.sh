#!/bin/bash

PORTO_MASTER=$1
PORTO_SLAVE=$2

./porto_fuzzer.py
RET=$?

echo "Porto test finished"

if [ "$RET" -ne 0 ]; then
    sudo kill -SIGSTOP $PORTO_MASTER
    sudo kill -SIGSTOP $PORTO_SLAVE
    echo "Porto stopped"
fi
