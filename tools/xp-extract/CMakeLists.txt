# xp-extract - inventory and unpack a PE file's resources.

add_executable(xp-extract main.cpp)
target_link_libraries(xp-extract PRIVATE xplogin_assets)
set_target_properties(xp-extract PROPERTIES OUTPUT_NAME "xp-extract")
