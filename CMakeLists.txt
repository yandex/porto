cmake_minimum_required(VERSION 2.8)

PROJECT(yandex-porto)

set(CMAKE_CXX_FLAGS "-O2 -Wall -std=c++11 -g")
set(CMAKE_EXE_LINKER_FLAGS "-static-libstdc++ -static-libgcc")
set(CMAKE_CXX_COMPILER "g++-4.7")

find_library(PB NAMES libprotobuf.a)

find_package(Protobuf REQUIRED)
include_directories(${PROTOBUF_INCLUDE_DIRS})

include_directories(${CMAKE_CURRENT_BINARY_DIR})
PROTOBUF_GENERATE_CPP(PROTO_SRCS PROTO_HDRS rpc.proto kv.proto)

add_custom_target(version.hpp
    git log -1 "--format=format:const std::string GIT_REVISION = \"%H\";%n" HEAD > version.hpp
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} VERBATIM)

add_executable(portod portod.cpp cgroup.cpp mount.cpp rpc.cpp protobuf.cpp container.cpp task.cpp file.cpp stringutil.cpp kvalue.cpp subsystem.cpp folder.cpp error.cpp property.cpp
    ${PROTO_SRCS} ${PROTO_HDRS})
add_dependencies(portod version.hpp)
target_link_libraries(portod ${PB} pthread)

add_executable(porto porto.cpp protobuf.cpp error.cpp selftest.cpp libporto.cpp ${PROTO_SRCS} ${PROTO_HDRS})
add_dependencies(porto version.hpp)
target_link_libraries(porto ${PB} pthread)


set(CPACK_SET_DESTDIR true)
set(CPACK_INSTALL_PREFIX "/")

install(
    FILES ${CMAKE_SOURCE_DIR}/portod
    PERMISSIONS OWNER_READ OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
    DESTINATION usr/bin 
)
install(
    FILES ${CMAKE_SOURCE_DIR}/porto
    PERMISSIONS OWNER_READ OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
    DESTINATION usr/bin 
)

set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_FILE_NAME "yandex-porto")
set(CPACK_PACKAGE_VERSION_MAJOR "0")
set(CPACK_PACKAGE_VERSION_MINOR "1")
set(CPACK_PACKAGE_VERSION_PATCH "0")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_VERSION ${CPACK_PACKAGE_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Component of Nirvana.")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Porto allows to run process in namespace with cgroup.")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Evgeniy Kilimchuk <ekilimchuk@yandex-team.ru>")
set(
    CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_CURRENT_SOURCE_DIR}/debian/postinst;${CMAKE_CURRENT_SOURCE_DIR}/debian/prerm;${CMAKE_CURRENT_SOURCE_DIR}/debian/postrm;"
)
include(CPack)
