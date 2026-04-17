cmake_minimum_required(VERSION 3.10)

project(cpp_websocket)
set(CMAKE_CXX_STANDARD 14)

if(MSVC)
  # /wd4005  macro-redefinition
  # /wd4068  unknown pragma
  # /wd4244  conversion from 'type1' to 'type2'
  # /wd4267  conversion from 'size_t' to 'type2'
  # /wd4800  force value to bool 'true' or 'false' (performance warning)
  add_compile_options(/wd4005 /wd4068 /wd4244 /wd4267 /wd4800)
  add_definitions(/DNOMINMAX /DWIN32_LEAN_AND_MEAN=1 /D_CRT_SECURE_NO_WARNINGS)
else()
  set(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS} -g -Wno-deprecated -Wno-deprecated-declarations -Wall -fexceptions -frtti -D__STDC_FORMAT_MACROS -fPIC")
endif()

find_package(OpenSSL QUIET)
if (OPENSSL_FOUND)
    message(STATUS "OpenSSL found: ${OPENSSL_VERSION}")
    add_definitions(-D ENABLE_OPEN_SSL)
else()
    message(FATAL_ERROR "OpenSSL not found. Please install OpenSSL and try again.")
endif()

include_directories(
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/net
    ${PROJECT_SOURCE_DIR}/net/http
    ${PROJECT_SOURCE_DIR}/net/tcp
    ${PROJECT_SOURCE_DIR}/utils
    ${OPENSSL_INCLUDE_DIR}
)

add_library(${PROJECT_NAME} STATIC 
        ${PROJECT_SOURCE_DIR}/net/http/http_client.cpp
        ${PROJECT_SOURCE_DIR}/net/http/websocket/websocket_client.cpp
        ${PROJECT_SOURCE_DIR}/net/http/websocket/ws_session_base.cpp
        ${PROJECT_SOURCE_DIR}/net/http/websocket/websocket_frame.cpp
        ${PROJECT_SOURCE_DIR}/net/http/websocket/websocket_pub.cpp
        ${PROJECT_SOURCE_DIR}/utils/byte_crypto.cpp
        ${PROJECT_SOURCE_DIR}/utils/base64.cpp
)

# 查找名为 "mylib" 的库
find_library(SSL_LIBRARY
        NAMES ssl
        /opt/homebrew/Cellar/openssl@3/3.4.0/lib
        NO_DEFAULT_PATH)

target_link_libraries(
        ${PROJECT_NAME}
        ${OPENSSL_LIBRARIES}
)