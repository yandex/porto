#!/usr/bin/python -u

from common import *
import targets

import porto

import random
import time
import os
import sys
import multiprocessing
import argparse
import subprocess
import signal
import functools
import tarfile

def prepare_fuzzer():
    if not os.path.exists(FUZZER_MNT):
        os.mkdir(FUZZER_MNT)

    if os.path.ismount(FUZZER_MNT):
        subprocess.check_call(["umount", "-l", FUZZER_MNT])

    subprocess.check_call(["mount", "-t", "tmpfs", "-o", "size=512M", "None", FUZZER_MNT])

    verify_paths = [VOL_MNT_PLACE, VOL_PLACE, VOL_STORAGE,
                    VOL_PLACE + "/porto_volumes", VOL_PLACE + "/porto_layers"]

    for p in verify_paths:
        if not os.path.exists(p):
            os.mkdir(p)

    open(FUZZER_MNT + "/f1.txt", "w").write("1234567890")
    open(FUZZER_MNT + "/f2.txt", "w").write("0987654321")
    open(FUZZER_MNT + "/f3.txt", "w").write("abcdeABCDE")

    t = tarfile.open(name=TAR1, mode="w")
    t.add(FUZZER_MNT + "/f1.txt", arcname="f1.txt")
    t.add(FUZZER_MNT + "/f2.txt", arcname="f2.txt")
    t.close()

    t = tarfile.open(name=TAR2, mode="w")
    t.add(FUZZER_MNT + "/f1.txt", arcname="f2.txt")
    t.add(FUZZER_MNT + "/f2.txt", arcname="f3.txt")
    t.close()

def cleanup_fuzzer():
    conn = porto.Connection()

    for c in targets.our_containers(conn):
        try:
            conn.Destroy(c)
        except porto.exceptions.ContainerDoesNotExist:
            pass

    for v in targets.our_volumes(conn):
        try:
            conn.UnlinkVolume(v, '***')
        except porto.exceptions.VolumeNotFound:
            pass

    for l in targets.our_layers(conn):
        conn.RemoveLayer(l)

    if (os.path.ismount(FUZZER_MNT)):
        subprocess.check_call(["umount", FUZZER_MNT])

    if (os.path.exists(FUZZER_MNT)):
        os.rmdir(FUZZER_MNT)

def should_stop():
    conn=porto.Connection(timeout=10)
    porto_errors = get_property(conn, "/", "porto_stat[errors]", "0")
    porto_warnings = get_property(conn, "/", "porto_stat[warnings]", "0")
    return porto_errors != "0" or porto_warnings != "0"

def fuzzer_killer(stop, porto_reloads, porto_kills):
    random.seed(time.time() + os.getpid())
    conn=porto.Connection(timeout=10)
    while not stop.is_set():
        target = select_by_weight([
            (90, None),
            (5, True if opts.reload else None),
            (5, False if opts.kill else None),
            ])

        if target is None:
            pid = None
        elif target:
            pid = get_portod_master_pid()
            sig = signal.SIGHUP
            counter = porto_reloads
        else:
            pid = get_portod_pid()
            sig = signal.SIGKILL
            counter = porto_kills

        if pid is not None:
            os.kill(pid, sig)
            with counter.get_lock():
                counter.value += 1

        time.sleep(1)

        if should_stop():
            stop.set();

def fuzzer_thread(please_stop, iter_count, fail_count):
    random.seed(time.time() + os.getpid())
    conn=porto.Connection(timeout=opts.timeout)
    fail_cnt = 0
    iter_cnt = 0;
    while True:
        if iter_cnt % 100 == 0 and please_stop.is_set():
            break
        iter_cnt += 1
        try:
            fail_cnt += select_by_weight([
                (100, targets.container_action),
                (10, targets.volume_action),
                (1, targets.layer_action)
            ])(conn)
        except porto.exceptions.SocketError:
            fail_cnt += 1
    with iter_count.get_lock():
        iter_count.value += iter_cnt
    with fail_count.get_lock():
        fail_count.value += fail_cnt

parser = argparse.ArgumentParser(description="Porto fuzzing utility")
parser.add_argument("--time", default=60, type=int)
parser.add_argument("--threads", default=100, type=int)
parser.add_argument("--timeout", default=180, type=int)
parser.add_argument("--verbose", action="store_true")
parser.add_argument("--active", action="store_true")
parser.add_argument("--no-kill", dest="kill", action="store_false")
parser.add_argument("--no-reload", dest="reload", action="store_false")
parser.add_argument("--no-cleanup", dest="cleanup", action="store_false")
opts = parser.parse_args()

prepare_fuzzer()

skip_log = os.path.getsize("/var/log/portod.log")

targets.set_verbose(opts.verbose)
targets.set_active(opts.active)

procs=[]
inject_test_utils("/tmp")

start_time = time.time()

stop = multiprocessing.Event()
iter_count = multiprocessing.Value('i', 0)
fail_count = multiprocessing.Value('i', 0)
porto_reloads = multiprocessing.Value('i', 0)
porto_kills = multiprocessing.Value('i', 0)

kill_proc = multiprocessing.Process(target=fuzzer_killer, args=(stop, porto_reloads, porto_kills), name="Killer")
kill_proc.start()

for i in range(0, opts.threads):
    proc = multiprocessing.Process(target=fuzzer_thread, args=(stop, iter_count, fail_count))
    proc.start()
    procs += [proc]

stop.wait(opts.time)
stop.set()

kill_proc.join()
for p in procs:
    p.join()

conn = porto.Connection(timeout=30)

print "Running time", time.time() - start_time
print "Iterations", iter_count.value
print "Fails", fail_count.value
print "Reloads", porto_reloads.value
print "Kills", porto_kills.value
print "Errors", get_property(conn, "/", "porto_stat[errors]")
print "Warnings", get_property(conn, "/", "porto_stat[warnings]")
print "Spawned", get_property(conn, "/", "porto_stat[spawned]")
print "RestoreFailed", get_property(conn, "/", "porto_stat[restore_failed]")

failed = get_property(conn, "/", "porto_stat[errors]") != "0" or get_property(conn, "/", "porto_stat[warnings]") != "0"

print ("--- log ---")
os.system("tail -c +{} /var/log/portod.log | grep -wE 'WRN|ERR|STK'".format(skip_log))
print ("--- end ---")

if opts.cleanup:
    cleanup_fuzzer()

if failed:
    sys.exit(1)
