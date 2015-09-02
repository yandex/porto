cmake_minimum_required(VERSION 2.8)

PROJECT(yandex-porto)

OPTION(ENABLE_TEST "Enables building portotest utility" ON)
OPTION(ENABLE_PYTHON "Enables building python API" ON)
OPTION(USE_SYSTEM_LIBNL "Use system libnl" OFF)
OPTION(ENABLE_ASAN "Enables address sanitizer" OFF)

if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE MATCHES None)
	set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS "-Wall -std=c++11 -g")
set(CMAKE_EXE_LINKER_FLAGS "-static-libstdc++ -static-libgcc")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -fno-omit-frame-pointer --coverage")
set(CMAKE_EXE_LINKER_FLAGS_DEBUG "--coverage")
set(CMAKE_CXX_FLAGS_RELEASE "-O2")
set(CMAKE_INSTALL_PREFIX /usr)

if(ENABLE_ASAN)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fno-omit-frame-pointer -fno-common")
	set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libasan")
endif()

if(EXISTS "/usr/bin/g++-4.7")
	set(CMAKE_CXX_COMPILER "g++-4.7")
endif()

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/api/cpp)

if(USE_SYSTEM_LIBNL)
	find_library(LIBNL NAMES libnl-3.a libnl-3 nl-3)
	find_library(LIBNL_ROUTE NAMES libnl-route-3.a libnl-route-3 nl-route-3)
	find_path(LIBNL_INCLUDE_DIR NAMES netlink/netlink.h PATH_SUFFIXES libnl3)
	include_directories(${LIBNL_INCLUDE_DIR})
else()
	include(ExternalProject)
	ExternalProject_Add(libnl
		BUILD_COMMAND $(MAKE)
		INSTALL_COMMAND $(MAKE) install
		SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/contrib/libnl
		UPDATE_COMMAND autoreconf -fi
		CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=<INSTALL_DIR> --disable-cli --disable-pthreads
		BUILD_COMMAND ${MAKE})
	ExternalProject_Get_Property(libnl INSTALL_DIR)
	include_directories(${INSTALL_DIR}/include/libnl3)
	SET(LIBNL ${INSTALL_DIR}/lib/libnl-3.a)
	SET(LIBNL_ROUTE ${INSTALL_DIR}/lib/libnl-route-3.a)
endif()

find_library(PB NAMES libprotobuf.a)
find_package(Protobuf REQUIRED)
include_directories(${PROTOBUF_INCLUDE_DIRS})

include_directories(${CMAKE_CURRENT_BINARY_DIR})
PROTOBUF_GENERATE_CPP(PROTO_SRCS PROTO_HDRS rpc.proto kv.proto config.proto)

find_package(Curses REQUIRED)
include_directories(${CURSES_INCLUDE_DIR})

set(PYTHON_SOURCES "")
if(ENABLE_PYTHON)
	add_custom_command(
		OUTPUT "api/python/porto/rpc_pb2.py"
		COMMAND protoc ARGS --python_out=${CMAKE_CURRENT_SOURCE_DIR}/api/python/porto/ --proto_path=${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/rpc.proto
		DEPENDS rpc.proto
		VERBATIM)
	set(PYTHON_SOURCES "api/python/porto/rpc_pb2.py")
endif()

add_custom_target(version.hpp ./GIT-VERSION-GEN WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} VERBATIM)

set_source_files_properties(tags PROPERTIES GENERATED true)
add_custom_target(tags COMMAND ctags -R --c++-kinds=+p --fields=+iaS --extra=+q . WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_source_files_properties(TAGS PROPERTIES GENERATED true)
add_custom_target(TAGS COMMAND ctags -R -e --c++-kinds=+p --fields=+iaS --extra=+q . WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_library(util STATIC util/error.cpp util/locks.cpp util/idmap.cpp util/namespace.cpp util/netlink.cpp util/log.cpp util/mount.cpp util/path.cpp util/file.cpp util/folder.cpp util/signal.cpp util/unix.cpp util/cred.cpp util/string.cpp util/crash.cpp util/crc32.cpp util/sha256.cpp util/ext4_proj_quota.c util/protobuf.cpp ${PROTO_SRCS})
if(NOT USE_SYSTEM_LIBNL)
add_dependencies(util libnl)
endif()

link_directories(${yandex-porto_BINARY_DIR})
include_directories(${yandex-porto_BINARY_DIR}/api/cpp)
add_library(porto STATIC util api/cpp/libporto.cpp)
add_dependencies(porto util version.hpp)

add_executable(portod portod.cpp cgroup.cpp rpc.cpp container.cpp holder.cpp event.cpp task.cpp kvalue.cpp subsystem.cpp config.cpp container_value.cpp value.cpp data.cpp property.cpp qdisc.cpp context.cpp volume.cpp epoll.cpp client.cpp)
add_dependencies(portod version.hpp)
target_link_libraries(portod porto util ${PB} ${LIBNL} ${LIBNL_ROUTE} pthread rt)

add_executable(portoctl ${PYTHON_SOURCES} portoctl.cpp cli.cpp config.cpp portotop.cpp)
add_dependencies(portoctl version.hpp)
target_link_libraries(portoctl porto util ${PB} rt ${CURSES_LIBRARIES} ${LIBNL} ${LIBNL_ROUTE})

add_executable(portod-meta-root meta_root.cpp util/signal.cpp)
add_dependencies(portod-meta-root version.hpp)
if(NOT ENABLE_ASAN)
	set_target_properties(portod-meta-root PROPERTIES LINK_FLAGS "-static")
endif()

set(TEST_TARGETS "")
if(ENABLE_TEST)
	add_executable(portotest portotest.cpp config.cpp qdisc.cpp test/selftest.cpp test/stresstest.cpp test/fuzzytest.cpp test/test.cpp config.cpp)
	add_dependencies(portotest version.hpp)
	target_link_libraries(portotest porto util ${PB} ${LIBNL} ${LIBNL_ROUTE} pthread rt)
	set(TEST_TARGETS "portotest")
endif()

install(
    TARGETS portod portoctl ${TEST_TARGETS} portod-meta-root
    RUNTIME DESTINATION sbin
    LIBRARY DESTINATION lib
)
