# Bakes assets/system32/logonui.exe into XPLogin.assets at build time.
#
# This is what makes the out-of-the-box promise work: the pack is produced by
# the build, embedded into XPLogin10-Setup.exe alongside the binaries, and
# deployed next to the DLL. Nobody installing the product ever sees a step
# about "supply your XP files" - that happened once, here, on the build machine.
#
# When logonui.exe is absent the pack is simply not produced. Everything still
# builds and the renderer falls back to drawing primitives, so a contributor
# without the reference material is not blocked.

set(XPLOGIN_LOGONUI "${CMAKE_SOURCE_DIR}/assets/system32/logonui.exe")
# The welcome screen is logonui.exe; the "Turn off computer" dialog it opens is
# a separate modal owned by msgina.dll, so its artwork lives there instead.
# Optional on its own - without it the dialog is drawn from primitives.
set(XPLOGIN_MSGINA "${CMAKE_SOURCE_DIR}/assets/system32/msgina.dll")
set(XPLOGIN_ASSET_PACK "${CMAKE_BINARY_DIR}/XPLogin.assets")

if(EXISTS "${XPLOGIN_LOGONUI}")
    set(XPLOGIN_HAVE_ASSETS TRUE)

    set(XPLOGIN_BAKE_ARGS "${XPLOGIN_LOGONUI}")
    set(XPLOGIN_BAKE_DEPS xp-bake "${XPLOGIN_LOGONUI}")
    if(EXISTS "${XPLOGIN_MSGINA}")
        list(APPEND XPLOGIN_BAKE_ARGS --msgina "${XPLOGIN_MSGINA}")
        list(APPEND XPLOGIN_BAKE_DEPS "${XPLOGIN_MSGINA}")
    endif()

    add_custom_command(
        OUTPUT "${XPLOGIN_ASSET_PACK}"
        COMMAND xp-bake ${XPLOGIN_BAKE_ARGS} -o "${XPLOGIN_ASSET_PACK}"
        DEPENDS ${XPLOGIN_BAKE_DEPS}
        COMMENT "Baking the XP welcome screen artwork into XPLogin.assets"
        VERBATIM)

    add_custom_target(xplogin-assets ALL DEPENDS "${XPLOGIN_ASSET_PACK}")

    message(STATUS "  XP artwork      : ${XPLOGIN_LOGONUI}")
else()
    set(XPLOGIN_HAVE_ASSETS FALSE)
    message(STATUS "  XP artwork      : not found, the renderer will draw "
                   "primitives instead")
    message(STATUS "                    (drop logonui.exe into assets/system32/)")
endif()
