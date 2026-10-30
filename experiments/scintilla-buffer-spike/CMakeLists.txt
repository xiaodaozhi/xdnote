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
