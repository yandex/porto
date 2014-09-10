cmake_minimum_required(VERSION 2.8)

PROJECT(yandex-porto)

set(CMAKE_CXX_FLAGS "-O2 -Wall -std=c++11 -g")
set(CMAKE_EXE_LINKER_FLAGS "-static-libstdc++ -static-libgcc")
#set(CMAKE_CXX_FLAGS "-O0 -Wall -std=c++11 -g -fprofile-arcs -ftest-coverage")
#set(CMAKE_EXE_LINKER_FLAGS "-static-libstdc++ -static-libgcc -fprofile-arcs -ftest-coverage")

if(EXISTS "/usr/bin/g++-4.7")
    set(CMAKE_CXX_COMPILER "g++-4.7")
endif()

find_library(PB NAMES libprotobuf.a)

find_package(Protobuf REQUIRED)
include_directories(${PROTOBUF_INCLUDE_DIRS})

include_directories(${CMAKE_CURRENT_BINARY_DIR})
PROTOBUF_GENERATE_CPP(PROTO_SRCS PROTO_HDRS rpc.proto kv.proto)

add_custom_target(version.hpp ./GIT-VERSION-GEN WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} VERBATIM)

set_source_files_properties(tags PROPERTIES GENERATED true)
add_custom_command(OUTPUT tags COMMAND ctags -R --c++-kinds=+p --fields=+iaS --extra=+q . WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(portod tags portod.cpp cgroup.cpp rpc.cpp container.cpp task.cpp kvalue.cpp subsystem.cpp error.cpp property.cpp util/log.cpp util/protobuf.cpp util/mount.cpp util/file.cpp util/folder.cpp util/unix.cpp util/string.cpp
    ${PROTO_SRCS} ${PROTO_HDRS})
add_dependencies(portod version.hpp)
target_link_libraries(portod ${PB} pthread)

add_executable(portoctl portoctl.cpp error.cpp selftest.cpp stresstest.cpp test.cpp libporto.cpp util/log.cpp util/protobuf.cpp util/file.cpp util/folder.cpp util/unix.cpp util/string.cpp ${PROTO_SRCS} ${PROTO_HDRS})
add_dependencies(portoctl version.hpp)
target_link_libraries(portoctl ${PB} pthread)

add_executable(portoloop portoloop.cpp util/log.cpp util/unix.cpp)
target_link_libraries(portoloop)
