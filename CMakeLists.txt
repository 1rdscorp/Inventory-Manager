cmake_minimum_required(VERSION 3.24)
project(InventoryManager VERSION 2.1.0 LANGUAGES C CXX)

if(NOT MSVC)
  message(FATAL_ERROR "Build with the Microsoft C++ compiler (run build.bat, or use a Visual Studio developer prompt).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static C/C++ runtime: the .exe runs without installing the Visual C++ Redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------- third-party code
# Downloaded once into the build folder and checked against fixed SHA-256 hashes.
set(DEPS "${CMAKE_BINARY_DIR}/_deps")

function(fetch_file url dest sha256)
  file(DOWNLOAD "${url}" "${dest}" EXPECTED_HASH SHA256=${sha256} TLS_VERIFY ON STATUS status)
  list(GET status 0 code)
  if(NOT code EQUAL 0)
    message(FATAL_ERROR "Download failed: ${url} (${status})")
  endif()
endfunction()

function(fetch_archive url name sha256 marker)
  if(NOT EXISTS "${DEPS}/${name}/${marker}")
    fetch_file("${url}" "${DEPS}/${name}.download" ${sha256})
    file(ARCHIVE_EXTRACT INPUT "${DEPS}/${name}.download" DESTINATION "${DEPS}/${name}")
  endif()
endfunction()

fetch_file(https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.18.3/httplib.h
  "${DEPS}/include/httplib.h" a0a0c13dc086663863dbe6730a19716f7d3744904b6205496e8301750814dbd5)
fetch_file(https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp
  "${DEPS}/include/nlohmann/json.hpp" 9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6)
fetch_archive(https://github.com/zeux/pugixml/releases/download/v1.14/pugixml-1.14.tar.gz
  pugixml 2f10e276870c64b1db6809050a75e11a897a8d7456c4be5c6b2e35a11168a015 pugixml-1.14/src/pugixml.cpp)
fetch_archive(https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip
  miniz ada38db0b703a56d3dd6d57bf84a9c5d664921d870d8fea4db153979fb5332c5 miniz.c)
fetch_archive(https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
  sqlite 77823cb110929c2bcb0f5d48e4833b5c59a8a6e40cdea3936b99e199dbbe5784 sqlite-amalgamation-3460100/sqlite3.c)
fetch_archive(https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.2792.45
  webview2 02ffd77440c09b5ff60f5f06defdc3fb07fb5630077c7cf6115bf6be793f91e3 build/native/include/WebView2.h)

set(SQLITE_DIR "${DEPS}/sqlite/sqlite-amalgamation-3460100")
set(PUGIXML_DIR "${DEPS}/pugixml/pugixml-1.14/src")
set(WEBVIEW2_DIR "${DEPS}/webview2/build/native")

add_library(thirdparty STATIC
  "${SQLITE_DIR}/sqlite3.c"
  "${DEPS}/miniz/miniz.c"
  "${PUGIXML_DIR}/pugixml.cpp")
target_include_directories(thirdparty PUBLIC
  "${DEPS}/include" "${SQLITE_DIR}" "${DEPS}/miniz" "${PUGIXML_DIR}")
target_compile_definitions(thirdparty PUBLIC
  SQLITE_THREADSAFE=1 SQLITE_DQS=0 SQLITE_OMIT_LOAD_EXTENSION SQLITE_DEFAULT_FOREIGN_KEYS=1)
target_compile_options(thirdparty PRIVATE /W0)

# ---------------------------------------------------------------- the app
configure_file(src/resources.rc.in "${CMAKE_BINARY_DIR}/resources.rc" @ONLY)
# Rebuild the resources when the bundled page files or icon change.
set_source_files_properties("${CMAKE_BINARY_DIR}/resources.rc" PROPERTIES OBJECT_DEPENDS
  "${PROJECT_SOURCE_DIR}/static/index.html;${PROJECT_SOURCE_DIR}/static/app.js;${PROJECT_SOURCE_DIR}/static/styles.css;${PROJECT_SOURCE_DIR}/packaging/app.ico")

add_executable(InventoryManager WIN32
  src/main.cpp
  src/server.cpp
  src/spreadsheet.cpp
  src/store.cpp
  src/util.cpp
  src/app.manifest
  "${CMAKE_BINARY_DIR}/resources.rc")
target_include_directories(InventoryManager PRIVATE src "${WEBVIEW2_DIR}/include")
target_compile_definitions(InventoryManager PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 CPPHTTPLIB_THREAD_POOL_COUNT=4)
target_compile_options(InventoryManager PRIVATE /W4 /utf-8 /permissive- /wd4100)
target_link_libraries(InventoryManager PRIVATE thirdparty
  "${WEBVIEW2_DIR}/x64/WebView2LoaderStatic.lib" ws2_32 version shlwapi)

# Put the starter spreadsheet next to the .exe so a first run imports it.
add_custom_command(TARGET InventoryManager POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/inventory_management.xlsx" "$<TARGET_FILE_DIR:InventoryManager>")
