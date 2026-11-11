# Shared by the desktop build and the GUI-free admission target.
set(XDNOTE_MODEL_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
add_library(xdnote_native_model STATIC
    "${XDNOTE_MODEL_ROOT}/apps/desktop/src/NativePreviewModel.cpp"
    "${XDNOTE_MODEL_ROOT}/apps/desktop/src/NativePreviewModel.h")
target_include_directories(xdnote_native_model PUBLIC "${XDNOTE_MODEL_ROOT}/apps/desktop/src")
target_link_libraries(xdnote_native_model PUBLIC Qt6::Core PRIVATE xdnote_markdown)
if(WIN32)
    target_link_libraries(xdnote_native_model PRIVATE ws2_32 userenv bcrypt ntdll advapi32)
endif()
if(MSVC)
    target_compile_options(xdnote_native_model PRIVATE /utf-8 /EHsc)
endif()
if(BUILD_TESTING)
    find_package(Qt6 ${XDNOTE_QT_VERSION} EXACT REQUIRED COMPONENTS Test)
    add_executable(xdnote_native_model_tests "${XDNOTE_MODEL_ROOT}/apps/desktop/tests/native_preview_model_tests.cpp")
    target_link_libraries(xdnote_native_model_tests PRIVATE xdnote_native_model Qt6::Test)
    if(MSVC)
        target_compile_options(xdnote_native_model_tests PRIVATE /utf-8 /EHsc)
    endif()
    add_test(NAME native_preview_model COMMAND xdnote_native_model_tests
        -o -,txt -o "${CMAKE_BINARY_DIR}/native-model-qtest.txt,txt")
    set_tests_properties(native_preview_model PROPERTIES TIMEOUT 120)
endif()
