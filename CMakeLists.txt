cmake_minimum_required(VERSION 2.8)

set(CMAKE_CXX_FLAGS "-O2 -Wall -std=c++11 -g")
set(CMAKE_EXE_LINKER_FLAGS "-static-libstdc++ -static-libgcc")

find_library(PB NAMES libprotobuf.a)

find_package(Protobuf REQUIRED)
include_directories(${PROTOBUF_INCLUDE_DIRS})

include_directories(${CMAKE_CURRENT_BINARY_DIR})
PROTOBUF_GENERATE_CPP(PROTO_SRCS PROTO_HDRS rpc.proto kv.proto)

add_executable(portod portod.cpp cgroup.cpp mount.cpp rpc.cpp protobuf.cpp container.cpp task.cpp file.cpp stringutil.cpp kvalue.cpp subsystem.cpp folder.cpp error.cpp property.cpp
		       ${PROTO_SRCS} ${PROTO_HDRS})
target_link_libraries(portod ${PB} pthread)

add_executable(porto porto.cpp protobuf.cpp error.cpp selftest.cpp libporto.cpp ${PROTO_SRCS} ${PROTO_HDRS})
target_link_libraries(porto ${PB} pthread)
