project(porto)

if(USE_SYSTEM_LIBNL)
	find_library(LIBNL NAMES libnl-3.a libnl-3 nl-3)
	find_library(LIBNL_ROUTE NAMES libnl-route-3.a libnl-route-3 nl-route-3)
	find_path(LIBNL_INCLUDE_DIR NAMES netlink/netlink.h PATH_SUFFIXES libnl3)
	include_directories(${LIBNL_INCLUDE_DIR})
else()
	include(ExternalProject)

	if(NOT EXISTS "/usr/bin/autoreconf")
		message(FATAL_ERROR "dh-autoreconf is required to build libnl")
	endif()

	if(NOT EXISTS "/usr/bin/pkg-config")
		message(FATAL_ERROR "pkg-config is required to build libnl")
	endif()

	find_package(BISON REQUIRED)
	find_package(FLEX REQUIRED)

	ExternalProject_Add(libnl
		GIT_REPOSITORY git://github.com/tgraf/libnl.git
		GIT_TAG libnl3_2_25
		BUILD_COMMAND $(MAKE)
		INSTALL_COMMAND $(MAKE) install
		SOURCE_DIR ${CMAKE_SOURCE_DIR}/src/libnl
		UPDATE_COMMAND autoreconf -fi
		CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=<INSTALL_DIR> --disable-cli --disable-pthreads
		BUILD_COMMAND ${MAKE})
	ExternalProject_Get_Property(libnl INSTALL_DIR)
	include_directories(${INSTALL_DIR}/include/libnl3)
	SET(LIBNL ${INSTALL_DIR}/lib/libnl-3.a)
	SET(LIBNL_ROUTE ${INSTALL_DIR}/lib/libnl-route-3.a)
endif()

find_package(Curses REQUIRED)
include_directories(${CURSES_INCLUDE_DIR})

find_library(PB NAMES libprotobuf.a)
find_package(Protobuf REQUIRED)
# FindProtobuf requires only the libraries to be present
if(NOT PROTOBUF_PROTOC_EXECUTABLE)
	message(FATAL_ERROR "Couldn't find protoc executable.")
endif()

PROTOBUF_GENERATE_CPP(RPC_PROTO_SRCS RPC_PROTO_HDRS rpc.proto)
add_library(rpc_proto STATIC ${RPC_PROTO_SRCS})

PROTOBUF_GENERATE_CPP(KV_PROTO_SRCS KV_PROTO_HDRS kv.proto)
add_library(kv_proto STATIC ${KV_PROTO_SRCS})

PROTOBUF_GENERATE_CPP(CONFIG_PROTO_SRCS CONFIG_PROTO_HDRS config.proto)
add_library(config STATIC ${CONFIG_PROTO_SRCS} config.cpp)
add_dependencies(config rpc_proto) # rpc.pp.h -> error.hpp -> config.hpp

PROTOBUF_GENERATE_CPP(PROTO_SRCS PROTO_HDRS rpc.proto kv.proto config.proto)

include_directories(${PROTOBUF_INCLUDE_DIRS})

add_custom_command(
	OUTPUT version.c
	DEPENDS ${CMAKE_SOURCE_DIR}/debian/changelog
		${CMAKE_SOURCE_DIR}/scripts/version
	COMMAND ${CMAKE_SOURCE_DIR}/scripts/version > ${CMAKE_CURRENT_BINARY_DIR}/version.c
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
	VERBATIM)
add_library(version STATIC version.c)

include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

add_subdirectory(api/cpp)
add_subdirectory(util)

include_directories(${libporto_SOURCE_DIR})

add_executable(portod portod.cpp cgroup.cpp rpc.cpp container.cpp holder.cpp
		      event.cpp task.cpp env.cpp device.cpp network.cpp
		      kvalue.cpp config.cpp property.cpp context.cpp
		      volume.cpp epoll.cpp client.cpp stream.cpp protobuf.cpp)
target_link_libraries(portod version porto util config
			     rpc_proto kv_proto
			     pthread rt ${PB} ${LIBNL} ${LIBNL_ROUTE})

add_executable(portoctl portoctl.cpp cli.cpp portotop.cpp)
target_link_libraries(portoctl version porto util
			       rt ${PB} ${CURSES_LIBRARIES})

add_executable(portoinit portoinit.c)
target_link_libraries(portoinit version)
if(NOT ENABLE_ASAN)
	set_target_properties(portoinit PROPERTIES LINK_FLAGS "-static")
endif()

install(
	TARGETS portod portoctl portoinit
	RUNTIME DESTINATION sbin
	LIBRARY DESTINATION lib
)
