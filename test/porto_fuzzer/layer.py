#!/usr/bin/python

import common
from common import *
from random import randint
import os

VERBOSE=True

def Import(conn, place, layerstr):
    tar = select_by_weight([ (1, ""), (3, TAR1), (3, TAR2) ])
    if VERBOSE:
        print "Importing layer {} from {}".format(layerstr, tar)
    try:
        conn.ImportLayer(layerstr, tar, place=place, private_value=FUZZER_PRIVATE)
    except porto.exceptions.NoSpace:
        pass

def Merge(conn, place, layerstr):
    tar = select_by_weight([ (1, ""), (3, TAR1), (3, TAR2) ])
    if VERBOSE:
        print "Merging layer {} from {}".format(layerstr, tar)
    try:
        conn.MergeLayer(layerstr, tar, place=place, private_value=FUZZER_PRIVATE)
    except porto.exceptions.NoSpace:
        pass

def Remove(conn, place, layerstr):
    if VERBOSE:
        print "Removing layer {}".format(layerstr)
    conn.RemoveLayer(layerstr, place=place)
