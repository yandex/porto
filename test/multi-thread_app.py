import threading
import time

def spawn_threads():
    class DummyThread(threading.Thread):
        def run(self):
            time.sleep(300)

    for i in range(10):
        th = DummyThread()
        th.daemon = True
        th.start()

spawn_threads()
time.sleep(30)
