There are five types of resource limits in Porto: Process, Memory, CPU, IO and network limits.
All limits can be set in stopped and running states (if not specified otherwise).

# Process

* ulimit - container resource limits, syntax: <type> <soft> <hard>; ... (man 2 getrlimit); use unlim/unlimited to indicate RLIM\_INFINITY

# Memory

* memory\_guarantee (bytes, default 0)

  Amount of memory (anon + page cache) guaranteed to container. Guarantee means
  that allocated memory will not be evicted until memory usage is less than guarantee.
  Porto does not allow to overcommit memory_guarantee and reserves 2Gb for host system.

* memory\_limit (bytes, default 0)

  _When changing this property for running container, new value can not be less than memory\_usage._

  Memory usage hard limit (anon + page cache).
  Memory allocation over limit will be rejected (returns ENOMEM). Page fault
  will case OOM killer invocation.

* dirty\_limit (bytes, default 0)

  Hard limit for dirty memory (unwritten to disk).

# CPU

* cpu\_guarantee ([0, 100]%, default 0%)

  _Does not affect containers with cpu_policy=rt._

  Percentage of CPU time of host, which is available to container when all containers are CPU bound.
  When overcommited, guarantee is distributed proportionally.
  Should be used only with cpu_policy=normal. RT task have priority over guarantee.

* cpu\_limit ([1, 100]%, default 100%)

  _Does not affect containers with cpu_policy=rt._

  Maximum percentage of CPU time available for container.
  You may specify limit in terms of CPU cores using c suffix.

* cpu\_policy ([normal, rt], default normal)

  RT containers have priority over normal (SCHED_RR is used for such tasks).

# IO

* io\_limit (bytes/s, default 0)

  IO limit for container (read/write);

* io\_policy ([batch, normal], default normal)

  - normal - interactive tasks;
  - batch - background batch tasks (currently implemented as fixed blkio limit);

# Net

* net\_guarantee (bytes/s, default 1)

  _Currently, this property may be changed only in stopped state._

  Guaranteed TX network bandwidth.
  When overcommited, guarantee is distributed according to net_priority.

* net\_limit (bytes/s, default inf)

  _Currently, this property may be changed only in stopped state._

  TX bandwidth limit.

* net\_priority ([0, 7], default 3)

  _Currently, this property may be changed only in stopped state._

  Network priority (not IP TOS). Packets with higher priority are sent before packets with lower priority.

* net_tos

  IP TOS (http://en.wikipedia.org/wiki/Type_of_service).
