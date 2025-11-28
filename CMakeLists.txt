cmake_minimum_required(VERSION 3.10)
project(ConnectTool)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set OpenGL Preference
set(OpenGL_GL_PREFERENCE "GLVND")

# Determine platform-specific Steam library
if(WIN32)
    set(STEAM_LIBRARY ${CMAKE_SOURCE_DIR}/steamworks/redistributable_bin/win64/steam_api64.lib)
    set(STEAM_RUNTIME_LIB ${CMAKE_SOURCE_DIR}/steamworks/redistributable_bin/win64/steam_api64.dll)
    set(STEAM_RUNTIME_LIB_NAME steam_api64.dll)
elseif(APPLE)
    set(STEAM_LIBRARY ${CMAKE_SOURCE_DIR}/steamworks/redistributable_bin/osx/libsteam_api.dylib)
    set(STEAM_RUNTIME_LIB ${CMAKE_SOURCE_DIR}/steamworks/redistributable_bin/osx/libsteam_api.dylib)
    set(STEAM_RUNTIME_LIB_NAME libsteam_api.dylib)
else()
    set(STEAM_LIBRARY ${CMAKE_SOURCE_DIR}/steamworks/redistributable_bin/linux64/libsteam_api.so)
    set(STEAM_RUNTIME_LIB ${CMAKE_SOURCE_DIR}/steamworks/redistributable_bin/linux64/libsteam_api.so)
    set(STEAM_RUNTIME_LIB_NAME libsteam_api.so)
endif()

# Find packages
find_package(OpenGL REQUIRED)
find_package(glfw3 REQUIRED)
find_package(Boost REQUIRED)

# Include directories
include_directories(${CMAKE_SOURCE_DIR})
include_directories(${CMAKE_SOURCE_DIR}/imgui)
include_directories(${CMAKE_SOURCE_DIR}/imgui/backends)
include_directories(${CMAKE_SOURCE_DIR}/steamworks/public)
include_directories(${CMAKE_SOURCE_DIR}/steamworks/public/steam)
include_directories(${CMAKE_SOURCE_DIR}/net)
include_directories(${CMAKE_SOURCE_DIR}/nanoid_cpp/inc)

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

# Link libraries
target_link_libraries(ConnectTool
    glfw
    OpenGL::GL
    Boost::headers
    ${STEAM_LIBRARY}
)

# Copy platform-specific Steam library to output directory for runtime
add_custom_command(TARGET ConnectTool POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
    ${STEAM_RUNTIME_LIB}
    $<TARGET_FILE_DIR:ConnectTool>/${STEAM_RUNTIME_LIB_NAME}
)