# Container states and life cycle #

Container can be in one of the following states:
* stopped: container isn't running and doesn't use any system resources
* running: container is running
* paused: container is paused (it isn't running, but uses system resources)
* dead: container isn't running, but it has exit code and still use some system resources
* meta: it's a special running state for containers with unspecified command

The normal life cycle of container can looks as follows:
(create) stopped -> (start) running -> (pause) paused -> (resume) running ->
(main process quit) dead -> (stop) stopped

# Container data and properties #

There are two types of container knobs:
* properties - setup container environment and limits
* data - readonly statistics and other container status information

Properties are generally modified in stopped state, while data is read mostly
from running/dead containers.
