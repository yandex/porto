#!/usr/bin/python

import os
import porto
import container
import volume
import layer

from common import *

import random
from random import randint
from random import random as randf
import functools

def existing_container(conn):
    existing = conn.List()
    if len(existing) == 0:
        return get_random_str(NAME_LIMIT)
    return existing[randint(0, len(existing) - 1)]

def existing_volume(conn):
    existing = conn.ListVolumes()
    if len(existing) == 0:
        return ""
    return existing[randint(0, len(existing) - 1)].path

def existing_layer(conn):
    place = select_by_weight([(1, None), (1, VOL_PLACE)])
    existing = conn.ListLayers(**{"place" : place})
    if len(existing) == 0:
        return ""
    layer = existing[randint(0, len(existing) - 1)].name

    if layer == "ubuntu-precise" or layer == "ubuntu-xenial":
        return ""

    return layer

def select_container(conn):
    return select_by_weight( [
        (1, select_by_weight( [
            (1, "/"),
            ] )
        ),
        (45, existing_container(conn)),
        (40, select_by_weight( [
                (60, get_random_str(NAME_LIMIT)),
                (40, existing_container(conn) + "/" + get_random_str(NAME_LIMIT))
            ] )
        )
    ] )

def select_volume_container(conn):
    return select_by_weight( [
        (30, "/"),
        (40, select_container(conn))
    ] )

def select_volume(conn):
    return select_by_weight( [
        (15, None),
        (20, existing_volume(conn)),
        (30, get_random_dir(VOL_MNT_PLACE))
    ] )

def select_layer(conn):
    return select_by_weight( [
        (5, existing_layer(conn)),
        (5, get_random_str(LAYERNAME_LIMIT))
    ] )

def volume_action(conn):
    try:
        ct = select_volume_container(conn)
        select_by_weight( [
            (1, volume.Create),
            (2, functools.partial(volume.Unlink, **{"container" : ct})),
            (2, functools.partial(volume.Link, **{"container" : ct})),
        ] )(conn, select_volume(conn))
        return 0
    except (
            porto.exceptions.VolumeNotFound,
            porto.exceptions.ContainerDoesNotExist,
            porto.exceptions.PermissionError,
            porto.exceptions.VolumeAlreadyExists,
            porto.exceptions.VolumeAlreadyLinked,
            porto.exceptions.VolumeNotLinked,
            porto.exceptions.VolumeNotReady,
            porto.exceptions.InvalidValue,
            porto.exceptions.Busy,
            porto.exceptions.ResourceNotAvailable,
            porto.exceptions.InvalidProperty,
            porto.exceptions.LayerNotFound,
            porto.exceptions.UnknownError
           ):
        return 1

def layer_action(conn):
    try:
        select_by_weight( [
            (1, layer.Import),
            (2, layer.Merge),
            (2, layer.Remove)
        ] )(conn, select_layer(conn))
        return 0
    except (
            porto.exceptions.LayerAlreadyExists,
            porto.exceptions.LayerNotFound,
            porto.exceptions.InvalidValue,
            porto.exceptions.Busy,
            porto.exceptions.UnknownError
           ):
        return 1
     

def container_action(conn):
    try:
        select_by_weight( [
                (75, container.Create),
                (20, container.Destroy),
                (150, container.SetProperty),
                (40, container.Start),
                (30, container.Stop),
#                (15, container.Pause),
#                (15, container.Resume),
                (15, container.Wait),
                (25, container.Kill)
        ] )(conn, select_container(conn))
        return 0
    except (
            porto.exceptions.ContainerDoesNotExist,
            porto.exceptions.PermissionError,
            porto.exceptions.InvalidProperty,
            porto.exceptions.InvalidValue,
            porto.exceptions.InvalidState,
            porto.exceptions.ContainerAlreadyExists,
            porto.exceptions.UnknownError,
            porto.exceptions.ResourceNotAvailable,
            porto.exceptions.Busy,
            porto.exceptions.NotSupported,
           ):
        return 1
    #Otherwise immediately fail if abnormal exception raised (script or lib bound)

def set_verbose(is_verbose):
    container.VERBOSE = is_verbose
    volume.VERBOSE = is_verbose
    layer.VERBOSE = is_verbose

def set_active(is_active):
    container.set_active(is_active)
