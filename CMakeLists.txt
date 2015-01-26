cmake_minimum_required(VERSION 2.8)

PROJECT(yandex-porto)

set(CMAKE_INSTALL_PREFIX /usr)
set(CMAKE_CXX_FLAGS "-O2 -Wall -std=c++11 -g")
set(CMAKE_EXE_LINKER_FLAGS "-static-libstdc++ -static-libgcc")
#set(CMAKE_CXX_FLAGS "-O0 -Wall -std=c++11 -g -fno-omit-frame-pointer -fprofile-arcs -ftest-coverage")
#set(CMAKE_EXE_LINKER_FLAGS "-static-libstdc++ -static-libgcc -fprofile-arcs -ftest-coverage")

if(EXISTS "/usr/bin/g++-4.7")
    set(CMAKE_CXX_COMPILER "g++-4.7")
endif()

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/api/cpp)

include(ExternalProject)
ExternalProject_Add(libnl
	SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/contrib/libnl
	UPDATE_COMMAND autoreconf -fi
	CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=<INSTALL_DIR> --disable-cli --disable-pthreads
	BUILD_COMMAND ${MAKE})

ExternalProject_Get_Property(libnl INSTALL_DIR)
include_directories(${INSTALL_DIR}/include/libnl3)

find_library(PB NAMES libprotobuf.a)
find_package(Protobuf REQUIRED)
include_directories(${PROTOBUF_INCLUDE_DIRS})

include_directories(${CMAKE_CURRENT_BINARY_DIR})
PROTOBUF_GENERATE_CPP(PROTO_SRCS PROTO_HDRS rpc.proto kv.proto config.proto)
add_custom_command(
	OUTPUT "api/python/porto/rpc_pb2.py"
	COMMAND protoc ARGS --python_out=${CMAKE_CURRENT_SOURCE_DIR}/api/python/porto/ --proto_path=${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/rpc.proto
	DEPENDS rpc.proto
	VERBATIM)

add_custom_target(version.hpp ./GIT-VERSION-GEN WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} VERBATIM)

#set_source_files_properties(tags PROPERTIES GENERATED true)
#add_custom_target(tags COMMAND ctags -R --c++-kinds=+p --fields=+iaS --extra=+q . WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
#set_source_files_properties(TAGS PROPERTIES GENERATED true)
#add_custom_target(TAGS COMMAND ctags -R -e --c++-kinds=+p --fields=+iaS --extra=+q . WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

link_directories(${yandex-porto_BINARY_DIR})
include_directories(${yandex-porto_BINARY_DIR}/api/cpp)
add_library(porto api/cpp/libporto.cpp error.cpp util/protobuf.cpp ${PROTO_SRCS} ${PROTO_HDRS})
add_dependencies(porto version.hpp)

add_executable(portod portod.cpp cgroup.cpp rpc.cpp container.cpp holder.cpp event.cpp task.cpp kvalue.cpp subsystem.cpp config.cpp value.cpp data.cpp property.cpp qdisc.cpp context.cpp util/idmap.cpp util/namespace.cpp util/netlink.cpp util/log.cpp util/mount.cpp util/path.cpp util/file.cpp util/folder.cpp util/unix.cpp util/cred.cpp util/string.cpp util/crash.cpp util/crc32.cpp)
set_target_properties(portod PROPERTIES COMPILE_DEFINITIONS "PORTOD=1")
add_dependencies(portod version.hpp)
target_link_libraries(portod porto ${PB} ${INSTALL_DIR}/lib/libnl-3.a ${INSTALL_DIR}/lib/libnl-route-3.a pthread rt)

add_executable(portoctl api/python/porto/rpc_pb2.py portoctl.cpp cli.cpp config.cpp util/namespace.cpp util/log.cpp util/mount.cpp util/path.cpp util/file.cpp util/folder.cpp util/unix.cpp util/cred.cpp util/string.cpp)
add_dependencies(portoctl version.hpp)
target_link_libraries(portoctl porto ${PB} rt)

add_executable(portotest portotest.cpp config.cpp qdisc.cpp test/selftest.cpp test/stresstest.cpp test/test.cpp config.cpp util/mount.cpp util/netlink.cpp util/log.cpp util/path.cpp util/file.cpp util/folder.cpp util/unix.cpp util/cred.cpp util/string.cpp)
add_dependencies(portotest version.hpp)
target_link_libraries(portotest porto ${PB} ${INSTALL_DIR}/lib/libnl-3.a ${INSTALL_DIR}/lib/libnl-route-3.a pthread rt)


add_executable(portoinit init/portoinit.cpp init/init.pb.cc error.cpp rpc.pb.cc util/string.cpp util/protobuf.cpp util/unix.cpp util/file.cpp util/folder.cpp util/path.cpp util/log.cpp util/cred.cpp config.cpp config.pb.cc api/cpp/libporto.cpp util/unix.hpp
    ${PROTO_SRCS} ${PROTO_HDRS})
target_link_libraries(portoinit ${PB} pthread rt)

install(
    TARGETS portod portoctl portotest
    RUNTIME DESTINATION sbin
    LIBRARY DESTINATION lib
    )
