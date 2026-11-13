set(XDNOTE_PREVIEW_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
add_library(xdnote_native_preview STATIC
    "${XDNOTE_PREVIEW_ROOT}/apps/desktop/src/NativePreviewContent.cpp"
    "${XDNOTE_PREVIEW_ROOT}/apps/desktop/src/NativePreviewContent.h"
    "${XDNOTE_PREVIEW_ROOT}/apps/desktop/src/NativePreviewPlaceholders.h"
    "${XDNOTE_PREVIEW_ROOT}/apps/desktop/src/NativePreviewView.cpp"
    "${XDNOTE_PREVIEW_ROOT}/apps/desktop/src/NativePreviewView.h")
target_link_libraries(xdnote_native_preview PUBLIC xdnote_native_model Qt6::Widgets Qt6::Concurrent)
if(MSVC)
    target_compile_options(xdnote_native_preview PRIVATE /utf-8 /EHsc)
endif()
if(BUILD_TESTING)
    # Reuse the real shared controls in the isolated admission executable. This
    # target does not link the product's legacy preview or duplicate UI at runtime.
    add_library(xdnote_native_preview_test_ui STATIC
        "${XDNOTE_PREVIEW_ROOT}/apps/desktop/src/Appearance.cpp"
        "${XDNOTE_PREVIEW_ROOT}/apps/desktop/src/Preferences.cpp"
        "${XDNOTE_PREVIEW_ROOT}/apps/desktop/src/Shortcuts.cpp"
        "${XDNOTE_PREVIEW_ROOT}/apps/desktop/src/Preferences.h"
        "${XDNOTE_PREVIEW_ROOT}/apps/desktop/src/UiFonts.cpp"
        "${XDNOTE_PREVIEW_ROOT}/apps/desktop/src/VisualTokens.cpp"
        "${XDNOTE_PREVIEW_ROOT}/apps/desktop/src/InputModality.cpp"
        "${XDNOTE_PREVIEW_ROOT}/apps/desktop/src/SharedWidgets.cpp")
    target_include_directories(xdnote_native_preview_test_ui PUBLIC "${XDNOTE_PREVIEW_ROOT}/apps/desktop/src")
    target_compile_definitions(xdnote_native_preview_test_ui PRIVATE XDNOTE_RESOURCE_NAME=xdnote_native_assets)
    target_link_libraries(xdnote_native_preview_test_ui PUBLIC Qt6::Widgets Qt6::Svg)
    if(WIN32)
        target_link_libraries(xdnote_native_preview_test_ui PRIVATE dwmapi)
    endif()
    file(GLOB XDNOTE_NATIVE_ICONS CONFIGURE_DEPENDS "${XDNOTE_PREVIEW_ROOT}/design/assets/ui-v2/icons/svg/*.svg")
    qt_add_resources(xdnote_native_preview_test_ui xdnote_native_assets PREFIX /xdnote
        BASE "${XDNOTE_PREVIEW_ROOT}" FILES "${XDNOTE_PREVIEW_ROOT}/docs/visual/tokens.json" ${XDNOTE_NATIVE_ICONS})
    add_executable(xdnote_native_preview_tests "${XDNOTE_PREVIEW_ROOT}/apps/desktop/tests/native_preview_view_tests.cpp")
    target_link_libraries(xdnote_native_preview_tests PRIVATE xdnote_native_preview xdnote_native_preview_test_ui Qt6::Test)
    if(MSVC)
        target_compile_options(xdnote_native_preview_test_ui PRIVATE /utf-8 /EHsc)
        target_compile_options(xdnote_native_preview_tests PRIVATE /utf-8 /EHsc)
    endif()
    add_test(NAME native_preview_view COMMAND xdnote_native_preview_tests
        -o -,txt -o "${CMAKE_BINARY_DIR}/native-preview-qtest.txt,txt")
    set_tests_properties(native_preview_view PROPERTIES TIMEOUT 180)
endif()
