add_library(porto STATIC libporto.cpp)
add_dependencies(porto util version.hpp)
