cmake_minimum_required(VERSION 2.6)
project (WEBRTC)
find_package(PkgConfig)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "CMakeTargets")
function(set_file_group prefix)
  message(STATUS "set_file_group " ${prefix} " " ${ARGC})
  foreach(FILE IN LISTS ARGN 1)
    # Get the directory of the source file
    get_filename_component(PARENT_DIR "${FILE}" DIRECTORY)

    # Remove common directory prefix to make the group
    string(REPLACE "${prefix}" "" GROUP "${PARENT_DIR}")

    # Make sure we are using windows slashes
    string(REPLACE "/" "\\" GROUP "${GROUP}")

    source_group("${GROUP}" FILES "${FILE}")
  endforeach()
endfunction()

set (WEBRTC_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/webrtc)

set(CMAKE_CXX_FLAGS "-g -Wall -std=c++17")
if(MSVC)
  # /wd4005  macro-redefinition
  # /wd4068  unknown pragma
  # /wd4244  conversion from 'type1' to 'type2'
  # /wd4267  conversion from 'size_t' to 'type2'
  # /wd4800  force value to bool 'true' or 'false' (performance warning)
  add_compile_options(/wd4005 /wd4068 /wd4244 /wd4267 /wd4800)
  add_definitions(/DNOMINMAX /DWIN32_LEAN_AND_MEAN=1 /D_CRT_SECURE_NO_WARNINGS)
endif()
# ABSEIL
add_subdirectory(deps/abseil-cpp/abseil-cpp)

# 检查操作系统并添加相应的源文件
if (WIN32)
    add_definitions(-DWEBRTC_WIN)
elseif(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}  -fno-stack-protector -DWEBRTC_POSIX -DWEBRTC_MAC")
else()
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC -DWEBRTC_POSIX -DWEBRTC_LINUX")
endif()

include_directories(deps/abseil-cpp/abseil-cpp ${WEBRTC_SOURCE_DIR})

file(GLOB_RECURSE WEBRTC_SOURCES "${WEBRTC_SOURCE_DIR}/*.c" "${WEBRTC_SOURCE_DIR}/*.cpp" "${WEBRTC_SOURCE_DIR}/*.cc")
file(GLOB_RECURSE WEBRTC_HEADERS "${WEBRTC_SOURCE_DIR}/*.h")

set_file_group("${WEBRTC_SOURCE_DIR}" ${WEBRTC_SOURCES})
set_file_group("${WEBRTC_SOURCE_DIR}" ${WEBRTC_HEADERS})
add_library(webrtc STATIC ${WEBRTC_SOURCES} ${WEBRTC_HEADERS})

target_include_directories(webrtc PUBLIC ${WEBRTC_SOURCE_DIR})

target_link_libraries(webrtc absl::base)
