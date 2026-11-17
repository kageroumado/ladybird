# The Refrax engine host: a UI process without a window. It speaks the Hub's protocol on one side and Refrax's engine
# contract, as JSON, on the other, so that Refrax can show Ladybird's pages in its own windows.
set(CMAKE_AUTOMOC OFF)
set(CMAKE_AUTORCC OFF)
set(CMAKE_AUTOUIC OFF)

# Every message the Hub can send gets a handler that does nothing, so the host only writes the ones it acts on, and a
# message that the Hub gains later needs no change here.
set(hub_client_endpoint "${CMAKE_BINARY_DIR}/Services/Hub/HubClientEndpoint.h")
set(hub_client_no_ops "${CMAKE_CURRENT_BINARY_DIR}/HubClientNoOps.inc")
add_custom_command(
    OUTPUT "${hub_client_no_ops}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_hub_client_no_ops.py" "${hub_client_endpoint}" "${hub_client_no_ops}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/generate_hub_client_no_ops.py" "${hub_client_endpoint}"
    VERBATIM
)

add_executable(RefraxLadybirdHost
    Host.cpp
    HubLink.cpp
    Wire.cpp
    main.cpp
    "${hub_client_no_ops}"
)
add_dependencies(RefraxLadybirdHost hubipc)

target_include_directories(RefraxLadybirdHost PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(RefraxLadybirdHost PRIVATE ${CMAKE_BINARY_DIR})
target_include_directories(RefraxLadybirdHost PRIVATE ${CMAKE_BINARY_DIR}/Services)
target_include_directories(RefraxLadybirdHost PRIVATE ${LADYBIRD_SOURCE_DIR})
target_include_directories(RefraxLadybirdHost PRIVATE ${LADYBIRD_SOURCE_DIR}/Services)

target_link_libraries(RefraxLadybirdHost PRIVATE AK LibCore LibFileSystem LibGfx LibHTTP LibIPC LibMain LibThreading LibURL LibWebCommon LibWebView hubipc)

ladybird_sign_helper_process(RefraxLadybirdHost)
