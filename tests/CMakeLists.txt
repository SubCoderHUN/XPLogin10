# The automated test suite.
#
# One binary, registered with CTest twice: once as a whole (so `ctest` is a
# single pass/fail) and once per suite (so a failure names the area).

set(XPLOGIN_TEST_SOURCES
    framework/xptest_main.cpp
    unit/test_credential_packer.cpp
    unit/test_authentication.cpp
    unit/test_state_machine.cpp
    unit/test_health_monitor.cpp
    unit/test_registration_plan.cpp
    unit/test_deployment_plan.cpp
    unit/test_pe_resources.cpp
    unit/test_assets.cpp
    unit/test_png_writer.cpp
    unit/test_ui_layout.cpp
    unit/test_config.cpp
    unit/test_signin_options.cpp
    unit/test_shell_status.cpp
    integration/test_user_enumeration.cpp
    integration/test_session_simulation.cpp
    integration/test_logon_flow.cpp
)

add_executable(xplogin_tests ${XPLOGIN_TEST_SOURCES})

target_include_directories(xplogin_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/framework
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks
)

# So a test can read the files the setup actually ships, rather than a copy of
# them written out in the test. The shipped XPLogin.theme.ini once carried a
# whole set of stale measurements that overrode the real ones at run time, and
# nothing noticed because every test built its ini inline.
target_compile_definitions(xplogin_tests PRIVATE
    XPLOGIN_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

target_link_libraries(xplogin_tests PRIVATE xplogin_core xplogin_ui xplogin_assets)

if(MSVC)
    # The tests deliberately build wide-string literals with non-ASCII content.
    target_compile_options(xplogin_tests PRIVATE /utf-8)
endif()

add_test(NAME xplogin_all COMMAND xplogin_tests
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# Per-suite entries make a CTest run readable at a glance.
set(XPLOGIN_TEST_SUITES
    CredentialPacker
    Authentication
    AuthStatusMapping
    StateMachine
    HealthMonitor
    RegistrationPlan
    Deployment
    ProgramsAndFeatures
    SetupPlan
    SystemRestore
    PeResources
    BmpDecoder
    AssetPack
    Assets
    PngWriter
    Theme
    Layout
    HitTest
    Animation
    IniFile
    AppConfig
    SigninOptions
    UserEnumeration
    SessionSimulation
    LogonFlow
)

foreach(suite ${XPLOGIN_TEST_SUITES})
    add_test(NAME suite.${suite}
             COMMAND xplogin_tests --filter ${suite}.
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endforeach()
