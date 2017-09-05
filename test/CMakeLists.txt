project(tests)

include_directories(${util_SOURCE_DIR})
include_directories(${libporto_SOURCE_DIR})
include_directories(${porto_SOURCE_DIR})
include_directories(${porto_BINARY_DIR})

add_executable(portotest portotest.cpp test.cpp selftest.cpp stresstest.cpp
	       ${porto_SOURCE_DIR}/protobuf.cpp)

target_link_libraries(portotest version porto util config
				pthread rt fmt ${PB} ${LIBNL} ${LIBNL_ROUTE})

add_executable(mem_touch mem_touch.c)

# head

add_test(NAME cleanup_logs
         COMMAND rm -f /var/log/portod.log
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME portod_start
         COMMAND ${CMAKE_BINARY_DIR}/portod --discard --verbose restart
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# interface tests

add_test(NAME python_api
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-api.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

if(EXISTS /usr/bin/go AND EXISTS /usr/share/gocode/src/github.com/golang/protobuf)
add_test(NAME go_api
         COMMAND go test -v api/go/porto
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(go_api
                     PROPERTIES
                     ENVIRONMENT GOPATH=${CMAKE_SOURCE_DIR}:/usr/share/gocode)
endif()

add_test(NAME knobs
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-knobs.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME portoctl-exec
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-portoctl-exec.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME portoctl-wait
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-portoctl-wait.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME self-container
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-self-container.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME portoctl-attach
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-portoctl-attach.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# limits test

add_test(NAME mem-overcommit
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-mem-overcommit.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME mem_limit_total
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-mem_limit_total.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME mem_recharge
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-mem-recharge.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME dirty_limit
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-dirty-limit.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME cpu_limit
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-cpu_limit.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# functional tests

add_test(NAME uid_handling
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-uid_handling.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME unpriv-cred
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-unpriv-cred.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME security
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-security.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME tc-rebuild
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-tc-rebuild.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME prev_release_upgrade
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-prev_release_upgrade.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME portotest
         COMMAND ${CMAKE_BINARY_DIR}/portotest --except recovery wait_recovery volume_recovery leaks net_property
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME networking
         COMMAND ${CMAKE_BINARY_DIR}/portotest net_property
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME locate-process
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-locate-process.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME oom_non_fatal
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-oom_non_fatal.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# slow tests

add_test(NAME mem_limit
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-mem_limit.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME volume_backends
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-volume_backends.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME volume_places
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-volume_places.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME portod_cli
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-portod_cli.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME recovery
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-recovery.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME leaks
         COMMAND ${CMAKE_BINARY_DIR}/portotest leaks
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME perf
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-performance.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME fuzzer
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/porto_fuzzer/porto_fuzzer.py
         --timeout 300 --progress 32 2000
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# tail

add_test(NAME stats
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-stats.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME portod_stop
         COMMAND ${CMAKE_BINARY_DIR}/portod stop
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# end

set_tests_properties(python_api recovery security volume_backends volume_places
                     mem-overcommit mem_limit_total self-container tc-rebuild
                     unpriv-cred prev_release_upgrade uid_handling knobs
                     cpu_limit mem_limit fuzzer stats mem_recharge dirty_limit
                     locate-process oom_non_fatal perf
                     PROPERTIES
                     ENVIRONMENT PYTHONPATH=${CMAKE_SOURCE_DIR}/src/api/python)
