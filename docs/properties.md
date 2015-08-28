Container properties define container environment, container data define container state and statistics.

# Properties

* **command** - container start command
* **env** - environment of main container process, syntax: <variable>: <value>; ...
* **group** - gid of container processes (only root can change this property)
* **user** - uid of container processes (only root can change this property)
* **isolate** - whether to use (true) or not (false) PID isolation
* **respawn** - respawn container after it reached dead state (delay between respawns is 1 second)
* **max\_respawns** - how many times container can be respawned (by default is unlimited, -1)
* **private** - this property is not interpreted by Porto and may be used by managing software to keep some private per-container information
* **recharge\_on\_pgfault** - when page fault occurs, current process becomes the owner of page
* **stdout\_path** - path to the file where stdout of container will be redirected (if user redefines this property he is responsible for removal of the file); by default Porto provides some internal file which will be removed when container is stopped
* **stderr\_path** - ditto for stderr
* **stdin\_path** - container stdin path; by default is /dev/null
* **stdout\_limit** - maximum number of bytes Proto will return when reading stdout/stderr data
* **virt\_mode** - virtualization mode:
  - *app* - (default) start process with specified user:group
  - *os* - start process with user and group set to root with limited capabilities (should be used to run lxc/docker containers)
* **aging\_time** - after specified time in seconds dead container is automatically destroyed (24 hours is default)

# Data

## Status information
* **exit\_status** - container exit status (see man 2 wait for format)
* **oom\_killed** - true, if container has been OOM killed
* **parent** - parent container name
* **respawn\_count** - how many times container has been respawned (using respawn property)
* **root\_pid** - container root pid
* **state** - current container state (stopped/running/paused/dead)
* **stderr** - returns container stderr
* **stdout** - returns container stdout
* **time** - container running time in seconds

## Counters
* **cpu\_usage** - CPU time used in nanoseconds
* **io\_read** - bytes read from disk, syntax: <disk>: <number of bytes>; ...
* **io\_write** - ditto for bytes written to disk
* **major\_faults** - number of major page faults occurred in container
* **minor\_faults** - ditto for minor faults
* **memory\_usage** - container memory usage (anon + page cache) in bytes
* **max\_rss** - maximum anon memory usage in bytes

# Examples

```
portoctl run container command='ps auxf' env='a=b; c=d' isolate=false
portoctl wait container
portoctl get exit_status oom_killed state time
portoctl get cpu_usage major_faults memory_usage
portoctl destroy container
```
