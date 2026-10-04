# PC98PLAYER の SDL3 版（Windows / macOS / Linux）
#   cmake -S sdl -B build-sdl -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-sdl
# 上の階層の CMakeLists.txt（Windows 版）とは別のプロジェクトにしてある
cmake_minimum_required(VERSION 3.16)
project(PC98PLAYER_SDL CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(PC98_SDL_FREETYPE "Windows でも FreeType でフォントを描く" OFF)
option(PC98_SDL_STATIC "SDL3 を静的リンクする" ON)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
find_package(SDL3 REQUIRED CONFIG)

file(GLOB CORE ${ROOT}/core/*.cpp)
set(SRC ${CORE} main.cpp)
include_directories(${ROOT}/third_party/TinySoundFont)   # tsf.h（MidiSoundFont=）

set(YMFM ${ROOT}/third_party/ymfm/src)
if(EXISTS ${YMFM}/ymfm_opn.cpp)
  list(APPEND SRC ${YMFM}/ymfm_opn.cpp ${YMFM}/ymfm_adpcm.cpp ${YMFM}/ymfm_ssg.cpp)
  add_compile_definitions(GMPV3_WITH_YMFM)
  include_directories(${YMFM})
else()
  message(WARNING "third_party/ymfm がありません（git submodule update --init）。FM 音源なしでビルドします")
endif()

if(WIN32 AND NOT PC98_SDL_FREETYPE)
  list(APPEND SRC hostfont_gdi.cpp)
else()
  find_package(Freetype REQUIRED)
  list(APPEND SRC hostfont_ft.cpp)
  set(USE_FREETYPE ON)
endif()

if(WIN32)
  enable_language(RC)
  list(APPEND SRC ${ROOT}/win32/pc98player.rc)   # アイコンとバージョン情報
endif()

add_executable(PC98PLAYER_SDL WIN32 MACOSX_BUNDLE ${SRC})
if(APPLE)
  # PC98PLAYER.app（フォルダをアイコンに落として起動できる）
  set_target_properties(PC98PLAYER_SDL PROPERTIES
    OUTPUT_NAME PC98PLAYER
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
    MACOSX_BUNDLE_BUNDLE_NAME PC98PLAYER
    MACOSX_BUNDLE_GUI_IDENTIFIER pc98player.sdl
    MACOSX_BUNDLE_SHORT_VERSION_STRING 1.1.0
    MACOSX_BUNDLE_BUNDLE_VERSION 1.1.0)
endif()

if(PC98_SDL_STATIC AND TARGET SDL3::SDL3-static)
  target_link_libraries(PC98PLAYER_SDL PRIVATE SDL3::SDL3-static)
else()
  target_link_libraries(PC98PLAYER_SDL PRIVATE SDL3::SDL3)
endif()
if(USE_FREETYPE)
  target_link_libraries(PC98PLAYER_SDL PRIVATE Freetype::Freetype)
endif()

if(WIN32)
  target_compile_definitions(PC98PLAYER_SDL PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS NOMINMAX)
  target_link_libraries(PC98PLAYER_SDL PRIVATE winmm gdi32 user32 shell32 setupapi advapi32)
  if(MINGW)
    target_link_options(PC98PLAYER_SDL PRIVATE -municode -static)   # UNICODE なので SDL の入口は wWinMain
  endif()
else()
  # hostfs / hostfont_ft が Shift_JIS の変換に iconv を使う（glibc では libc に入っている）
  find_package(Iconv REQUIRED)
  target_link_libraries(PC98PLAYER_SDL PRIVATE Iconv::Iconv)
endif()
if(MSVC)
  target_compile_options(PC98PLAYER_SDL PRIVATE /utf-8)
  set_property(TARGET PC98PLAYER_SDL PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
