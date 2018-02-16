#!/usr/bin/python

from common import *
import targets

import os

VERBOSE=True

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

    place = targets.select_place()
    if place is not None:
        kwargs["place"] = place

    layers = targets.select_layers(conn, place)
    if layers is not None:
        kwargs["layers"] = layers

    if "ubuntu-precise" in layers and backend != "overlay":
        #Skip operation
        return

    kwargs['private'] = FUZZER_PRIVATE

    conn.CreateVolume(pathstr, **kwargs)

def UnlinkVolume(conn, pathstr):
    ct = targets.select_volume_container(conn)

    if VERBOSE:
        print "Unlinking volume: {} from container: {}".format(pathstr, ct)

    if pathstr is None:
        pathstr = ""

    conn.UnlinkVolume(pathstr, ct)

def UnlinkVolumeStrict(conn, pathstr):
    ct = targets.select_volume_container(conn)

    if VERBOSE:
        print "Unlinking volume: {} from container: {}".format(pathstr, ct)

    if pathstr is None:
        pathstr = ""

    conn.UnlinkVolume(pathstr, ct, strict=True)

def LinkVolume(conn, pathstr):

    ct = targets.select_volume_container(conn)

    if VERBOSE:
        print "Linking volume: {} to container {}".format(pathstr, ct)

    if pathstr is None:
        pathstr = ""

    if ct is None:
        ct = ""

    conn.LinkVolume(pathstr, ct)

def LinkVolumeTarget(conn, pathstr):

    ct = targets.select_volume_container(conn)
    target = get_random_dir(FUZZER_MNT)

    if VERBOSE:
        print "Linking volume: {} to container {} to {}".format(pathstr, ct, target)

    if pathstr is None:
        pathstr = ""

    if ct is None:
        ct = ""

    conn.LinkVolume(pathstr, ct, target=target)
