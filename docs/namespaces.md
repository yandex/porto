# Container names, hierarchies and namespaces #
Container is identified by it's name: e.g. "my\_service".
Generally, a user should care about uniqueness of container names.

Containers can build hierarchies: for instance, "my\_service" is a parent container,
and "my\_service/monitoring" is a child container. Porto doesn't limit the depth of hierarchy
(but the underlying Linux kernel does).
Hierarchies are used to set group limits (like limit maximum available memory for two containers)
and to manage a state of a group of containers simultaneously (stop the whole group).

Container names can be absolute and relative to a container namespace. What does it mean?
When a user asks Porto to create container "my\_service", Porto will care about who asks. If a user
(more strictly, a user process) works on host, Porto will create "my\_service". Otherwise, Porto
will add a namespace of user's container to the specified name. In other words, container namespace
is a prefix, that limits the visibility of Porto containers hierarchy.
Container namespaces allow to limit visibility as well as by sub-hierarchy, so as by custom prefix.

For example, a user from container "production" with namespace "production/" creates container "my\_service".
The absolute name of this container is "production/my_service". So, it becomes a child of "production" container.
Another example, assume the parent container has "production-" namespace. In this case, the child container will
have name "production-my\_service" and will be not nested into the "production" container.
If a user asks Porto about list of existing containers, Porto returns list of relative names.

So, Porto namespaces provides an ability to run a software that manages Porto containers in a Porto container
without any modification. Also, namespaces can be described as an ability to run Porto under Porto.

There is a special "." container, that points (it's not a container, it's like symbolic link) to the top container,
that is accessible from the client's namespace.
It has to be used to get the amount of available resources instead of "/" container, if your container works in isolated namespace.

Any container has absolute_name data. It may be used to pass container names between namespaces.
