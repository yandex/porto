project(libporto)
add_library(porto STATIC libporto.cpp)
target_link_libraries(porto rpc_proto)
