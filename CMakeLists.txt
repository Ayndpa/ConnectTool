cmake_minimum_required(VERSION 3.10)
project(ConnectTool)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find packages
find_package(OpenGL REQUIRED)
find_package(glfw3 REQUIRED)
find_package(Boost REQUIRED)

# Include directories
include_directories(${CMAKE_SOURCE_DIR})
include_directories(${CMAKE_SOURCE_DIR}/imgui)
include_directories(${CMAKE_SOURCE_DIR}/imgui/backends)
include_directories(${CMAKE_SOURCE_DIR}/nanoid_cpp/inc)
include_directories(${CMAKE_SOURCE_DIR}/steamworks/public)
include_directories(${CMAKE_SOURCE_DIR}/steamworks/public/steam)
include_directories(${CMAKE_SOURCE_DIR}/net)

# Source files
file(GLOB SOURCES
    "online_game_tool.cpp"
    "imgui/*.cpp"
    "imgui/backends/imgui_impl_glfw.cpp"
    "imgui/backends/imgui_impl_opengl3.cpp"
    "net/*.cpp"
    "steam/*.cpp"
    "nanoid_cpp/src/nanoid/*.cpp"
)

# Create executable
add_executable(ConnectTool ${SOURCES})

# Determine Steamworks directory location with fallback
set(STEAMWORKS_DIR "${CMAKE_SOURCE_DIR}/steamworks")
set(STEAMWORKS_SDK_DIR "${CMAKE_SOURCE_DIR}/sdk")

# Function to find Steam API library with fallback
function(find_steam_api OUTPUT_VAR PLATFORM_PATH)
    set(STEAM_API_PATH "${STEAMWORKS_DIR}/redistributable_bin/${PLATFORM_PATH}")
    set(STEAM_API_SDK_PATH "${STEAMWORKS_SDK_DIR}/redistributable_bin/${PLATFORM_PATH}")
    
    if(EXISTS "${STEAM_API_PATH}")
        set(${OUTPUT_VAR} "${STEAM_API_PATH}" PARENT_SCOPE)
    elseif(EXISTS "${STEAM_API_SDK_PATH}")
        set(${OUTPUT_VAR} "${STEAM_API_SDK_PATH}" PARENT_SCOPE)
        message(STATUS "Using Steam API from sdk directory: ${STEAM_API_SDK_PATH}")
    else()
        set(${OUTPUT_VAR} "${STEAM_API_PATH}" PARENT_SCOPE)
        message(WARNING "Steam API library not found at: ${STEAM_API_PATH} or ${STEAM_API_SDK_PATH}")
    endif()
endfunction()

# Platform-specific Steam API linking
if(WIN32)
    # Windows 64-bit
    find_steam_api(STEAM_API_LIB "win64/steam_api64.dll")
    target_link_libraries(ConnectTool
        glfw
        OpenGL::GL
        Boost::headers
        ${STEAM_API_LIB}
    )
    
    # Copy steam_api64.dll to output directory for runtime
    add_custom_command(TARGET ConnectTool POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
        ${STEAM_API_LIB}
        $<TARGET_FILE_DIR:ConnectTool>/steam_api64.dll
    )
elseif(UNIX AND NOT APPLE)
    # Linux platforms
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "amd64")
        # x86_64 Linux
        find_steam_api(STEAM_API_LIB "linux64/libsteam_api.so")
        target_link_libraries(ConnectTool
            glfw
            OpenGL::GL
            Boost::headers
            ${STEAM_API_LIB}
        )
        
        add_custom_command(TARGET ConnectTool POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
            ${STEAM_API_LIB}
            $<TARGET_FILE_DIR:ConnectTool>/libsteam_api.so
        )
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "i386" OR CMAKE_SYSTEM_PROCESSOR MATCHES "i686")
        # x86 32-bit Linux
        find_steam_api(STEAM_API_LIB "linux32/libsteam_api.so")
        target_link_libraries(ConnectTool
            glfw
            OpenGL::GL
            Boost::headers
            ${STEAM_API_LIB}
        )
        
        add_custom_command(TARGET ConnectTool POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
            ${STEAM_API_LIB}
            $<TARGET_FILE_DIR:ConnectTool>/libsteam_api.so
        )
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
        # ARM64 Linux
        find_steam_api(STEAM_API_LIB "linuxarm64/libsteam_api.so")
        target_link_libraries(ConnectTool
            glfw
            OpenGL::GL
            Boost::headers
            ${STEAM_API_LIB}
        )
        
        add_custom_command(TARGET ConnectTool POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
            ${STEAM_API_LIB}
            $<TARGET_FILE_DIR:ConnectTool>/libsteam_api.so
        )
    else()
        message(WARNING "Unsupported Linux architecture: ${CMAKE_SYSTEM_PROCESSOR}")
    endif()
elseif(APPLE)
    # macOS
    find_steam_api(STEAM_API_LIB "osx/libsteam_api.dylib")
    target_link_libraries(ConnectTool
        glfw
        OpenGL::GL
        Boost::headers
        ${STEAM_API_LIB}
    )
    
    # Copy libsteam_api.dylib to output directory for runtime
    add_custom_command(TARGET ConnectTool POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
        ${STEAM_API_LIB}
        $<TARGET_FILE_DIR:ConnectTool>/libsteam_api.dylib
    )
else()
    message(WARNING "Unsupported platform")
endif()

# Create steam_id.txt file with content 480
add_custom_command(TARGET ConnectTool POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "480" > $<TARGET_FILE_DIR:ConnectTool>/steam_appid.txt
)
