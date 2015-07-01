from porto import *
import time
import sys
import signal
import multiprocessing
import traceback

################################################################################

def thread1(conn): # listener (monitoring, portoctl list, ...)
#    print(conn.List())
    print(conn.Get(conn.List(), ['state', 'cpu_usage', 'memory_usage']))
    time.sleep(1)

def thread2(conn): # top-level executor
    name = 'iss'
    sub1 = 'iss/qloud'
    sub2 = 'iss/base'

    try:
        conn.Destroy(sub2)
        conn.Destroy(sub1)
        conn.Destroy(name)
    except exceptions.ContainerDoesNotExist:
        pass

    print('create ' + name)
    c = conn.Create(name)
    c.SetProperty('command', 'sleep 1000000')
    c.Start()

    print('create ' + sub1)
    c2 = conn.Create(sub1)
    c2.SetProperty('command', 'sleep 1000000')
    c2.Start()

    print('create ' + sub2)
    c3 = conn.Create(sub2)
    c3.SetProperty('command', 'sleep 1')
    c3.Start()

    time.sleep(5)

    print('stop ' + name)
    c.Stop()

    conn.Destroy(sub2)
    conn.Destroy(sub1)
    conn.Destroy(name)

def thread3(conn): # sub-executor
    time.sleep(1)

def thread4(conn): # sibling executor
    time.sleep(1)

tests = [thread1, thread2, thread3, thread4]

################################################################################

def run_thread(thread):
    conn = Connection()
    conn.connect()

    while True:
        thread(conn)

    conn.disconnect()

def init_worker():
    signal.signal(signal.SIGINT, signal.SIG_IGN)

if __name__ == '__main__':
    pool = multiprocessing.Pool(len(tests), init_worker)
    result = pool.map_async(run_thread, tests)

    try:
        result.get(60)
        print "Test failed"
        sys.exit(-1)
    except KeyboardInterrupt:
        print "Terminated by user"
        pool.terminate()
        pool.join()
        sys.exit(-1)
    except multiprocessing.TimeoutError:
        pool.terminate()
        pool.join()
        print "Test passed"
        sys.exit(0)
