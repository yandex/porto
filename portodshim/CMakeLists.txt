project(portodshim)
cmake_minimum_required(VERSION 3.0)
find_library(golang REQUIRED)

set(GOPATH ${CMAKE_CURRENT_BINARY_DIR})
set(SRCS config.go cri_api.go go.mod go.sum image_mapper.go main.go runtime_mapper.go server.go registry.go streaming.go)
set(TARGET portodshim)

add_custom_command(
  OUTPUT ${TARGET}
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  DEPENDS ${SRCS}
  COMMAND env GOPATH=${GOPATH} GOCACHE=/tmp go build -modcacherw -buildmode=exe -o "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}"
)

add_custom_target(cri_plugin ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${TARGET})

install(
  FILES ${CMAKE_CURRENT_BINARY_DIR}/${TARGET}
  DESTINATION sbin
)

add_subdirectory(logshim)
