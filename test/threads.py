from porto import *
import time
import sys
import os
import signal
import multiprocessing
import traceback

################################################################################

def iss(conn, this_script, top): # top-level executor
    if top:
        monitoring = conn.Create('monitoring')
        monitoring.SetProperty('command', 'python ' + this_script + ' monitoring')
        monitoring.SetProperty('isolate', 'false')
        monitoring.Start()

        cloud = conn.Create('cloud')
        cloud.SetProperty('command', 'python ' + this_script + ' cloud')
        cloud.SetProperty('porto_namespace', 'cloud/')
        cloud.Start()

        iss2 = conn.Create('iss2')
        iss2.SetProperty('command', 'python ' + this_script + ' iss')
        iss2.SetProperty('porto_namespace', 'iss2/')
        iss2.Start()

    jobs = []
    for x in range(0, 20):
        top = conn.Create('search' + str(x))
        top.SetProperty('isolate', 'true')
        c = conn.Create('search' + str(x) + '/hook')
        c.SetProperty('command', 'sleep ' + str(x))
        c.SetProperty('isolate', 'true')
        c.Start()
        jobs.append(top)

    time.sleep(20)

    for x in range(0, 20):
        jobs[x].Stop()
        conn.Destroy('search' + str(x))

    if top:
        conn.Stop('cloud')
        conn.Destroy('cloud')
        conn.Stop('iss2')
        conn.Destroy('iss2')
        conn.Stop('monitoring')
        conn.Destroy('monitoring')

def monitoring(conn, this_script, top):
    conn.Get(conn.List(), ['state', 'cpu_usage', 'memory_usage'])
    time.sleep(1)

def cloud(conn, this_script, top):
    jobs = []
    for x in range(0, 10):
        c = conn.Create('job' + str(x))
        c.SetProperty('command', 'sleep 5')
        c.Start()
        jobs.append(c)
        time.sleep(1)

    for x in range(0, 10):
        jobs[x].Stop()
        conn.Destroy('job' + str(x))
        time.sleep(1)

################################################################################

def run_thread(thread, this_script, top):
    try:
        conn = Connection()
        conn.connect()

        while True:
            print(thread)
            globals()[thread](conn, this_script, top)
    except:
        print('Error in ' + thread)
        sys.exit(-1)

def init_worker():
    signal.signal(signal.SIGINT, signal.SIG_IGN)

if __name__ == '__main__':
    try:
        if len(sys.argv) > 1:
            run_thread(sys.argv[1], sys.argv[0], False)
        else:
            run_thread('iss', os.getcwd() + '/' + sys.argv[0], True)
        sys.exit(0)
    except KeyboardInterrupt:
        print 'Terminated by user'
        pool.terminate()
        pool.join()
        sys.exit(-1)
