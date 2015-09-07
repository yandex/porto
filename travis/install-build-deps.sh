#!/bin/bash

# gcc-4.7
sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test
sudo add-apt-repository -y ppa:canonical-kernel-team/ppa

sudo apt-get update -qq
sudo apt-get install -qq g++-4.7 flex bison libtool autoconf libncurses5-dev libprotobuf-dev protobuf-compiler linux-headers-goldfish

export CXX="g++-4.7" CC="gcc-4.7"