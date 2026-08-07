# xplogin_core - platform-independent logon logic.
#
# This target must build and its tests must pass on any host, which is what
# keeps the OS-specific code confined to the win32/ subdirectory.

set(XPLOGIN_CORE_SOURCES
    src/StringUtil.cpp
    src/SigninOptions.cpp
    src/ShellStatus.cpp
    src/CredentialPacker.cpp
    src/LogonStateMachine.cpp
    src/UserDirectory.cpp
    src/HealthMonitor.cpp
    src/Config.cpp
    src/Logging.cpp
    src/RegistrationPlan.cpp
    src/DeploymentPlan.cpp
    src/LogonController.cpp
    src/AuthStatusMapping.cpp
)

# The clock and the registry-backed state store live in Win32Environment.cpp
# alongside the registry helpers they are built on.
set(XPLOGIN_CORE_WIN32_SOURCES
    src/win32/Win32Environment.cpp
    src/win32/Win32Authenticator.cpp
    src/win32/Win32UserEnumerator.cpp
    src/win32/Win32SessionManager.cpp
    src/win32/Win32PowerController.cpp
    src/win32/Win32SoundPlayer.cpp
    src/win32/Win32SystemRestore.cpp
)

add_library(xplogin_core STATIC ${XPLOGIN_CORE_SOURCES})

target_include_directories(xplogin_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

set_target_properties(xplogin_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(WIN32)
    target_sources(xplogin_core PRIVATE ${XPLOGIN_CORE_WIN32_SOURCES})
    target_link_libraries(xplogin_core PUBLIC
        netapi32      # NetUserEnum / NetUserGetInfo
        wtsapi32      # session enumeration
        userenv       # CreateEnvironmentBlock for explorer.exe
        secur32       # LsaConnectUntrusted / LsaLookupAuthenticationPackage
        credui        # CredPackAuthenticationBuffer / CredProtect
        advapi32
        winmm         # PlaySound
        powrprof      # IsPwrSuspendAllowed / SetSuspendState
        shlwapi
        ole32         # CoInitializeEx around SRSetRestorePointW
    )
    target_compile_definitions(xplogin_core PUBLIC XPLOGIN_WIN32=1)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(xplogin_core PUBLIC Threads::Threads)
endif()
