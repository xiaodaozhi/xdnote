cmake_minimum_required(VERSION 3.21)
project(xdnote_scintilla_buffer_spike LANGUAGES CXX)

get_filename_component(XDNOTE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(SCINTILLA_SOURCE_DIR "${XDNOTE_ROOT}/build/scintilla-5.6.7/scintilla" CACHE PATH
    "Scintilla 5.6.7 source directory prepared explicitly before configuring")
if(NOT EXISTS "${SCINTILLA_SOURCE_DIR}/version.txt")
    message(FATAL_ERROR "Scintilla source missing; see experiments/scintilla-buffer-spike/README.md")
endif()
file(READ "${SCINTILLA_SOURCE_DIR}/version.txt" SCINTILLA_VERSION)
string(STRIP "${SCINTILLA_VERSION}" SCINTILLA_VERSION)
if(NOT SCINTILLA_VERSION STREQUAL "567")
    message(FATAL_ERROR "This experiment expects Scintilla 5.6.7")
endif()

add_executable(scintilla_buffer_spike
    tests/buffer_test.cpp
    "${SCINTILLA_SOURCE_DIR}/src/CellBuffer.cxx"
    "${SCINTILLA_SOURCE_DIR}/src/ChangeHistory.cxx"
    "${SCINTILLA_SOURCE_DIR}/src/UndoHistory.cxx"
    "${SCINTILLA_SOURCE_DIR}/src/RunStyles.cxx"
    "${SCINTILLA_SOURCE_DIR}/src/PerLine.cxx"
    "${SCINTILLA_SOURCE_DIR}/src/UniConversion.cxx"
)
target_compile_features(scintilla_buffer_spike PRIVATE cxx_std_17)
set_target_properties(scintilla_buffer_spike PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(scintilla_buffer_spike PRIVATE
    "${SCINTILLA_SOURCE_DIR}/include" "${SCINTILLA_SOURCE_DIR}/src")
if(MSVC)
    target_compile_options(scintilla_buffer_spike PRIVATE /utf-8 /EHsc)
endif()

include(CTest)
add_test(NAME scintilla_buffer_spike COMMAND scintilla_buffer_spike)

option(XDNOTE_SCINTILLA_WIDGET "Build the separate Qt widget experiment" OFF)
if(XDNOTE_SCINTILLA_WIDGET)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Core5Compat)
    set(SCINTILLA_QT_DIR "${SCINTILLA_SOURCE_DIR}/qt/ScintillaEditBase")
    set(SCINTILLA_CORE_NAMES
        AutoComplete CallTip CaseConvert CaseFolder CellBuffer ChangeHistory
        CharacterCategoryMap CharacterType CharClassify ContractionState DBCS Decoration
        Document EditModel Editor EditView Geometry Indicator KeyMap LineMarker MarginView
        PerLine PositionCache RESearch RunStyles ScintillaBase Selection Style UndoHistory
        UniConversion UniqueString ViewStyle XPM)
    set(SCINTILLA_CORE_SOURCES)
    foreach(name IN LISTS SCINTILLA_CORE_NAMES)
        list(APPEND SCINTILLA_CORE_SOURCES "${SCINTILLA_SOURCE_DIR}/src/${name}.cxx")
    endforeach()
    add_library(scintilla_qt_fixture STATIC
        ${SCINTILLA_CORE_SOURCES}
        "${SCINTILLA_QT_DIR}/PlatQt.cpp"
        "${SCINTILLA_QT_DIR}/ScintillaQt.cpp"
        "${SCINTILLA_QT_DIR}/ScintillaEditBase.cpp"
        "${SCINTILLA_QT_DIR}/ScintillaEditBase.h"
        widget/handoff.cpp
        widget/handoff.h
    )
    set_target_properties(scintilla_qt_fixture PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
    target_compile_features(scintilla_qt_fixture PUBLIC cxx_std_17)
    target_compile_definitions(scintilla_qt_fixture PUBLIC SCINTILLA_QT=1 EXPORT_IMPORT_API=)
    target_include_directories(scintilla_qt_fixture PUBLIC
        "${SCINTILLA_SOURCE_DIR}/include" "${SCINTILLA_SOURCE_DIR}/src" "${SCINTILLA_QT_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/widget")
    target_link_libraries(scintilla_qt_fixture PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Core5Compat)
    if(MSVC)
        target_compile_options(scintilla_qt_fixture PRIVATE /utf-8 /EHsc)
    endif()

    add_executable(scintilla_widget_spike tests/widget_test.cpp)
    target_link_libraries(scintilla_widget_spike PRIVATE scintilla_qt_fixture)
    add_executable(scintilla_widget_manual widget/manual.cpp)
    target_link_libraries(scintilla_widget_manual PRIVATE scintilla_qt_fixture)
    if(MSVC)
        target_compile_options(scintilla_widget_spike PRIVATE /utf-8 /EHsc)
        target_compile_options(scintilla_widget_manual PRIVATE /utf-8 /EHsc)
    endif()
    add_test(NAME scintilla_widget_spike COMMAND scintilla_widget_spike)
    set_tests_properties(scintilla_widget_spike PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
