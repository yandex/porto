#!/usr/bin/python

from common import *

import os
import random
import functools
import porto
import container
import volume
import layer

def random_container():
    return FUZZER_PRIVATE + '-' + get_random_str(NAME_LIMIT)

def our_containers(conn):
    return conn.List(mask=FUZZER_PRIVATE+"-***")

def our_container(conn):
    return select_equal(our_containers(conn), random_container())

def our_volumes(conn):
    our = []
    for v in conn.ListVolumes():
        try:
            if v.GetProperty('private') == FUZZER_PRIVATE:
                our.append(v.path)
        except porto.exceptions.PortoException:
            pass
    return our

def our_volume(conn):
    return select_equal(our_volumes(conn), "")

def our_layers(conn, place=None):
    our = []
    for l in conn.ListLayers(place=place):
        try:
            if l.GetPrivate() == FUZZER_PRIVATE:
                our.append(l.name)
        except porto.exceptions.PortoException:
            pass
    return our

def all_layers(conn, place=None):
    return [ l.name for l in conn.ListLayers(place=place) ]

def get_random_layers(conn, place=None):
    max_depth = randint(1, LAYER_LIMIT)
    result = []
    layers = all_layers(conn, place)
    for i in range(0, max_depth):
        result.append(select_by_weight( [
            (3, select_equal(layers, get_random_str(LAYERNAME_LIMIT))),
            (1, get_random_str(LAYERNAME_LIMIT))
        ]))

    return result

def select_layers(conn, place=None):
    return select_by_weight( [
        (10, []),
        (8, ["ubuntu-precise"]),
        (5, get_random_layers(conn, place)),
    ] )

def select_container(conn):
    return select_by_weight( [
        (1, lambda conn: "/"),
        (1, lambda conn: "self"),
        (50, our_container),
        (20, lambda conn: random_container()),
        (10, lambda conn: random_container() + "/" + random_container()),
        (5, lambda conn: random_container() + "/" + random_container() + "/" + random_container()),
        (2, lambda conn: our_container(conn) + "/" + random_container()),
        (1, lambda conn: our_container(conn) + "/" + random_container() + "/" + random_container()),
    ] )(conn)

def select_volume_container(conn):
    return select_by_weight( [
        (10, "/"),
        (10, "self"),
        (10, None),
        (10, "***"),
        (100, select_container(conn))
    ] )

def select_volume(conn):
    return select_by_weight( [
        (15, None),
        (20, our_volume(conn)),
        (30, get_random_dir(VOL_MNT_PLACE))
    ] )

def volume_action(conn):
    select_by_weight( [
        (1, volume.Create),
        (2, volume.UnlinkVolume),
        (2, volume.UnlinkVolumeStrict),
        (2, volume.LinkVolume),
        (2, volume.LinkVolumeTarget),
    ] )(conn, select_volume(conn))

def select_place():
    return select_by_weight( [
        (1, None),
        (1, VOL_PLACE)
    ] )

def layer_action(conn):
    place = select_place()
    name = select_by_weight( [
        (5, select_equal(our_layers(conn, place), get_random_str(LAYERNAME_LIMIT))),
        (5, get_random_str(LAYERNAME_LIMIT))
    ] )
    select_by_weight( [
        (1, layer.Import),
        (2, layer.Merge),
        (2, layer.Remove)
    ] )(conn, place, name)

def container_action(conn):
    select_by_weight( [
        (75, container.Create),
        (20, container.Destroy),
        (150, container.SetProperty),
        (40, container.Start),
        (30, container.Stop),
#       (15, container.Pause),
#       (15, container.Resume),
        (15, container.Wait),
        (25, container.Kill)
    ] )(conn, select_container(conn))

def set_verbose(is_verbose):
    container.VERBOSE = is_verbose
    volume.VERBOSE = is_verbose
    layer.VERBOSE = is_verbose

def set_active(is_active):
    container.set_active(is_active)
