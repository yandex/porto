project(tests)

include_directories(${util_SOURCE_DIR})
include_directories(${libporto_SOURCE_DIR})
include_directories(${porto_SOURCE_DIR})
include_directories(${porto_BINARY_DIR})

add_executable(portotest portotest.cpp test.cpp selftest.cpp stresstest.cpp fuzzytest.cpp
	       ${porto_SOURCE_DIR}/protobuf.cpp)
target_link_libraries(portotest version porto util config
				pthread rt ${PB} ${LIBNL} ${LIBNL_ROUTE})

add_test(NAME selftest COMMAND ${CMAKE_SOURCE_DIR}/scripts/standalone_test
	 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
