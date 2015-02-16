cmake_minimum_required(VERSION 2.8)

set(CMAKE_CXX_FLAGS "-O2 -Wall -std=c++11 -g -I..")
set(CMAKE_EXE_LINKER_FLAGS "-static-libstdc++ -static-libgcc")

if(EXISTS "/usr/bin/g++-4.7")
    set(CMAKE_CXX_COMPILER "g++-4.7")
endif()

find_library(PB NAMES libprotobuf.a)
find_package(Protobuf REQUIRED)
include_directories(${PROTOBUF_INCLUDE_DIRS})

include_directories(${CMAKE_CURRENT_BINARY_DIR})
PROTOBUF_GENERATE_CPP(PROTO_SRCS PROTO_HDRS init.proto ../rpc.proto ../config.proto)

add_executable(portoinit portoinit.cpp ../error.cpp ../util/string.cpp ../util/protobuf.cpp ../util/unix.cpp ../util/file.cpp ../util/folder.cpp ../util/path.cpp ../util/log.cpp ../util/cred.cpp ../config.cpp ../api/cpp/libporto.cpp ../util/unix.hpp
    ${PROTO_SRCS} ${PROTO_HDRS})
target_link_libraries(portoinit ${PB} pthread rt)

install(
    TARGETS portoinit
    RUNTIME DESTINATION sbin
    LIBRARY DESTINATION lib
    )
