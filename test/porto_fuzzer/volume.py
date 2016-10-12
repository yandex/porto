#!/usr/bin/python

import common
from common import *
from random import randint
import os

VERBOSE=True

def get_random_layers(conn, place):
    max_depth = randint(1, LAYER_LIMIT) 
    result = []

    for i in range(0, max_depth):
        layers = conn.ListLayers(place)
        if len(layers) == 0:
            result += [get_random_str(LAYERNAME_LIMIT)]
        else:
            result += [
                select_by_weight( [
                    (3, layers[randint(0, len(layers) - 1)].name),
                    (1, get_random_str(LAYERNAME_LIMIT))
                ] )
            ]

    return result

def Create(conn, pathstr):

    def select_storage():
        return select_by_weight( [
            (5, None),
            (2, get_random_dir(VOL_STORAGE)),
            (2, get_random_dir(VOL_MNT_PLACE)),
#            (1, get_random_dir(VOL_PLACE + "/porto_volumes")),
#            (1, get_random_dir(VOL_PLACE + "/porto_layers"))
        ] )

    def select_backend():
        return select_by_weight( [
            (10, None), 
            (5, "bind"),
            (5, "plain"),
            (5, "tmpfs"),
            (7, "overlay"),
#            (3, "quota"),
            (3, "native"),
            (3, "loop"),
#            (1, "rbd")       
        ] )

    def select_layers(conn, place):
        return select_by_weight( [
            (10, []),
            (8, ["ubuntu-precise"]),
            (5, get_random_layers(conn, place)),
        ] )

    def select_place():
        return select_by_weight( [
            (1, None),
            (1, VOL_PLACE)
        ] )

    if VERBOSE:
        print "Creating volume: {}".format(pathstr)

    kwargs = {}
    storage = select_storage()
    if storage is not None:
        kwargs["storage"] = storage

    backend = select_backend()
    if backend is not None:
        kwargs["backend"] = backend

    place = select_place()
    if place is not None:
        kwargs["place"] = place

    layers = select_layers(conn, place)
    if layers is not None:
        kwargs["layers"] = layers

    if "ubuntu-precise" in layers and backend != "overlay":
        #Skip operation
        return

    conn.CreateVolume(pathstr, **kwargs)

def Unlink(conn, pathstr, **kwargs):
    if VERBOSE:
        print "Unlinking volume: {} from container: {}".format(pathstr, kwargs["container"])

    if pathstr is None:
        pathstr = ""

    conn.UnlinkVolume(pathstr, **kwargs)

def Link(conn, pathstr, **kwargs):
    if VERBOSE:
        print "Linking volume: {} to container {}".format(pathstr, kwargs["container"])

    if pathstr is None:
        pathstr = ""

    conn.LinkVolume(pathstr, **kwargs)
