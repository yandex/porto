#!/usr/bin/python -u

import porto

import common
from common import *
import targets

import random
from random import randint
from random import random as randf

import time
import os
import sys

inject_test_utils("/tmp")

random.seed(time.time() + os.getpid())
conn=porto.Connection(timeout=None)

FAIL_CNT = 0

for i in range(0, ITER):
    FAIL_CNT += select_by_weight(
        [
            (1, targets.container_action)
        ]
    )(conn)

print "Action performed: " + str(ITER) + " , failed: " + str(FAIL_CNT)
