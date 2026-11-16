# SpeedBreaker extras (GPL-3.0-or-later): the AppImage's launcher and the
# AI texture pack tool. Built with -DSB_EXTRAS=ON; plain x86-64 (no -march),
# so they run on any PC and can say when the game can't.
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

add_executable(sb-texpack texpack/texpack.cpp texpack/ea_compress.cpp texpack/image_io.cpp)
target_compile_features(sb-texpack PRIVATE cxx_std_20)
target_include_directories(sb-texpack SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/tools/XenonRecomp/thirdparty/xxHash)
target_link_libraries(sb-texpack PRIVATE ZLIB::ZLIB Threads::Threads)

add_executable(sb-launcher launcher/main.cpp launcher/gfx.cpp launcher/hardware.cpp launcher/process.cpp
    launcher/settings_file.cpp launcher/content.cpp launcher/xiso.cpp ${CMAKE_SOURCE_DIR}/runtime/install/sha256.cpp)
target_compile_features(sb-launcher PRIVATE cxx_std_20)
# user/key_names.h: the game's own key names for settings.toml.
target_include_directories(sb-launcher PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
target_link_libraries(sb-launcher PRIVATE imgui Threads::Threads)
