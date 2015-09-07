#!/bin/bash

: ${FORCE_NEW_KERNEL_FLAGS:="1"}
export FORCE_NEW_KERNEL_FLAGS

# Trevis build platform doesn't have those newer Linux flags.
CXXFLAGS="-DPR_SET_CHILD_SUBREAPER=36 -DPR_GET_CHILD_SUBREAPER=37 -DCAP_BLOCK_SUSPEND=36"
if [[ "${FORCE_NEW_KERNEL_FLAGS}" -eq 1 ]]; then
  export CXXFLAGS
fi

export CXX="g++-4.7" CC="gcc-4.7"

cmake . && make clean && make
