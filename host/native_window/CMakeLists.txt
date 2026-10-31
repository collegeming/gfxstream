

if (APPLE)
    set(gfxstream_host_native_window_platform_sources
        native_sub_window_cocoa.mm)
elseif (WIN32)
    set(gfxstream_host_native_window_platform_sources
        native_sub_window_win32.cpp)
elseif (QNX)
    set(gfxstream_host_native_window_platform_sources
        native_sub_window_qnx.cpp)
elseif (ANDROID)
    # Android has no X11/desktop sub-windows; the display backend (Android
    # display service / VNC / KMS) is handled elsewhere.
    set(gfxstream_host_native_window_platform_sources
        native_sub_window_android.cpp)
else()
    set(gfxstream_host_native_window_platform_sources
        native_sub_window_x11.cpp)
endif()


add_library(
    gfxstream_host_native_window
    STATIC
    ${gfxstream_host_native_window_platform_sources})

target_include_directories(
    gfxstream_host_native_window
    PUBLIC
    include
    )

target_link_libraries(
    gfxstream_host_native_window
    PRIVATE
    gfxstream_common_base
    gfxstream_backend_headers
    gfxstream_host_decoder_common
    )
