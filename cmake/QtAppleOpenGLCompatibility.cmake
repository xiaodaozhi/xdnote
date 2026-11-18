if(APPLE AND TARGET WrapOpenGL::WrapOpenGL)
    set(XDNOTE_APPLE_SDK_NAME "${CMAKE_OSX_SYSROOT}")
    if(NOT XDNOTE_APPLE_SDK_NAME)
        set(XDNOTE_APPLE_SDK_NAME macosx)
    endif()
    execute_process(COMMAND /usr/bin/xcrun --sdk "${XDNOTE_APPLE_SDK_NAME}" --show-sdk-path
        OUTPUT_VARIABLE XDNOTE_APPLE_SDK OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    if(NOT EXISTS "${XDNOTE_APPLE_SDK}/System/Library/Frameworks/AGL.framework")
        # The fixed Qt SDK still lists AGL as an explicit public dependency.
        # Some newer SDKs omit the link stub. Match Qt's later OpenGL link
        # interface; the actual Qt runtime closure is validated separately.
        get_target_property(XDNOTE_OPENGL_LINKS WrapOpenGL::WrapOpenGL INTERFACE_LINK_LIBRARIES)
        list(FILTER XDNOTE_OPENGL_LINKS EXCLUDE REGEX "(^-framework AGL$|/AGL\\.framework(/.*)?$)")
        set_property(TARGET WrapOpenGL::WrapOpenGL PROPERTY INTERFACE_LINK_LIBRARIES "${XDNOTE_OPENGL_LINKS}")
        message(STATUS "Fixed Qt SDK: omit explicit AGL link absent from selected Apple SDK")
    endif()
endif()
