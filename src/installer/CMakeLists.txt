# Shared installer machinery, used by both xplogin-install.exe (the advanced
# and recovery CLI) and XPLogin10-Setup.exe (what a user double-clicks).

add_library(xplogin_installsupport STATIC
    RegistryExecutor.cpp
    FileExecutor.cpp
)

target_include_directories(xplogin_installsupport PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(xplogin_installsupport PUBLIC
    xplogin_core
    advapi32
)

# xplogin-install.exe - registration, removal and the recovery artefacts,
# without file deployment. Kept because it is the tool you reach for from Safe
# Mode when the setup binary is not what you want to run.
add_executable(xplogin-install
    main.cpp
    installer.manifest      # requireAdministrator
)

target_link_libraries(xplogin-install PRIVATE
    xplogin_installsupport
    shlwapi
)

set_target_properties(xplogin-install PROPERTIES OUTPUT_NAME "xplogin-install")

if(MSVC)
    # /MANIFESTUAC:NO is coupled to installer.manifest above and cannot be
    # removed on its own. The linker embeds its own trustInfo fragment by
    # default - asInvoker - and mt.exe then refuses to merge that with the
    # requireAdministrator in our file:
    #
    #   manifest authoring error c1010001: Values of attribute "level" not
    #   equal in different manifest snippets
    #
    # This turns the linker's fragment off so the manifest file is the only
    # source of the elevation request. It has to be the file rather than
    # /MANIFESTUAC:level=... because the same manifest also carries the
    # supportedOS compatibility GUID.
    target_link_options(xplogin-install PRIVATE /SUBSYSTEM:CONSOLE /MANIFESTUAC:NO)
endif()
