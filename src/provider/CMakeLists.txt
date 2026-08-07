# XPLoginProvider.dll - the credential provider Windows loads into LogonUI.exe.

add_library(XPLoginProvider SHARED
    dllmain.cpp
    Guids.cpp
    XPProvider.cpp
    XPCredential.cpp
    XPFilter.cpp
    XPLoginProvider.def
    provider.rc
)

target_include_directories(XPLoginProvider PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(XPLoginProvider PRIVATE
    xplogin_core
    xplogin_ui
    xplogin_ui_win32
    ole32
    oleaut32
    credui
    secur32
    propsys      # PKEY_Identity_* used by ICredentialProviderUserArray
    advapi32
    shlwapi
)

set_target_properties(XPLoginProvider PROPERTIES
    OUTPUT_NAME "XPLoginProvider"
    PREFIX ""
)

if(MSVC)
    target_link_options(XPLoginProvider PRIVATE
        /DEF:${CMAKE_CURRENT_SOURCE_DIR}/XPLoginProvider.def
        # The DLL runs inside LogonUI: make exploitation of any bug here as
        # unpleasant as the toolchain allows.
        /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /GUARD:CF
    )
    target_compile_options(XPLoginProvider PRIVATE /GS /sdl)
endif()
