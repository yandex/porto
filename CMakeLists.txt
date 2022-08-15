cmake_minimum_required(VERSION 2.8)

if(NOT DEFINED ENV{CXX})
	execute_process(COMMAND g++ -dumpversion OUTPUT_VARIABLE GXX_VERSION)
	if(${GXX_VERSION} VERSION_LESS 4.7 AND EXISTS "/usr/bin/g++-4.7")
		set(CMAKE_C_COMPILER "/usr/bin/gcc-4.7")
		set(CMAKE_CXX_COMPILER "/usr/bin/g++-4.7")
	endif()
endif()

if(USE_CLANG)
	set(CMAKE_C_COMPILER "/usr/bin/clang")
	set(CMAKE_CXX_COMPILER "/usr/bin/clang++")
endif()

PROJECT(yandex-porto)

OPTION(ENABLE_PORTODSHIM "Enables building CRI for Porto" ON)
OPTION(ENABLE_GO "Enables building golang API" OFF)
OPTION(ENABLE_PYTHON "Enables building python API" ON)
OPTION(USE_SYSTEM_LIBNL "Use system libnl" ON)
OPTION(ENABLE_ASAN "Enables address sanitizer" OFF)
OPTION(ENABLE_GCOV "Enables coverage" OFF)
OPTION(USE_CLANG "Compile with clang" OFF)
OPTION(USE_PORTO_KERNEL "Use porto kernel module for testing" OFF)

if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE MATCHES None)
	set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Werror -Wno-deprecated-declarations -std=c++11 -fno-exceptions -g")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Werror -Wno-deprecated-declarations -std=c99 -g")

set(CMAKE_CXX_FLAGS_DEBUG "-O0 -fno-omit-frame-pointer -gdwarf-4")
set(CMAKE_C_FLAGS_DEBUG "-O0 -fno-omit-frame-pointer -gdwarf-4")

set(CMAKE_CXX_FLAGS_RELEASE "-O2")
set(CMAKE_C_FLAGS_RELEASE "-O2")

set(CMAKE_SHARED_LIBRARY_LINK_C_FLAGS "")

set(CMAKE_EXE_LINKER_FLAGS "-static-libstdc++ -static-libgcc")

if(ENABLE_ASAN)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fno-omit-frame-pointer -fno-common")
	if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libasan")
	endif()
endif()

if (ENABLE_GCOV)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --coverage")
	set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")
endif()

include(ExternalProject)

if(USE_SYSTEM_LIBNL)
	find_library(LIBNL NAMES libnl-3.a libnl-3 nl-3)
	find_library(LIBNL_IDIAG NAMES libnl-idiag-3.a libnl-idiag-3 nl-idiag-3)
	find_library(LIBNL_ROUTE NAMES libnl-route-3.a libnl-route-3 nl-route-3)
	find_path(LIBNL_INCLUDE_DIR NAMES netlink/netlink.h PATH_SUFFIXES libnl3)
	include_directories(${LIBNL_INCLUDE_DIR})
else()
	if(NOT EXISTS "/usr/bin/autoreconf")
		message(FATAL_ERROR "dh-autoreconf is required to build libnl")
	endif()

	if(NOT EXISTS "/usr/bin/pkg-config")
		message(FATAL_ERROR "pkg-config is required to build libnl")
	endif()

	find_package(BISON REQUIRED)
	find_package(FLEX REQUIRED)

	ExternalProject_Add(libnl
		URL https://github.com/thom311/libnl/releases/download/libnl3_2_27/libnl-3.2.27.tar.gz
		URL_MD5 cec19f941b79a38d9eb92dbc171d6dbd
		UPDATE_COMMAND autoreconf -fi
		CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=<INSTALL_DIR> --disable-cli --disable-pthreads
		BUILD_COMMAND $(MAKE)
		INSTALL_COMMAND $(MAKE) install)
	ExternalProject_Get_Property(libnl INSTALL_DIR)
	include_directories(${INSTALL_DIR}/include/libnl3)
	SET(LIBNL ${INSTALL_DIR}/lib/libnl-3.a)
	SET(LIBNL_IDIAG ${INSTALL_DIR}/lib/libnl-idiag-3.a)
	SET(LIBNL_ROUTE ${INSTALL_DIR}/lib/libnl-route-3.a)
endif()

if(NOT OPENSSL_TGZ_URL)
    set(OPENSSL_TGZ_URL "https://github.com/openssl/openssl/archive/refs/tags/OpenSSL_1_1_1o.tar.gz")
endif()

ExternalProject_Add(openssl
    URL ${OPENSSL_TGZ_URL}
    URL_MD5 8c7daf923abc12d4492e82d576c779f2
    CONFIGURE_COMMAND <SOURCE_DIR>/config --prefix=<INSTALL_DIR> no-dso
    BUILD_COMMAND $(MAKE)
    INSTALL_COMMAND $(MAKE) install_sw)
ExternalProject_Get_Property(openssl INSTALL_DIR)
include_directories(${INSTALL_DIR}/include)
SET(LIBSSL ${INSTALL_DIR}/lib/libssl.a)
SET(LIBCRYPTO ${INSTALL_DIR}/lib/libcrypto.a)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DCPPHTTPLIB_OPENSSL_SUPPORT")

find_library(PB NAMES libprotobuf.a)
find_package(Protobuf REQUIRED)
# FindProtobuf requires only the libraries to be present
if(NOT PROTOBUF_PROTOC_EXECUTABLE)
	message(FATAL_ERROR "Couldn't find protoc executable.")
endif()

find_package(Curses REQUIRED)

enable_testing()
add_custom_target(porto_test COMMAND ctest -V -T Test)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

set(CMAKE_INSTALL_PREFIX /usr)

add_subdirectory(src)
add_subdirectory(test)

if(ENABLE_PORTODSHIM)
	add_subdirectory(portodshim)
endif()

if(ENABLE_GO)
	add_subdirectory(src/api/go)
endif()

if(ENABLE_PYTHON)
	include(FindPythonInterp)
	add_subdirectory(src/api/python)
endif()

set_source_files_properties(tags PROPERTIES GENERATED true)
add_custom_target(tags COMMAND ctags -R --c++-kinds=+p --fields=+iaS --extra=+q . WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

set_source_files_properties(TAGS PROPERTIES GENERATED true)
add_custom_target(TAGS COMMAND ctags -R -e --c++-kinds=+p --fields=+iaS --extra=+q . WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
