cmake_minimum_required(VERSION 2.8)

PROJECT(yandex-porto)

set(CMAKE_CXX_FLAGS "-O2 -Wall -std=c++11 -g")
set(CMAKE_EXE_LINKER_FLAGS "-static-libstdc++ -static-libgcc")
#set(CMAKE_CXX_FLAGS "-O0 -Wall -std=c++11 -g -fprofile-arcs -ftest-coverage")
#set(CMAKE_EXE_LINKER_FLAGS "-static-libstdc++ -static-libgcc -fprofile-arcs -ftest-coverage")

if(EXISTS "/usr/bin/g++-4.7")
    set(CMAKE_CXX_COMPILER "g++-4.7")
endif()

find_library(NL NAMES nl-3)
find_library (RTNL nl-route-3)
find_path(NL_INCLUDE_DIR NAMES netlink/netlink.h PATH_SUFFIXES libnl3)
include_directories(${NL_INCLUDE_DIR})

find_library(PB NAMES libprotobuf.a)
find_package(Protobuf REQUIRED)
include_directories(${PROTOBUF_INCLUDE_DIRS})

include_directories(${CMAKE_CURRENT_BINARY_DIR})
PROTOBUF_GENERATE_CPP(PROTO_SRCS PROTO_HDRS rpc.proto kv.proto config.proto)
add_custom_command(
	OUTPUT "api/python/porto/rpc_pb2.py"
	COMMAND protoc ARGS --python_out=api/python/porto/ rpc.proto
	DEPENDS rpc.proto
	VERBATIM)

add_custom_target(version.hpp ./GIT-VERSION-GEN WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} VERBATIM)

set_source_files_properties(tags PROPERTIES GENERATED true)
add_custom_target(tags COMMAND ctags -R --c++-kinds=+p --fields=+iaS --extra=+q . WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_source_files_properties(TAGS PROPERTIES GENERATED true)
add_custom_target(TAGS COMMAND ctags -R -e --c++-kinds=+p --fields=+iaS --extra=+q . WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

link_directories(${yandex-porto_BINARY_DIR})
include_directories(${yandex-porto_BINARY_DIR}/api/cpp)
add_library(porto api/cpp/libporto.cpp error.cpp util/protobuf.cpp ${PROTO_SRCS} ${PROTO_HDRS})
add_dependencies(porto version.hpp)

add_executable(portod portod.cpp cgroup.cpp rpc.cpp container.cpp task.cpp kvalue.cpp subsystem.cpp config.cpp property.cpp qdisc.cpp util/namespace.cpp util/netlink.cpp util/log.cpp util/mount.cpp util/file.cpp util/folder.cpp util/unix.cpp util/pwd.cpp util/string.cpp util/crash.cpp)
add_dependencies(portod version.hpp)
target_link_libraries(portod porto ${PB} ${RTNL} ${NL} pthread rt)

add_executable(portoctl api/python/porto/rpc_pb2.py portoctl.cpp cli.cpp config.cpp util/namespace.cpp util/log.cpp util/mount.cpp util/file.cpp util/folder.cpp util/unix.cpp util/pwd.cpp util/string.cpp)
add_dependencies(portoctl version.hpp)
target_link_libraries(portoctl porto ${PB} rt)

add_executable(portotest portotest.cpp config.cpp test/selftest.cpp test/stresstest.cpp test/test.cpp config.cpp util/netlink.cpp util/log.cpp util/file.cpp util/folder.cpp util/unix.cpp util/pwd.cpp util/string.cpp)
add_dependencies(portotest version.hpp)
target_link_libraries(portotest porto ${PB} ${RTNL} ${NL} pthread rt)
