project(portogo)

add_custom_command(
	OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/porto/pkg/rpc
	DEPENDS ${porto_SOURCE_DIR}/rpc.proto
	COMMAND ${PROTOBUF_PROTOC_EXECUTABLE} --go_out=${CMAKE_CURRENT_SOURCE_DIR}/porto/pkg --proto_path=${porto_SOURCE_DIR} ${porto_SOURCE_DIR}/rpc.proto
	VERBATIM)

add_custom_target(go_rpc_proto ALL DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/porto/pkg/rpc)
