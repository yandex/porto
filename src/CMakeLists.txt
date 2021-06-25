project(porto)

include_directories(${CURSES_INCLUDE_DIR})

PROTOBUF_GENERATE_CPP(RPC_PROTO_SRCS RPC_PROTO_HDRS rpc.proto)
add_library(rpc_proto STATIC ${RPC_PROTO_SRCS})
set_source_files_properties(${RPC_PROTO_SRCS} PROPERTIES COMPILE_FLAGS -Wno-unused-parameter)

PROTOBUF_GENERATE_CPP(KV_PROTO_SRCS KV_PROTO_HDRS kv.proto)
add_library(kv_proto STATIC ${KV_PROTO_SRCS})
set_source_files_properties(${KV_PROTO_SRCS} PROPERTIES COMPILE_FLAGS -Wno-unused-parameter)

PROTOBUF_GENERATE_CPP(CONFIG_PROTO_SRCS CONFIG_PROTO_HDRS config.proto)
add_library(config STATIC ${CONFIG_PROTO_SRCS} config.cpp)
add_dependencies(config rpc_proto) # rpc.pp.h -> error.hpp -> config.hpp
set_source_files_properties(${CONFIG_PROTO_SRCS} PROPERTIES COMPILE_FLAGS -Wno-unused-parameter)

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
add_subdirectory(fmt)

include_directories(${libporto_SOURCE_DIR})

add_executable(portod portod.cpp core.cpp cgroup.cpp rpc.cpp container.cpp
		      event.cpp task.cpp env.cpp device.cpp network.cpp
		      filesystem.cpp volume.cpp storage.cpp
		      kvalue.cpp config.cpp property.cpp
		      epoll.cpp client.cpp stream.cpp helpers.cpp waiter.cpp)
target_link_libraries(portod version porto util config
			     rpc_proto kv_proto
			     pthread rt fmt ${PB} ${LIBNL} ${LIBNL_ROUTE} ${LIBNL_IDIAG})

add_executable(portoctl portoctl.cpp cli.cpp)
target_link_libraries(portoctl version porto util pthread rt fmt ${PB})

add_executable(portoctl-top portotop.cpp)
target_link_libraries(portoctl-top version porto util pthread
			       rt menu fmt ${PB} ${CURSES_LIBRARIES})

add_executable(portoinit portoinit.c)
target_link_libraries(portoinit version)
set_target_properties(portoinit PROPERTIES LINK_FLAGS "-static")

install(
	TARGETS portod portoctl
	RUNTIME DESTINATION sbin
	LIBRARY DESTINATION lib
)

install(
	TARGETS portoinit portoctl-top
	RUNTIME DESTINATION lib/porto
)

add_custom_command(
	OUTPUT porto.8
	DEPENDS ${CMAKE_SOURCE_DIR}/porto.md
	COMMAND pandoc ${CMAKE_SOURCE_DIR}/porto.md -s -t man > ${CMAKE_CURRENT_BINARY_DIR}/porto.8
	VERBATIM)

add_custom_target(man ALL
	DEPENDS porto.8)

install(
	FILES ${CMAKE_CURRENT_BINARY_DIR}/porto.8
	DESTINATION share/man/man8
)
