cmake_minimum_required(VERSION 3.24)
if(CMAKE_HOST_APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS deployment target")
endif()
project(xdnote_desktop_admission VERSION 0.1.0 LANGUAGES CXX)
if(MSVC AND MSVC_VERSION LESS 1930)
    message(FATAL_ERROR "The pinned Qt SDK requires MSVC 2022 (19.30+) or newer")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
include(CTest)
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
file(READ "${PROJECT_SOURCE_DIR}/tools/desktop-dependencies.json" XDNOTE_DEPENDENCIES)
string(JSON XDNOTE_QT_VERSION GET "${XDNOTE_DEPENDENCIES}" qt version)
string(JSON XDNOTE_RUST_VERSION GET "${XDNOTE_DEPENDENCIES}" rust toolchain)
find_package(Qt6 ${XDNOTE_QT_VERSION} EXACT REQUIRED COMPONENTS
    Core Gui Widgets Core5Compat Qml Quick QuickWidgets QuickControls2 WebEngineWidgets Concurrent)
find_program(XDNOTE_RUSTUP rustup REQUIRED)
execute_process(COMMAND "${XDNOTE_RUSTUP}" run "${XDNOTE_RUST_VERSION}" rustc -vV
    OUTPUT_VARIABLE XDNOTE_RUST_INFO RESULT_VARIABLE XDNOTE_RUST_STATUS OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT XDNOTE_RUST_STATUS EQUAL 0)
    message(FATAL_ERROR "Pinned Rust is missing; explicitly prepare the toolchain before configuring")
endif()
string(REGEX MATCH "host: ([^\r\n]+)" XDNOTE_RUST_HOST "${XDNOTE_RUST_INFO}")
set(XDNOTE_RUST_TARGET "${CMAKE_MATCH_1}" CACHE STRING "Native Rust target for this single-architecture build")
if(NOT XDNOTE_RUST_TARGET)
    message(FATAL_ERROR "Cannot determine Rust target")
endif()
if(APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES ";")
    message(FATAL_ERROR "Admission builds one architecture at a time; universal packaging is not implemented")
endif()

set(XDNOTE_SCINTILLA_SOURCE "${PROJECT_SOURCE_DIR}/build/scintilla-5.6.7/scintilla")
if(NOT EXISTS "${XDNOTE_SCINTILLA_SOURCE}/version.txt")
    message(FATAL_ERROR "Scintilla source missing; run the explicit prepare command")
endif()
file(READ "${XDNOTE_SCINTILLA_SOURCE}/version.txt" XDNOTE_SCINTILLA_VERSION)
string(STRIP "${XDNOTE_SCINTILLA_VERSION}" XDNOTE_SCINTILLA_VERSION)
if(NOT XDNOTE_SCINTILLA_VERSION STREQUAL "567")
    message(FATAL_ERROR "Unexpected Scintilla source version")
endif()
include(cmake/Scintilla.cmake)

set(XDNOTE_CARGO_HOME "${PROJECT_SOURCE_DIR}/build/cargo-home" CACHE PATH "Explicitly prepared Cargo cache")
if(WIN32)
    set(XDNOTE_RUST_LIBRARY "${CMAKE_BINARY_DIR}/cargo/${XDNOTE_RUST_TARGET}/release/xdnote_markdown.lib")
else()
    set(XDNOTE_RUST_LIBRARY "${CMAKE_BINARY_DIR}/cargo/${XDNOTE_RUST_TARGET}/release/libxdnote_markdown.a")
endif()
add_custom_command(OUTPUT "${XDNOTE_RUST_LIBRARY}"
    COMMAND "${CMAKE_COMMAND}" -E env "CARGO_HOME=${XDNOTE_CARGO_HOME}" "CARGO_NET_OFFLINE=true"
        "RUSTUP_AUTO_INSTALL=0" "${XDNOTE_RUSTUP}" run "${XDNOTE_RUST_VERSION}" cargo build
        --manifest-path "${PROJECT_SOURCE_DIR}/Cargo.toml" --locked --offline --release
        --target "${XDNOTE_RUST_TARGET}" --target-dir "${CMAKE_BINARY_DIR}/cargo"
    DEPENDS Cargo.toml Cargo.lock rust-toolchain.toml crates/markdown/Cargo.toml crates/markdown/src/lib.rs
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" VERBATIM)
add_custom_target(xdnote_rust_build DEPENDS "${XDNOTE_RUST_LIBRARY}")
add_library(xdnote_markdown STATIC IMPORTED GLOBAL)
set_target_properties(xdnote_markdown PROPERTIES IMPORTED_LOCATION "${XDNOTE_RUST_LIBRARY}")
add_dependencies(xdnote_markdown xdnote_rust_build)

add_library(xdnote_desktop STATIC
    apps/desktop/src/DocumentSession.cpp apps/desktop/src/DocumentSession.h
    apps/desktop/src/SourceEditor.cpp apps/desktop/src/SourceEditor.h
    apps/desktop/src/MarkdownPreview.cpp apps/desktop/src/MarkdownPreview.h
    apps/desktop/src/VisualTokens.cpp apps/desktop/src/VisualTokens.h
    apps/desktop/src/InputModality.cpp apps/desktop/src/InputModality.h
    apps/desktop/src/GuiRuntime.cpp apps/desktop/src/GuiRuntime.h
    apps/desktop/src/AdmissionWindow.cpp apps/desktop/src/AdmissionWindow.h)
target_include_directories(xdnote_desktop PUBLIC apps/desktop/src)
target_link_libraries(xdnote_desktop PUBLIC xdnote_scintilla xdnote_markdown
    Qt6::Widgets Qt6::Qml Qt6::Quick Qt6::QuickWidgets Qt6::QuickControls2 Qt6::WebEngineWidgets Qt6::Concurrent)
if(WIN32)
    target_link_libraries(xdnote_desktop PUBLIC ws2_32 userenv bcrypt ntdll advapi32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(xdnote_desktop PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
endif()
qt_add_resources(xdnote_desktop xdnote_assets PREFIX /xdnote BASE "${PROJECT_SOURCE_DIR}" FILES
    docs/visual/tokens.json apps/desktop/ui/Chrome.qml apps/desktop/ui/SharedButton.qml)

add_executable(xdnote_admission apps/desktop/admission_main.cpp)
set_target_properties(xdnote_admission PROPERTIES OUTPUT_NAME xdnote-admission)
target_link_libraries(xdnote_admission PRIVATE xdnote_desktop)
if(MSVC)
    foreach(target xdnote_scintilla xdnote_desktop xdnote_admission)
        target_compile_options(${target} PRIVATE /utf-8 /EHsc)
    endforeach()
endif()

if(BUILD_TESTING)
    find_package(Qt6 ${XDNOTE_QT_VERSION} EXACT REQUIRED COMPONENTS Test)
    add_executable(xdnote_desktop_tests apps/desktop/tests/desktop_tests.cpp)
    target_link_libraries(xdnote_desktop_tests PRIVATE xdnote_desktop Qt6::Test)
    if(MSVC)
        target_compile_options(xdnote_desktop_tests PRIVATE /utf-8 /EHsc)
    endif()
    add_test(NAME desktop_admission COMMAND xdnote_desktop_tests
        -o -,txt -o "${CMAKE_BINARY_DIR}/desktop-qtest.txt,txt")
    set_tests_properties(desktop_admission PROPERTIES TIMEOUT 180)
endif()
