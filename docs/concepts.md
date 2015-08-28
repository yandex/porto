* Porto deals with only few concepts: containers, volumes and layers, theirs data and properties.
We try to build simple interface.
* Porto hides implementation details and kernel abstractions (whenever possible and necessary).
We use Porto in huge production systems. Porto helps us to throw all "unix magic" out of infrastructure
services like monitoring and cluster management. Also, it helps us to hide differences between different
kernel versions.
* Porto is local.
That means that Porto doesn't contain (and never will) any non-local parts like images repositories,
cluster orchestration etc. We limit the scope of Porto project to achieve better quality.
* Porto doesn't store persistent state.
All containers and volumes are destroyed on reboot. Persistency can be implemented on top of Porto
by other tools.
Persistency is not very useful in clouds. When a server wakes up after reboot, it's never known,
if it should run the same software as before. It's a task of cluster management systems, not Porto.
* Porto is reliable.
Porto strives to be reliable and don't loose containers, their states, exit codes, etc during Porto updates and even
failures (whenever it's possible, of course).
* Portoctl for manual operations, API for building infrastructure.
Porto provides portoctl CLI tool, Python API and protobuf-based API. portoctl is intended to be convenient for
manual operations, but theirs behavior can change. API is more reliable and should be used to build
software on top of Porto.
* Porto supports various degrees of isolation (from containers running completely in host namespace to highly isolated containers).
We care about easy integration into existing infrastructure. You can start using Porto very easy and
without any performance penalty. Later you can step by step add isolation, when necessary.
