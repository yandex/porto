#!/usr/bin/python
# -*- coding: utf-8 -*-

import time
import sys
import os

################################################################################

def container_create(name):
    print('Create porto container: ', name)

if __name__ == '__main__':
    try:
        container_create('c1')
        sys.exit(0)
    except KeyboardInterrupt:
        print('Terminated by user')
        raise
