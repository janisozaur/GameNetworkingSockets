if(SANITIZE)
	add_definitions(-DSANITIZER)
endif()
set( GAMENETWORKINGSOCKETS_LIB "GameNetworkingSockets::static" )
add_executable(test_connection test_common.cpp test_connection.cpp)
set_target_common_gns_properties( test_connection )
target_link_libraries(test_connection ${GAMENETWORKINGSOCKETS_LIB})

add_executable(test_repro_sentinel test_common.cpp test_repro_sentinel.cpp)
set_target_common_gns_properties( test_repro_sentinel )
target_include_directories(test_repro_sentinel PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/public
    ${CMAKE_SOURCE_DIR}/src/common
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/steamnetworkingsockets/clientlib
)
target_compile_definitions(test_repro_sentinel PRIVATE
    STEAMNETWORKINGSOCKETS_STATIC_LINK
    GOOGLE_PROTOBUF_NO_RTTI
)
target_link_libraries(test_repro_sentinel ${GAMENETWORKINGSOCKETS_LIB})
add_sanitizers(test_repro_sentinel)
add_test(NAME repro_sentinel COMMAND test_repro_sentinel WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

add_executable(test_crypto test_crypto.cpp)
set_target_common_gns_properties( test_crypto )
target_include_directories(test_crypto PRIVATE ../src ../src/public ../src/common ../include)
target_link_libraries(test_crypto GameNetworkingSockets::static)
