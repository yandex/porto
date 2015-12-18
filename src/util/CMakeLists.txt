add_library(util STATIC locks.cpp idmap.cpp namespace.cpp netlink.cpp log.cpp mount.cpp path.cpp file.cpp signal.cpp unix.cpp cred.cpp string.cpp crash.cpp crc32.cpp sha256.cpp quota.cpp)
add_dependencies(util protobufs)

if(NOT USE_SYSTEM_LIBNL)
add_dependencies(util libnl)
endif()
