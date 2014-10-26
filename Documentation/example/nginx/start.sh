#!/bin/bash

NGINX_PWD=$CWD
NGINX_PID=$NGINX_PWD/nginx.pid

on_die() {
    echo "Kill nginx!!!"
    kill -15 `cat $NGINX_PID`
    if [ $? -ne  0 ]; then
        sleep 2
        kill -9 `cat $NGINX_PID`
    fi
    exit 0
}

trap "on_die" SIGINT SIGTERM

$NGINX_PWD/nginx -c ./nginx.conf

while true; do
    sleep 1
    if [ -f $NGINX_PID ]; then
        kill -0 `cat  $NGINX_PID`
        if [ $? -ne 0 ];  then
            exit 1;
        fi
    else
            exit 2;
    fi
done
