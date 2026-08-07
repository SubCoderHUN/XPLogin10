# Picks XP's own sounds out of assets/Media for the setup to embed.
#
# Same bargain as the artwork: if the reference material is there, the product
# ships with sound and nobody installing it is asked to supply anything. If it
# is not, the setup simply carries no sounds and the screen is silent, which is
# what the [sounds] section already handles.
#
# The files are referenced in place rather than copied - the resource compiler
# reads them straight from assets/Media at link time.

set(XPLOGIN_MEDIA "${CMAKE_SOURCE_DIR}/assets/Media")

# Payload id -> the file in assets/Media it comes from. The names on the left
# are what lands in the install directory; see PayloadId in src/setup/Payload.h.
set(XPLOGIN_SOUND_IDS      108 109 110 111)
set(XPLOGIN_SOUND_SOURCES
    "Windows XP Logon Sound.wav"
    "Windows XP Logoff Sound.wav"
    "Windows XP Error.wav"
    "Windows XP Shutdown.wav")

set(XPLOGIN_PAYLOAD_SOUNDS "")
set(XPLOGIN_HAVE_SOUNDS FALSE)

list(LENGTH XPLOGIN_SOUND_IDS _xplogin_sound_count)
math(EXPR _xplogin_sound_last "${_xplogin_sound_count} - 1")

foreach(_i RANGE ${_xplogin_sound_last})
    list(GET XPLOGIN_SOUND_IDS ${_i} _id)
    list(GET XPLOGIN_SOUND_SOURCES ${_i} _name)
    set(_path "${XPLOGIN_MEDIA}/${_name}")
    if(EXISTS "${_path}")
        # RC wants backslashes escaped; forward slashes work everywhere and
        # sidestep the whole question, spaces in the name included.
        string(APPEND XPLOGIN_PAYLOAD_SOUNDS "${_id} RCDATA \"${_path}\"\n")
        set(XPLOGIN_HAVE_SOUNDS TRUE)
    endif()
endforeach()

if(XPLOGIN_HAVE_SOUNDS)
    message(STATUS "  XP sounds       : ${XPLOGIN_MEDIA}")
else()
    message(STATUS "  XP sounds       : not found, the screen will be silent")
    message(STATUS "                    (drop the .wav files into assets/Media/)")
    set(XPLOGIN_PAYLOAD_SOUNDS "// no sounds in this build\n")
endif()
