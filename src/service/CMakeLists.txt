# XPLoginWatchdog.exe - session-0 recovery service.

add_executable(XPLoginWatchdog WatchdogService.cpp)

target_link_libraries(XPLoginWatchdog PRIVATE
    xplogin_core
    advapi32
    wtsapi32   # session change notifications
)

set_target_properties(XPLoginWatchdog PROPERTIES OUTPUT_NAME "XPLoginWatchdog")

if(MSVC)
    target_link_options(XPLoginWatchdog PRIVATE /SUBSYSTEM:CONSOLE)
endif()
