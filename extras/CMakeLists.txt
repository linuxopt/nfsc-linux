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
    launcher/settings_file.cpp launcher/content.cpp launcher/xiso.cpp launcher/updater.cpp launcher/icons.cpp
    texpack/image_io.cpp ${CMAKE_SOURCE_DIR}/runtime/install/sha256.cpp
    # The game's icons from default.xex (launcher/icons.h): XenonUtils' XDBF reader.
    ${CMAKE_SOURCE_DIR}/tools/XenonRecomp/XenonUtils/xdbf_wrapper.cpp)
# Its version and where its updates come from (the top-level CMakeLists.txt).
target_compile_definitions(sb-launcher PRIVATE NFSC_VERSION="${SB_VERSION_CARBON}" NFSC_GITHUB_REPO="${NFSC_GITHUB_REPO}")
target_compile_features(sb-launcher PRIVATE cxx_std_20)
# user/key_names.h: the game's own key names for settings.toml.
target_include_directories(sb-launcher PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
target_link_libraries(sb-launcher PRIVATE imgui XenonUtilsLite ZLIB::ZLIB Threads::Threads)
