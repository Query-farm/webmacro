cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME webmacro)

# DuckDB's extension distribution supports vcpkg. As such, dependencies can be added in ./vcpkg.json and then
# used in cmake with find_package. Feel free to remove or replace with other dependencies.
# Note that it should also be removed from vcpkg.json to prevent needlessly installing it..
if(NOT EMSCRIPTEN)
  find_package(OpenSSL REQUIRED)
endif()

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})
include_directories(src/include duckdb/third_party/httplib)

set(EXTENSION_SOURCES src/webmacro_extension.cpp src/query_farm_telemetry.cpp)

if(MINGW)
    set(OPENSSL_USE_STATIC_LIBS TRUE)
    set(WIN_LIBS crypt32 ws2_32 wsock32)
endif()

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

if(NOT EMSCRIPTEN)
  target_link_libraries(${EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto ${WIN_LIBS})
  target_link_libraries(${LOADABLE_EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto ${WIN_LIBS})
endif()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
