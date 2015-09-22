add_library(porto STATIC libporto.cpp error.cpp protobuf.cpp)
add_dependencies(porto util version.hpp)