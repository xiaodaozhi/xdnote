cmake_minimum_required(VERSION 3.21)
project(xdnote_qt_document_spike LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Network Qml Quick Test Widgets)

add_executable(qt_open_intent_sender
    src/spike_ipc.cpp
    src/spike_ipc.h
    ipc/send_open.cpp
)
target_compile_features(qt_open_intent_sender PRIVATE cxx_std_17)
target_include_directories(qt_open_intent_sender PRIVATE src)
target_link_libraries(qt_open_intent_sender PRIVATE Qt6::Core Qt6::Network)

add_executable(qt_fixture_writer tests/fixture_writer.cpp)
target_compile_features(qt_fixture_writer PRIVATE cxx_std_17)
target_link_libraries(qt_fixture_writer PRIVATE Qt6::Core)

if(WIN32)
    add_executable(qt_uia_client tests/uia_client.cpp)
    target_compile_features(qt_uia_client PRIVATE cxx_std_17)
    target_link_libraries(qt_uia_client PRIVATE Ole32 OleAut32 UIAutomationCore)
endif()

add_executable(qt_document_spike_tests
    src/spike_document.cpp
    src/spike_document.h
    src/spike_handoff.cpp
    src/spike_handoff.h
    src/spike_ipc.cpp
    src/spike_ipc.h
    tests/file_identity_probe.cpp
    tests/file_identity_probe.h
    tests/image_reference_probe.cpp
    tests/image_reference_probe.h
    tests/displaced_probe.cpp
    tests/displaced_probe.h
    tests/qt_spike_test.cpp
)
add_dependencies(qt_document_spike_tests qt_open_intent_sender qt_fixture_writer)
if(WIN32)
    add_dependencies(qt_document_spike_tests qt_uia_client)
endif()
target_compile_features(qt_document_spike_tests PRIVATE cxx_std_17)
target_include_directories(qt_document_spike_tests PRIVATE src)
target_link_libraries(qt_document_spike_tests PRIVATE
    Qt6::Core Qt6::Gui Qt6::Network Qt6::Qml Qt6::Quick Qt6::Test
)
qt_add_resources(qt_document_spike_tests markdown_fixture
    PREFIX "/"
    FILES tests/fixtures/markdown-core.md
)

add_executable(qt_widget_accessibility_tests tests/widget_accessibility_test.cpp)
target_compile_features(qt_widget_accessibility_tests PRIVATE cxx_std_17)
target_link_libraries(qt_widget_accessibility_tests PRIVATE Qt6::Core Qt6::Gui Qt6::Test Qt6::Widgets)
if(MSVC)
    target_compile_options(qt_open_intent_sender PRIVATE /utf-8)
    target_compile_options(qt_fixture_writer PRIVATE /utf-8)
    target_compile_options(qt_document_spike_tests PRIVATE /utf-8)
    target_compile_options(qt_widget_accessibility_tests PRIVATE /utf-8)
endif()

add_executable(qt_document_handoff_manual
    src/spike_handoff.cpp
    src/spike_handoff.h
    manual/handoff.cpp
)
target_compile_features(qt_document_handoff_manual PRIVATE cxx_std_17)
target_include_directories(qt_document_handoff_manual PRIVATE src)
target_link_libraries(qt_document_handoff_manual PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick)
qt_add_resources(qt_document_handoff_manual handoff_manual_qml
    PREFIX "/"
    FILES manual/handoff.qml
)
if(MSVC)
    target_compile_options(qt_document_handoff_manual PRIVATE /utf-8)
endif()

add_executable(qt_document_spike_benchmark
    src/spike_document.cpp
    src/spike_document.h
    benchmark/benchmark.cpp
)
target_compile_features(qt_document_spike_benchmark PRIVATE cxx_std_17)
target_include_directories(qt_document_spike_benchmark PRIVATE src)
target_link_libraries(qt_document_spike_benchmark PRIVATE Qt6::Core Qt6::Gui Qt6::Quick)
if(MSVC)
    target_compile_options(qt_document_spike_benchmark PRIVATE /utf-8)
endif()
if(WIN32)
    target_link_libraries(qt_document_spike_benchmark PRIVATE Psapi)
endif()

include(CTest)
add_test(NAME qt_document_spike COMMAND qt_document_spike_tests)
add_test(NAME qt_document_handoff_smoke COMMAND qt_document_handoff_manual --smoke-test)
add_test(NAME qt_widget_accessibility COMMAND qt_widget_accessibility_tests)
set_tests_properties(qt_document_spike qt_document_handoff_smoke qt_widget_accessibility PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
)
