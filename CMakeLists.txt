cmake_minimum_required(VERSION 3.20)
project(Tenhou VERSION 1.0.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "Use MSVC Win32: cmake -S . -B build -A Win32")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
add_compile_options(/W4 /utf-8 /EHsc)

find_program(POWERSHELL_EXECUTABLE NAMES powershell pwsh REQUIRED)
file(GLOB LOBBY_INPUTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/tenhou4/*")
set(LOBBY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/web")
set(LOBBY_OUTPUTS
  "${LOBBY_OUTPUT_DIRECTORY}/index.html"
  "${LOBBY_OUTPUT_DIRECTORY}/assets/renderer.js"
  "${LOBBY_OUTPUT_DIRECTORY}/assets/languages.js"
)
foreach(input IN LISTS LOBBY_INPUTS)
  if(input MATCHES "\\.(png|jpg|ttf)$")
    get_filename_component(filename "${input}" NAME)
    list(APPEND LOBBY_OUTPUTS "${LOBBY_OUTPUT_DIRECTORY}/assets/${filename}")
  endif()
endforeach()
add_custom_command(
  OUTPUT ${LOBBY_OUTPUTS}
  COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
    -File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_lobby.ps1"
    -OutputDirectory "${LOBBY_OUTPUT_DIRECTORY}"
  DEPENDS
    scripts/build_lobby.ps1
    src/web/index.html
    src/web/adapter.js
    ${LOBBY_INPUTS}
  COMMENT "Generating local 4K lobby assets"
  VERBATIM
)
add_custom_target(tenhou_lobby ALL DEPENDS ${LOBBY_OUTPUTS})

add_library(tenhou_patch SHARED
  src/patch/patch.cpp
  src/native/counter.cpp
  src/native/chat_policy.cpp
  src/native/input_policy.cpp
  src/localization/localization.cpp
  src/lobby/web_lobby.cpp
  src/lobby/lobby_protocol.cpp
  src/lobby/web_transport.cpp
  src/lobby/web_wire.cpp
  src/lobby/lobby_services.cpp
  src/lobby/local_replays.cpp
)
target_include_directories(tenhou_patch PRIVATE
  src/common
  src/native
  src/localization
  src/localization/generated
  src/lobby
  vendor/webview2/include
)
target_link_libraries(tenhou_patch PRIVATE user32 gdi32 shell32 bcrypt crypt32 gdiplus comctl32 comdlg32 oleaut32 ole32 winhttp ws2_32)
add_executable(TenhouEnhanced WIN32
  src/launcher/loader.cpp
  src/launcher/launcher.res
)
set_source_files_properties(src/launcher/launcher.res PROPERTIES EXTERNAL_OBJECT TRUE)
target_include_directories(TenhouEnhanced PRIVATE src/common)
add_dependencies(TenhouEnhanced tenhou_lobby tenhou_patch)
target_link_libraries(TenhouEnhanced PRIVATE user32 shell32)
