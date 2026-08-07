# xplogin_assets - reading Microsoft's original resources out of PE files.
#
# Portable on purpose: extraction runs on the build machine, and parsing an
# untrusted binary with a plain byte reader beats handing it to LoadLibrary.

add_library(xplogin_assets STATIC
    src/PeResources.cpp
    src/BmpDecoder.cpp
    src/AssetPack.cpp
    src/PngWriter.cpp
)

target_include_directories(xplogin_assets PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

set_target_properties(xplogin_assets PROPERTIES POSITION_INDEPENDENT_CODE ON)
