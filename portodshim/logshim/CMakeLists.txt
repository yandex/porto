project(logshim)
cmake_minimum_required(VERSION 3.0)
find_library(golang REQUIRED)

set(GOPATH ${CMAKE_CURRENT_BINARY_DIR})
set(SRCS main.go)
set(TARGET logshim)

add_custom_command(
  OUTPUT ${TARGET}
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  DEPENDS ${SRCS}
  COMMAND env GOPATH=${GOPATH} GOCACHE=/tmp go build -modcacherw -buildmode=exe -o "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}"
)

add_custom_target(logshim ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${TARGET})

install(
  FILES ${CMAKE_CURRENT_BINARY_DIR}/${TARGET}
  DESTINATION sbin
)
