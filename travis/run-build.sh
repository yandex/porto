#!/bin/bash

: ${FORCE_NEW_KERNEL_FLAGS:="1"}
export FORCE_NEW_KERNEL_FLAGS

export CXX="g++-4.7" CC="gcc-4.7"

cmake . && make clean && make
