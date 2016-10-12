#!/usr/bin/python

import common
from common import *
from random import randint
import os

VERBOSE=True

def Import(conn, layerstr):
    tar = select_by_weight([ (1, ""), (3, TAR1), (3, TAR2) ])
    if VERBOSE:    
        print "Importing layer {} from {}".format(layerstr, tar)
    place = select_by_weight([ (1, None), (1, VOL_PLACE)])
    conn.ImportLayer(layerstr, tar, **{"place" : place,})

def Merge(conn, layerstr):
    tar = select_by_weight([ (1, ""), (3, TAR1), (3, TAR2) ])
    if VERBOSE:
        print "Merging layer {} from {}".format(layerstr, tar)
    place = select_by_weight([ (1, None), (1, VOL_PLACE)])
    conn.MergeLayer(layerstr, tar, **{"place" : place,})

def Remove(conn, layerstr):
    if VERBOSE:
        print "Removing layer {}".format(layerstr)
    place = select_by_weight([ (1, None), (1, VOL_PLACE)])
    conn.RemoveLayer(layerstr, **{"place" : place,})
