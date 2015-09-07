cmake_minimum_required(VERSION 2.8)

PROJECT(yandex-porto)

OPTION(ENABLE_TEST "Enables building portotest utility" ON)
OPTION(ENABLE_PYTHON "Enables building python API" ON)
OPTION(USE_SYSTEM_LIBNL "Use system libnl" OFF)
OPTION(ENABLE_ASAN "Enables address sanitizer" OFF)

if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE MATCHES None)
	set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -std=c++11 -g")
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

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_subdirectory(src)