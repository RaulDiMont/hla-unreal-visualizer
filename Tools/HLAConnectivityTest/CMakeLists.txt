cmake_minimum_required(VERSION 3.20)
project(HLAConnectivityTest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Path to the installed OpenRTI Windows build (Phase 4.1 output)
set(OPENRTI_DIR "C:/OpenRTI-0.10.0-win64" CACHE PATH "Path to OpenRTI Windows install directory")

if(NOT EXISTS "${OPENRTI_DIR}/include")
    message(FATAL_ERROR
        "OpenRTI headers not found at ${OPENRTI_DIR}/include\n"
        "Run Phase 4.1 Step 2 to build and install OpenRTI first.")
endif()

add_executable(HLAConnectivityTest main.cpp)

target_include_directories(HLAConnectivityTest PRIVATE
    "${OPENRTI_DIR}/include/rti1516e"
)

target_link_libraries(HLAConnectivityTest PRIVATE
    "${OPENRTI_DIR}/lib/rti1516e.lib"
)

# Copy DLLs next to the exe — librti1516e.dll depends on OpenRTI.dll
add_custom_command(TARGET HLAConnectivityTest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${OPENRTI_DIR}/bin/librti1516e.dll"
        "$<TARGET_FILE_DIR:HLAConnectivityTest>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${OPENRTI_DIR}/bin/libfedtime1516e.dll"
        "$<TARGET_FILE_DIR:HLAConnectivityTest>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${OPENRTI_DIR}/bin/OpenRTI.dll"
        "$<TARGET_FILE_DIR:HLAConnectivityTest>"
    COMMENT "Copying OpenRTI DLLs to output directory"
)
