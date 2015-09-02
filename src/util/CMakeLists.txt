add_library(util STATIC error.cpp locks.cpp idmap.cpp namespace.cpp netlink.cpp log.cpp mount.cpp path.cpp file.cpp folder.cpp signal.cpp unix.cpp cred.cpp string.cpp crash.cpp crc32.cpp sha256.cpp ext4_proj_quota.c protobuf.cpp)
add_dependencies(util protobufs)

if(NOT USE_SYSTEM_LIBNL)
add_dependencies(util libnl)
endif()
