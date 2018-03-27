project(python-portopy)

add_custom_command(
	OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/porto/rpc_pb2.py
	DEPENDS ${porto_SOURCE_DIR}/rpc.proto
	COMMAND ${PROTOBUF_PROTOC_EXECUTABLE} --python_out=${CMAKE_CURRENT_SOURCE_DIR}/porto/ --proto_path=${porto_SOURCE_DIR} ${porto_SOURCE_DIR}/rpc.proto
	VERBATIM)

add_custom_target(python_rpc_proto ALL DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/porto/rpc_pb2.py)

install(CODE "execute_process(WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} COMMAND ${PYTHON_EXECUTABLE} -uB setup.py install --root=\$ENV{DESTDIR} --prefix=\${CMAKE_INSTALL_PREFIX})")
install(CODE "execute_process(WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} COMMAND python3 -uB setup.py install --root=\$ENV{DESTDIR} --prefix=\${CMAKE_INSTALL_PREFIX})")
