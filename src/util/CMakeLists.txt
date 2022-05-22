project(util)

set(UTIL_BASE_SRCS error.cpp log.cpp path.cpp signal.cpp unix.cpp string.cpp proc.cpp)
add_library(utilbase STATIC ${UTIL_BASE_SRCS})
add_library(util STATIC ${UTIL_BASE_SRCS} task.cpp cred.cpp netlink.cpp mutex.cpp crc32.cpp md5.cpp namespace.cpp quota.cpp)
add_dependencies(utilbase config rpc_proto)
add_dependencies(util config rpc_proto)

if(NOT USE_SYSTEM_LIBNL)
add_dependencies(util libnl)
endif()
