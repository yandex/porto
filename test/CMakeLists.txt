project(tests)

include_directories(${util_SOURCE_DIR})
include_directories(${porto_SOURCE_DIR})
include_directories(${libporto_SOURCE_DIR})
include_directories(${porto_BINARY_DIR})

add_executable(portotest portotest.cpp test.cpp selftest.cpp stresstest.cpp fuzzytest.cpp
	${porto_BINARY_DIR}/version.c ${porto_SOURCE_DIR}/config.cpp ${porto_SOURCE_DIR}/protobuf.cpp)

target_link_libraries(portotest libporto protobufs util ${PB} ${LIBNL} ${LIBNL_ROUTE} pthread rt)

add_test(NAME selftest COMMAND ${CMAKE_SOURCE_DIR}/scripts/standalone_test
	 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
