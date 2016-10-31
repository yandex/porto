project(tests)

include_directories(${util_SOURCE_DIR})
include_directories(${libporto_SOURCE_DIR})
include_directories(${porto_SOURCE_DIR})
include_directories(${porto_BINARY_DIR})

add_executable(portotest portotest.cpp test.cpp selftest.cpp stresstest.cpp
	       ${porto_SOURCE_DIR}/protobuf.cpp)
target_link_libraries(portotest version porto util config
				pthread rt ${PB} ${LIBNL} ${LIBNL_ROUTE})

add_test(NAME portod_start
         COMMAND ${CMAKE_BINARY_DIR}/portod --discard --verbose restart
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME selftest
         COMMAND ${CMAKE_BINARY_DIR}/portotest --except recovery wait_recovery volume_recovery
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME python_api
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-api.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME recovery
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-recovery.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME security
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-security.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME volume_backends
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-volume_backends.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME volume_places
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-volume_places.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME mem-overcommit
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-mem-overcommit.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME mem_limit_total
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-mem_limit_total.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME portoctl-attach
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-portoctl-attach.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME portoctl-exec
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-portoctl-exec.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME portoctl-wait
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-portoctl-wait.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME portod_cli
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-portod_cli.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME self-container
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-self-container.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME tc-rebuild
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-tc-rebuild.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME unpriv-cred
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/test-unpriv-cred.py
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME fuzzer
         COMMAND python -u ${CMAKE_SOURCE_DIR}/test/porto_fuzzer/porto_fuzzer.py
         --timeout 300 --progress 32 2000
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME portod_stop
         COMMAND ${CMAKE_BINARY_DIR}/portod stop
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

set_tests_properties(python_api recovery security volume_backends volume_places
                     mem-overcommit mem_limit_total self-container tc-rebuild
                     unpriv-cred fuzzer
                     PROPERTIES
                     ENVIRONMENT PYTHONPATH=${CMAKE_SOURCE_DIR}/src/api/python)
