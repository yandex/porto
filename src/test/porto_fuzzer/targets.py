#!/usr/bin/python

import os
import porto
import container

from common import *

import random
from random import randint
from random import random as randf

def existing_container(conn):
    existing = conn.List()
    if len(existing) == 1:
        return get_random_str(NAME_LIMIT)
    return existing[randint(1, len(existing) - 1)]

def select_container(conn):
    return select_by_weight( [
        (1, select_by_weight( [
            (1, "/"),
            (1, "/porto")
            ] )
        ),
        (45, existing_container(conn)),
        (40, select_by_weight( [
                (60, get_random_str(NAME_LIMIT)),
                (40, existing_container(conn) + "/" + get_random_str(NAME_LIMIT))
            ] )
        )
    ] )

def container_action(conn):
    try:
        select_by_weight(
                [
                (75, container.Create),
                (20, container.Destroy),
                (150, container.SetProperty),
                (40, container.Start),
                (30, container.Stop),
                (15, container.Pause),
                (15, container.Resume),
                (15, container.Wait),
                (25, container.Kill)
                ]

        )(conn, select_container(conn))
    except (
            porto.exceptions.ContainerDoesNotExist,
            porto.exceptions.PermissionError,
            porto.exceptions.InvalidValue,
            porto.exceptions.InvalidState,
            porto.exceptions.ContainerAlreadyExists,
            porto.exceptions.UnknownError,
            porto.exceptions.ResourceNotAvailable,
            porto.exceptions.Busy
           ):
        return 1
    #Otherwise immediately fail if abnormal exception raised (script or lib bound)

    return 0
