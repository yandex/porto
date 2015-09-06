#!/bin/bash

# gcc-4.7
sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test

sudo apt-get update -qq
sudo apt-get install -qq g++-4.7 flex bison libtool autoconf libncurses5-dev libprotobuf-dev protobuf-compiler

export CXX="g++-4.7" CC="gcc-4.7"