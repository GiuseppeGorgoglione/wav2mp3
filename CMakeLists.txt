cmake_minimum_required(VERSION 2.8)

project(wav2mp3)

if (WIN32)
    add_definitions(-DOS_WINDOWS)
elseif(UNIX)
    add_definitions(-DOS_UNIX)
else()
    error("Unsupported OS")
endif()

include_directories ("${PROJECT_SOURCE_DIR}/3rdparty/lame/include")
add_subdirectory (3rdparty/lame)

file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/test DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

set(SOURCES "main.c")

add_executable(${PROJECT_NAME} ${SOURCES})

target_link_libraries(${PROJECT_NAME} mp3lame)
target_link_libraries(${PROJECT_NAME} pthread)
target_link_libraries(${PROJECT_NAME} m)

