#!/bin/bash

SRC=$1
DST=$2

mkdir -p $DST

cp $SRC/Makefile $DST/Makefile
cp $SRC/porto_kernel.c $DST/porto_kernel.c

cd $DST

make clean

make
