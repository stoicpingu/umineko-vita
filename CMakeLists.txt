cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(NOT DEFINED ENV{VITASDK})
    message(FATAL_ERROR "Set VITASDK to a native VitaSDK installation")
  endif()
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE FILEPATH "VitaSDK toolchain")
endif()
project(onscripter_ru_vita LANGUAGES C CXX)
include("${VITASDK}/share/vita.cmake" REQUIRED)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(ONS_UPSTREAM "${CMAKE_CURRENT_SOURCE_DIR}/upstream/onscripter-ru")
set(VITA_TITLEID "UMNK00001" CACHE STRING "Umineko release application ID")
set(VITA_VERSION "01.20" CACHE STRING "Umineko release version (v1.2)")
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")
option(ONS_VITA_EXPORT_SHADERS "Developer-only: compile and export native shaders on the Vita" OFF)
option(ONS_VITA_RENDER_DIAGNOSTICS "Run a small, one-shot GPU probe at the title" OFF)
option(ONS_VITA_FILE_LOGGING "Developer-only: write native stdout/stderr logs to storage" OFF)

find_program(Python3_EXECUTABLE NAMES python3 NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_program(ONS_HOST_CC NAMES cc clang gcc NO_CMAKE_FIND_ROOT_PATH REQUIRED)
file(GLOB_RECURSE ONS_RESOURCES CONFIGURE_DEPENDS "${ONS_UPSTREAM}/Resources/Shaders/*.frag" "${ONS_UPSTREAM}/Resources/Shaders/*.vert" "${ONS_UPSTREAM}/Resources/Images/*.png")
list(FILTER ONS_RESOURCES EXCLUDE REGEX "/\\._")
set(ONS_EMBED "${CMAKE_CURRENT_BINARY_DIR}/host-tools/ons-embed")
add_custom_command(OUTPUT "${ONS_EMBED}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/host-tools"
  COMMAND "${ONS_HOST_CC}" -std=c11 "${ONS_UPSTREAM}/Tools/embed/embed.c" -o "${ONS_EMBED}"
  DEPENDS "${ONS_UPSTREAM}/Tools/embed/embed.c" VERBATIM)
set(ONS_RESOURCE_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/Resources.cpp")
add_custom_command(OUTPUT "${ONS_RESOURCE_SOURCE}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_resources.py"
    --tool "${ONS_EMBED}" --source "${ONS_UPSTREAM}" --output "${ONS_RESOURCE_SOURCE}"
  DEPENDS "${ONS_EMBED}" ${ONS_RESOURCES} tools/embed_resources.py VERBATIM)

# The MVP's local PNG build matches FreeType's setjmp ABI, unlike the
# preinstalled SDK PNG archive. Both native applications use this same source.
file(GLOB ONS_PNG_SOURCES CONFIGURE_DEPENDS native-bench/vendor/libpng/png*.c)
add_library(ons_native_png STATIC ${ONS_PNG_SOURCES})
target_include_directories(ons_native_png PUBLIC native-bench/vendor/libpng)
target_compile_definitions(ons_native_png PRIVATE PNG_ARM_NEON_OPT=0)
target_link_libraries(ons_native_png PUBLIC z)
include(platform/vita/graphics.cmake)
file(GLOB_RECURSE ONS_ENGINE_SOURCES CONFIGURE_DEPENDS "${ONS_UPSTREAM}/Engine/*.cpp")
list(FILTER ONS_ENGINE_SOURCES EXCLUDE REGEX "/\\._")
set(ONS_SUPPORT_SOURCES Support/AudioBridge.cpp Support/Cache.cpp Support/DirPaths.cpp
  Support/DirtyRect.cpp Support/FileIO.cpp Support/Unicode.cpp External/slre.c)
list(TRANSFORM ONS_SUPPORT_SOURCES PREPEND "${ONS_UPSTREAM}/")
add_executable(onscripter_vita ${ONS_ENGINE_SOURCES} ${ONS_SUPPORT_SOURCES}
  "${ONS_RESOURCE_SOURCE}" platform/vita/platform.cpp platform/vita/assets.cpp
  platform/vita/gxm_checked.c platform/vita/gl_init.c platform/vita/render_diagnostics.cpp
  platform/vita/startup_image.cpp)
if(ONS_VITA_RENDER_DIAGNOSTICS)
  target_compile_definitions(onscripter_vita PRIVATE ONS_VITA_RENDER_DIAGNOSTICS)
endif()
if(ONS_VITA_FILE_LOGGING)
  target_compile_definitions(onscripter_vita PRIVATE ONS_VITA_FILE_LOGGING)
endif()
if(ONS_VITA_EXPORT_SHADERS)
  target_compile_definitions(onscripter_vita PRIVATE ONS_VITA_EXPORT_SHADERS)
else()
  add_custom_target(ons_validate_shaders
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/native_shaders.py"
      --resources "${ONS_RESOURCE_SOURCE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/platform/vita/shaders/${ONS_SHADER_ABI}"
    DEPENDS "${ONS_RESOURCE_SOURCE}" ${ONS_SHADER_BINARIES}
    COMMENT "Validating every bundled shader against the compiled resource corpus"
    VERBATIM)
  add_dependencies(onscripter_vita ons_validate_shaders)
endif()
target_compile_definitions(onscripter_vita PRIVATE VITA PUBLIC_RELEASE USE_STD_REGEX USE_LUA SDL_MAIN_HANDLED)
target_compile_options(onscripter_vita PRIVATE -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -Wall -Wextra
  -Wno-unused-parameter -Wno-missing-field-initializers -Wno-deprecated-declarations)
target_include_directories(onscripter_vita PRIVATE "${ONS_UPSTREAM}" "${CMAKE_CURRENT_SOURCE_DIR}"
  "${VITASDK}/arm-vita-eabi/include/freetype2"
  "${VITASDK}/arm-vita-eabi/include/harfbuzz"
  "${VITASDK}/arm-vita-eabi/include/luajit-2.1")
# ARM/Thumb tail-call veneers must remain valid after the Vita relocates RX.
# GNU ld otherwise emits absolute literals without relocations for distant
# imports (observed in SDL's sceKernelGetThreadId startup path).
target_link_options(onscripter_vita PRIVATE -Wl,-q -Wl,--pic-veneer -Wl,--gc-sections "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/onscripter_vita.map")
target_link_options(onscripter_vita PRIVATE -Wl,--wrap=vglInitExtended)
foreach(ONS_GXM_CALL BeginScene EndScene ReserveVertexDefaultUniformBuffer
    ReserveFragmentDefaultUniformBuffer SetUniformDataF Draw DrawInstanced
    CreateRenderTarget DepthStencilSurfaceInit)
  target_link_options(onscripter_vita PRIVATE "-Wl,--wrap=sceGxm${ONS_GXM_CALL}")
endforeach()
include(platform/vita/media.cmake)
ons_vita_add_media(onscripter_vita)
# Static SDK libraries have circular dependencies; resolve the complete native
# graph here. No Android .so, Bionic/JNI bridge or kubridge is linked.
target_link_libraries(onscripter_vita PRIVATE -Wl,--start-group ons_vita_gpu ons_native_png stdc++
  SDL2_image ons_vita_mixer ons_vita_sdl2 freetype harfbuzz bz2 ass fribidi
  ${ONS_VITA_MEDIA_LIBRARIES}
  vorbisfile vorbis ogg opusfile opus mpg123 modplug xmp-lite
  luajit-5.1 dl jpeg webp sharpyuv z m pthread
  ons_vita_gl vitashark SceShaccCgExt taihen_stub SceShaccCg_stub mathneon
  ScePower_stub SceCtrl_stub SceGxm_stub SceDisplay_stub SceAppMgr_stub SceAppUtil_stub
  SceAudio_stub SceAudioIn_stub SceSysmodule_stub SceIofilemgr_stub SceCommonDialog_stub
  SceTouch_stub SceHid_stub SceMotion_stub SceProcessmgr_stub SceIme_stub
  SceKernelDmacMgr_stub SceSblSsMgr_stub SceAvPlayer_stub SceNet_stub SceNetCtl_stub -Wl,--end-group)

include(platform/vita/sony_io.cmake)

vita_create_self(eboot.bin onscripter_vita UNSAFE)
vita_create_vpk(umineko-native.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION} NAME "Umineko"
  FILE ${ONS_SHADER_PACKAGE_FILES}
       "${CMAKE_CURRENT_SOURCE_DIR}/platform/vita/data/native-image-sizes.tsv" native-image-sizes.tsv
       "${ONS_UPSTREAM}/LICENSE" licenses/ONScripter-RU.txt
       "${ONS_UPSTREAM}/LICENSE-GPLv2" licenses/GPL-2.0.txt
       "${CMAKE_CURRENT_SOURCE_DIR}/README.md" README.md
       "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY.md" THIRD_PARTY.md
       "${CMAKE_CURRENT_SOURCE_DIR}/licenses/FIOS.txt" licenses/FIOS.txt
       "${CMAKE_CURRENT_SOURCE_DIR}/licenses/FFmpeg.txt" licenses/FFmpeg.txt
       "${CMAKE_CURRENT_SOURCE_DIR}/licenses/FFmpeg-LGPL-2.1.txt" licenses/FFmpeg-LGPL-2.1.txt
       "${CMAKE_CURRENT_SOURCE_DIR}/licenses/SDL2.txt" licenses/SDL2.txt
       "${CMAKE_CURRENT_SOURCE_DIR}/licenses/SDL2_image.txt" licenses/SDL2_image.txt
       "${CMAKE_CURRENT_SOURCE_DIR}/licenses/SDL2_mixer.txt" licenses/SDL2_mixer.txt
       "${CMAKE_CURRENT_SOURCE_DIR}/vendor/sdl-gpu/LICENSE.txt" licenses/SDL_gpu.txt
       "${CMAKE_CURRENT_SOURCE_DIR}/vendor/vitagl-translator/COPYING.LESSER" licenses/vitaGL-LGPL-3.0.txt
       "${CMAKE_CURRENT_SOURCE_DIR}/vendor/vitagl-translator/COPYING" licenses/vitaGL-GPL-3.0.txt
       "${CMAKE_CURRENT_SOURCE_DIR}/native-bench/vendor/libpng/LICENSE" licenses/libpng.txt
       "${CMAKE_CURRENT_SOURCE_DIR}/native-bench/licenses/FreeType.txt" licenses/FreeType.txt
       "${CMAKE_CURRENT_SOURCE_DIR}/native-bench/licenses/libogg.txt" licenses/libogg.txt
       "${CMAKE_CURRENT_SOURCE_DIR}/native-bench/licenses/bzip2.txt" licenses/bzip2.txt
       "${CMAKE_CURRENT_SOURCE_DIR}/extras/livearea/icon0.png" sce_sys/icon0.png
       "${CMAKE_CURRENT_SOURCE_DIR}/extras/livearea/startup.png" sce_sys/livearea/contents/startup.png
       "${CMAKE_CURRENT_SOURCE_DIR}/extras/livearea/bg0.png" sce_sys/livearea/contents/bg0.png
       "${CMAKE_CURRENT_SOURCE_DIR}/extras/livearea/template.xml" sce_sys/livearea/contents/template.xml)
