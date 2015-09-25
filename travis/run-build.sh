#!/bin/bash

export CXX="g++-4.7" CC="gcc-4.7"

mkdir build && cd build && cmake ../ && make
