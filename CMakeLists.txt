cmake_minimum_required(VERSION 3.24)
if(CMAKE_HOST_APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS deployment target")
endif()
project(xdnote_desktop_admission VERSION 0.1.0 LANGUAGES CXX)
if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 17)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
include(CTest)
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
file(READ "${PROJECT_SOURCE_DIR}/tools/desktop-dependencies.json" XDNOTE_DEPENDENCIES)
string(JSON XDNOTE_QT_VERSION GET "${XDNOTE_DEPENDENCIES}" qt version)
set(XDNOTE_BUILD_PROFILE "default" CACHE STRING "Explicit toolchain profile")
set_property(CACHE XDNOTE_BUILD_PROFILE PROPERTY STRINGS default msvc2019)
set(XDNOTE_MINIMUM_MSVC 1930)
if(XDNOTE_BUILD_PROFILE STREQUAL "msvc2019")
    if(NOT WIN32)
        message(FATAL_ERROR "The msvc2019 compatibility profile is Windows-only")
    endif()
    string(JSON XDNOTE_MINIMUM_MSVC GET "${XDNOTE_DEPENDENCIES}" compatibilityProfiles msvc2019 minimumMsvc)
    if(NOT MSVC OR MSVC_VERSION LESS 1929 OR MSVC_VERSION GREATER_EQUAL 1930)
        message(FATAL_ERROR "The compatibility profile requires the actual MSVC v142 14.29 compiler")
    endif()
elseif(NOT XDNOTE_BUILD_PROFILE STREQUAL "default")
    message(FATAL_ERROR "Unknown desktop build profile")
endif()
if(MSVC AND MSVC_VERSION LESS XDNOTE_MINIMUM_MSVC)
    message(FATAL_ERROR "Compiler is too old for the selected profile; select the explicit msvc2019 compatibility profile")
endif()
message(STATUS "xdnote profile=${XDNOTE_BUILD_PROFILE}, Qt=${XDNOTE_QT_VERSION}, compiler=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/native-toolchain.json" CONTENT
    "{\"profile\":\"${XDNOTE_BUILD_PROFILE}\",\"qtVersion\":\"${XDNOTE_QT_VERSION}\",\"compiler\":\"${CMAKE_CXX_COMPILER_ID}\",\"compilerVersion\":\"${CMAKE_CXX_COMPILER_VERSION}\"}\n")
string(JSON XDNOTE_RUST_VERSION GET "${XDNOTE_DEPENDENCIES}" rust toolchain)
find_package(Qt6 ${XDNOTE_QT_VERSION} EXACT REQUIRED COMPONENTS
    Core Gui Widgets Core5Compat Qml Quick QuickWidgets QuickControls2 WebEngineWidgets Concurrent Network Svg)
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
file(GLOB_RECURSE XDNOTE_RUST_SOURCES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/crates/*.rs")
add_custom_command(OUTPUT "${XDNOTE_RUST_LIBRARY}"
    COMMAND "${CMAKE_COMMAND}" -E env "CARGO_HOME=${XDNOTE_CARGO_HOME}" "CARGO_NET_OFFLINE=true"
        "RUSTUP_AUTO_INSTALL=0" "${XDNOTE_RUSTUP}" run "${XDNOTE_RUST_VERSION}" cargo build
        --manifest-path "${PROJECT_SOURCE_DIR}/Cargo.toml" --locked --offline --release
        --target "${XDNOTE_RUST_TARGET}" --target-dir "${CMAKE_BINARY_DIR}/cargo"
    DEPENDS Cargo.toml Cargo.lock rust-toolchain.toml crates/markdown/Cargo.toml ${XDNOTE_RUST_SOURCES}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" VERBATIM)
add_custom_target(xdnote_rust_build DEPENDS "${XDNOTE_RUST_LIBRARY}")
add_library(xdnote_markdown STATIC IMPORTED GLOBAL)
set_target_properties(xdnote_markdown PROPERTIES IMPORTED_LOCATION "${XDNOTE_RUST_LIBRARY}")
add_dependencies(xdnote_markdown xdnote_rust_build)
include(cmake/NativePreviewModel.cmake)
include(cmake/NativePreviewView.cmake)

if(UNIX)
    add_executable(xdnote_resource_helper apps/desktop/src/ResourceHelper.cpp
        apps/desktop/src/FileService.cpp apps/desktop/src/WorkspaceResourceReader.cpp)
    set_target_properties(xdnote_resource_helper PROPERTIES OUTPUT_NAME "xdnote-resource-helper")
    target_link_libraries(xdnote_resource_helper PRIVATE Qt6::Core)
    if(APPLE)
        find_library(XDNOTE_SANDBOX_LIBRARY sandbox REQUIRED)
        target_link_libraries(xdnote_resource_helper PRIVATE "${XDNOTE_SANDBOX_LIBRARY}")
    endif()
endif()

add_library(xdnote_desktop STATIC
    apps/desktop/src/DocumentBreadcrumb.cpp apps/desktop/src/DocumentBreadcrumb.h
    apps/desktop/src/NavigationStore.cpp apps/desktop/src/NavigationStore.h
    apps/desktop/src/WorkspaceNavigation.cpp apps/desktop/src/WorkspaceNavigation.h
    apps/desktop/src/WorkspaceTree.cpp apps/desktop/src/WorkspaceTree.h
    apps/desktop/src/WorkspaceWindow.cpp apps/desktop/src/WorkspaceWindow.h
    apps/desktop/src/WindowChrome.cpp apps/desktop/src/WindowChrome.h
    apps/desktop/src/WindowGeometry.cpp apps/desktop/src/WindowGeometry.h
    apps/desktop/src/UiFonts.cpp apps/desktop/src/UiFonts.h
    apps/desktop/src/Appearance.cpp apps/desktop/src/Appearance.h
    apps/desktop/src/SharedWidgets.cpp apps/desktop/src/SharedWidgets.h
    apps/desktop/src/FileManager.cpp apps/desktop/src/FileManager.h
    apps/desktop/src/SingleFileWindow.cpp apps/desktop/src/SingleFileWindow.h
    apps/desktop/src/FileApplication.cpp apps/desktop/src/FileApplication.h
    apps/desktop/src/SingleInstance.cpp apps/desktop/src/SingleInstance.h
    apps/desktop/src/FileService.cpp apps/desktop/src/FileService.h
    apps/desktop/src/Utf8Text.h
    apps/desktop/src/SessionRegistry.cpp apps/desktop/src/SessionRegistry.h
    apps/desktop/src/DocumentSession.cpp apps/desktop/src/DocumentSession.h
    apps/desktop/src/SourceEditor.cpp apps/desktop/src/SourceEditor.h
    apps/desktop/src/MarkdownPreview.cpp apps/desktop/src/MarkdownPreview.h
    apps/desktop/src/MarkdownReferences.cpp apps/desktop/src/MarkdownReferences.h apps/desktop/src/MarkdownBridge.h
    apps/desktop/src/MarkdownHeadings.cpp apps/desktop/src/MarkdownHeadings.h
    apps/desktop/src/WorkspacePath.cpp apps/desktop/src/WorkspacePath.h
    apps/desktop/src/WorkspaceReferenceIndex.cpp apps/desktop/src/WorkspaceReferenceIndex.h
    apps/desktop/src/ReferenceMovePlan.cpp apps/desktop/src/ReferenceMovePlan.h
    apps/desktop/src/ResourceMaintenance.cpp apps/desktop/src/ResourceMaintenance.h
    apps/desktop/src/ResourceMaintenanceController.cpp apps/desktop/src/ResourceMaintenanceController.h
    apps/desktop/src/WorkspaceReferenceScan.cpp apps/desktop/src/WorkspaceReferenceScan.h
    apps/desktop/src/WorkspaceReferenceController.cpp apps/desktop/src/WorkspaceReferenceController.h
    apps/desktop/src/WorkspaceResourceReader.cpp apps/desktop/src/WorkspaceResourceReader.h
    apps/desktop/src/WorkspaceImagePreview.cpp apps/desktop/src/WorkspaceImagePreview.h
    apps/desktop/src/ImageAdmission.cpp apps/desktop/src/ImageAdmission.h
    apps/desktop/src/ImageImport.cpp apps/desktop/src/ImageImport.h
    apps/desktop/src/VisualTokens.cpp apps/desktop/src/VisualTokens.h
    apps/desktop/src/InputModality.cpp apps/desktop/src/InputModality.h
    apps/desktop/src/GuiRuntime.cpp apps/desktop/src/GuiRuntime.h
    apps/desktop/src/QuickAccessibleRoot.cpp apps/desktop/src/QuickAccessibleRoot.h
    apps/desktop/src/AdmissionWindow.cpp apps/desktop/src/AdmissionWindow.h)
target_include_directories(xdnote_desktop PUBLIC apps/desktop/src)
if(UNIX)
    add_dependencies(xdnote_desktop xdnote_resource_helper)
endif()
target_link_libraries(xdnote_desktop PUBLIC xdnote_scintilla xdnote_markdown
    Qt6::Widgets Qt6::Qml Qt6::Quick Qt6::QuickWidgets Qt6::QuickControls2 Qt6::WebEngineWidgets Qt6::Concurrent Qt6::Network Qt6::Svg)
if(WIN32)
    target_link_libraries(xdnote_desktop PUBLIC ws2_32 userenv bcrypt ntdll advapi32 dwmapi)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(xdnote_desktop PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
endif()
file(GLOB XDNOTE_UI_ICONS CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/design/assets/ui-v2/icons/svg/*.svg")
qt_add_resources(xdnote_desktop xdnote_assets PREFIX /xdnote BASE "${PROJECT_SOURCE_DIR}" FILES
    docs/visual/tokens.json apps/desktop/ui/Chrome.qml apps/desktop/ui/SharedButton.qml design/assets/ui-v2/brand/logo-light.svg design/assets/ui-v2/brand/logo-dark.svg design/assets/ui-v2/brand/app-icon.svg ${XDNOTE_UI_ICONS})

qt_add_executable(xdnote WIN32 apps/desktop/main.cpp)
target_link_libraries(xdnote PRIVATE xdnote_desktop)

add_executable(xdnote_admission apps/desktop/admission_main.cpp)
set_target_properties(xdnote_admission PROPERTIES OUTPUT_NAME xdnote-admission)
target_link_libraries(xdnote_admission PRIVATE xdnote_desktop)
if(MSVC)
    foreach(target xdnote_scintilla xdnote_desktop xdnote_admission xdnote)
        target_compile_options(${target} PRIVATE /utf-8 /EHsc)
    endforeach()
endif()

if(BUILD_TESTING)
    find_package(Qt6 ${XDNOTE_QT_VERSION} EXACT REQUIRED COMPONENTS Test)
    if(WIN32)
        configure_file(apps/desktop/tests/windows_uia_client.ps1 "${CMAKE_BINARY_DIR}/windows_uia_client.ps1" COPYONLY)
    endif()
    add_executable(xdnote_desktop_tests apps/desktop/tests/desktop_tests.cpp)
    target_link_libraries(xdnote_desktop_tests PRIVATE xdnote_desktop Qt6::Test)
    if(MSVC)
        target_compile_options(xdnote_desktop_tests PRIVATE /utf-8 /EHsc)
    endif()
    add_test(NAME desktop_admission COMMAND xdnote_desktop_tests
        -o -,txt -o "${CMAKE_BINARY_DIR}/desktop-qtest.txt,txt")
    set_tests_properties(desktop_admission PROPERTIES TIMEOUT 180)
    add_executable(xdnote_file_tests apps/desktop/tests/file_workflow_tests.cpp)
    target_link_libraries(xdnote_file_tests PRIVATE xdnote_desktop Qt6::Test)
    if(MSVC)
        target_compile_options(xdnote_file_tests PRIVATE /utf-8 /EHsc)
    endif()
    add_test(NAME file_workflow COMMAND xdnote_file_tests -o -,txt -o "${CMAKE_BINARY_DIR}/file-qtest.txt,txt")
    set_tests_properties(file_workflow PROPERTIES TIMEOUT 180)
    add_executable(xdnote_visual_tests apps/desktop/tests/standalone_visual_tests.cpp)
    target_link_libraries(xdnote_visual_tests PRIVATE xdnote_desktop Qt6::Test)
    if(MSVC)
        target_compile_options(xdnote_visual_tests PRIVATE /utf-8 /EHsc)
    endif()
    add_test(NAME standalone_visual COMMAND xdnote_visual_tests -o -,txt -o "${CMAKE_BINARY_DIR}/visual-qtest.txt,txt")
    set_tests_properties(standalone_visual PROPERTIES TIMEOUT 240)
    add_executable(xdnote_geometry_tests apps/desktop/tests/window_geometry_tests.cpp)
    target_link_libraries(xdnote_geometry_tests PRIVATE xdnote_desktop Qt6::Test)
    if(APPLE)
        target_sources(xdnote_geometry_tests PRIVATE apps/desktop/tests/MacWindowGeometryProbe.mm)
        find_library(XDNOTE_APPKIT_FRAMEWORK AppKit REQUIRED)
        target_link_libraries(xdnote_geometry_tests PRIVATE "${XDNOTE_APPKIT_FRAMEWORK}")
    endif()
    if(MSVC)
        target_compile_options(xdnote_geometry_tests PRIVATE /utf-8 /EHsc)
    endif()
    add_test(NAME window_geometry COMMAND xdnote_geometry_tests -o -,txt -o "${CMAKE_BINARY_DIR}/geometry-qtest.txt,txt")
    set_tests_properties(window_geometry PROPERTIES TIMEOUT 60)
    add_executable(xdnote_workspace_tests apps/desktop/tests/workspace_tests.cpp)
    target_link_libraries(xdnote_workspace_tests PRIVATE xdnote_desktop Qt6::Test)
    if(MSVC)
        target_compile_options(xdnote_workspace_tests PRIVATE /utf-8 /EHsc)
    endif()
    add_test(NAME workspace_workflow COMMAND xdnote_workspace_tests -o -,txt -o "${CMAKE_BINARY_DIR}/workspace-qtest.txt,txt")
    set_tests_properties(workspace_workflow PROPERTIES TIMEOUT 180)
endif()
