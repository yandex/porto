cmake_minimum_required(VERSION 2.8)

set(CMAKE_CXX_FLAGS "-O2 -Wall -std=c++11")

find_package(Protobuf REQUIRED)
include_directories(${PROTOBUF_INCLUDE_DIRS})

include_directories(${CMAKE_CURRENT_BINARY_DIR})
PROTOBUF_GENERATE_CPP(PROTO_SRCS PROTO_HDRS rpc.proto)

add_executable(portod portod.cpp cgroup.cpp mount.cpp rpc.cpp container.cpp
		       ${PROTO_SRCS} ${PROTO_HDRS})
target_link_libraries(portod ${PROTOBUF_LIBRARIES})
