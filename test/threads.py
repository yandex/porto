from porto import *
import time
import sys
import os
import signal
import multiprocessing
import traceback
import random

################################################################################

def check_running(conn, container):
    state = conn.GetData(container, 'state')
    if state != 'running':
        raise exceptions.InvalidState(container + ' in state ' + state)

def top_iss(conn, this_script):  # top-level executor
    print('start monitoring...')
    monitoring = conn.Create('monitoring')
    monitoring.SetProperty('command', 'python ' + this_script + ' monitoring')
    monitoring.SetProperty('cwd', os.getcwd())
    monitoring.SetProperty('isolate', 'false')
    monitoring.Start()

    print('start cloud')
    cloud = conn.Create('cloud')
    cloud.SetProperty('command', 'python ' + this_script + ' cloud')
    cloud.SetProperty('cwd', os.getcwd())
    cloud.SetProperty('porto_namespace', 'cloud/')
    cloud.Start()

    print('start iss2')
    iss2 = conn.Create('iss2')
    iss2.SetProperty('command', 'python ' + this_script + ' iss')
    iss2.SetProperty('cwd', os.getcwd())
    iss2.SetProperty('porto_namespace', 'iss2/')
    iss2.Start()

    iss(conn, this_script)

    time.sleep(random.randint(10, 30))

    check_running(conn, 'cloud')
    print('stop and destroy cloud')
    conn.Stop('cloud')
    conn.Destroy('cloud')

    check_running(conn, 'iss2')
    print('stop and destroy iss2')
    conn.Stop('iss2')
    conn.Destroy('iss2')

    check_running(conn, 'monitoring')
    print('stop and destroy monitoring')
    conn.Stop('monitoring')
    conn.Destroy('monitoring')

def iss(conn, this_script): #sub-executor
    jobs = []
    for x in range(0, 20):
        print('start search' + str(x))
        top = conn.Create('search' + str(x))
        top.SetProperty('isolate', 'true')
        c = conn.Create('search' + str(x) + '/hook')
        c.SetProperty('command', 'sleep ' + str(random.randint(1, 20)))
        c.SetProperty('isolate', 'true')
        c.Start()
        jobs.append(top)

    time.sleep(random.randint(1, 5))

    for x in range(0, 20):
        print('stop search' + str(x))
        jobs[x].Stop()
        print('destroy search' + str(x))
        conn.Destroy('search' + str(x))

def monitoring(conn, this_script):
    print(conn.Get(conn.List(), ['state', 'cpu_usage', 'memory_usage']))
    time.sleep(random.randint(0, 1))

def cloud(conn, this_script):
    jobs = []
    for x in range(0, 10):
        print('create and start job' + str(x))
        c = conn.Create('job' + str(x))
        c.SetProperty('command', 'sleep ' + str(random.randint(3, 10)))
        c.Start()
        jobs.append(c)
        time.sleep(random.randint(0, 1))

    for x in range(0, 10):
        print('stop and destroy job' + str(x))
        jobs[x].Stop()
        conn.Destroy('job' + str(x))
        time.sleep(random.randint(0, 1))

################################################################################

def run_thread(thread, this_script, iterations = 0):
    conn = Connection(socket_path='/run/portod.socket', timeout=30)
    conn.connect()

    if iterations > 0:
        for i in range(0, iterations):
            globals()[thread](conn, this_script)
    else:
        while True:
            globals()[thread](conn, this_script)

def init_worker():
    signal.signal(signal.SIGINT, signal.SIG_IGN)

if __name__ == '__main__':
    try:
        this_script = sys.argv[0]
        if this_script[0] != '/':
            this_script = os.getcwd() + '/' + this_script

        if len(sys.argv) > 1:
            run_thread(sys.argv[1], this_script)
        else:
            run_thread('top_iss', this_script, 10)
        sys.exit(0)
    except KeyboardInterrupt:
        print 'Terminated by user'
        raise
