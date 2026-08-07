# xplogin_ui - the XP welcome screen.
#
# Split in two on purpose: the model (theme, geometry, hit testing, animation)
# is portable and unit tested; the renderer is GDI+ and Windows only.

add_library(xplogin_ui STATIC
    src/XpTheme.cpp
    src/XpLayout.cpp
)

target_include_directories(xplogin_ui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(xplogin_ui PUBLIC xplogin_core)
set_target_properties(xplogin_ui PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(WIN32)
    # The "Turn off computer" modal, the avatar cache and the GDI+ lifetime
    # helper all live inside XpRenderer.cpp - they are a few dozen lines each
    # and only ever used from there.
    add_library(xplogin_ui_win32 STATIC
        src/win32/XpRenderer.cpp
        src/win32/XpAssets.cpp
        src/win32/XpLogonWindow.cpp
    )
    target_include_directories(xplogin_ui_win32 PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(xplogin_ui_win32 PUBLIC
        xplogin_ui
        xplogin_assets  # the baked XP artwork
        gdiplus
        msimg32     # GradientFill / AlphaBlend
        user32
        gdi32
        shlwapi
    )
endif()
