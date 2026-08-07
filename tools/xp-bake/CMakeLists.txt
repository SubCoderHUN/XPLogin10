# xp-bake - decodes logonui.exe's bitmaps into the runtime asset pack.

add_executable(xp-bake main.cpp)
target_link_libraries(xp-bake PRIVATE xplogin_assets)
set_target_properties(xp-bake PROPERTIES OUTPUT_NAME "xp-bake")
