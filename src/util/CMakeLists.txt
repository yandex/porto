project(util)

add_library(util STATIC error.cpp namespace.cpp netlink.cpp log.cpp path.cpp signal.cpp unix.cpp cred.cpp string.cpp crc32.cpp md5.cpp quota.cpp proc.cpp mutex.cpp)
add_dependencies(util config rpc_proto)

if(NOT USE_SYSTEM_LIBNL)
add_dependencies(util libnl)
endif()
