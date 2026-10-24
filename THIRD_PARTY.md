# Third-party software

The native executable contains ONScripter-RU and native libraries. Their
original licenses remain applicable. No game data is included.

| Component | Source / license |
| --- | --- |
| ONScripter-RU | `upstream/onscripter-ru/LICENSE`, `LICENSE-GPLv2`, `LICENSE-BSD`; source pinned in `upstream/source-manifest.json` |
| FIOS initialization/cache adapter | Andy Nguyen, MIT; adapted from the local legacy port, notice in `licenses/FIOS.txt` |
| SDL_gpu | `vendor/sdl-gpu/LICENSE.txt`, MIT; pin in `UPSTREAM.txt` |
| SDL2, SDL_image, SDL_mixer | [libsdl-org](https://github.com/libsdl-org), zlib license; native VitaSDK archives, with local SDL2 viewport ABI and SDL_mixer offline-decoder lock adaptations in `platform/vita/` |
| vitaGL | [Rinnegatamante/vitaGL](https://github.com/Rinnegatamante/vitaGL), LGPLv3; native VitaSDK archive with the local GLSL translator repair in `vendor/vitagl-translator/` |
| vitaShaRK | [Rinnegatamante/vitaShaRK](https://github.com/Rinnegatamante/vitaShaRK); native VitaSDK archive |
| FFmpeg | [6.0 source](https://ffmpeg.org/releases/ffmpeg-6.0.tar.xz), LGPL; reproducible configure/source hash in `extras/scripts/build_native_ffmpeg.sh` |
| libpng | `native-bench/vendor/libpng/LICENSE`, locally compiled |
| FreeType | [2.14.3 source](https://download.savannah.gnu.org/releases/freetype/freetype-2.14.3.tar.xz), FreeType License; locally compiled (soft-float), notice in `native-bench/licenses/FreeType.txt` |
| zlib | [1.3.2 source](https://github.com/madler/zlib/releases/tag/v1.3.2), zlib license; locally compiled (soft-float) |
| HarfBuzz | [harfbuzz/harfbuzz](https://github.com/harfbuzz/harfbuzz), MIT-style license |
| FriBidi | [fribidi/fribidi](https://github.com/fribidi/fribidi), LGPL |
| libass | [libass/libass](https://github.com/libass/libass), ISC |
| LuaJIT | [LuaJIT/LuaJIT](https://github.com/LuaJIT/LuaJIT), MIT |
| libogg, Vorbis, Opus | [Xiph.Org](https://xiph.org/), BSD-style licenses; Ogg notice in `native-bench/licenses/libogg.txt` |
| bzip2 | `native-bench/licenses/bzip2.txt` |
| JPEG, WebP, mpg123, libmodplug, libxmp | Native VitaSDK dependencies; retain their upstream licenses when distributing |

The historical MVP additionally links vita2d and Tremor; see
`native-bench/THIRD_PARTY.txt`. The full engine does not load the old Android
shared object or the former MIT SO loader.

Japanese text geometry fixes are adapted from [AuroraWright’s ONScripter-RU fork](https://github.com/AuroraWright/onscripter-ru/tree/021bc2440e081bed5e12ace6516021bd423719bc), under the existing ONScripter-RU license. See `docs/NATIVE_JAPANESE.md` for scope and provenance.

The optional PT-BR data add-on uses the Portuguese script from [Umineko Project r4068](https://github.com/umineko-project/umineko-scripting/releases/tag/z4068) and translation assets from [Monaco-a-Knox/language_pt](https://github.com/Monaco-a-Knox/language_pt). See `docs/NATIVE_PORTUGUESE.md` for pinned source revisions and Vita-specific preparation. These game assets are separate from the engine VPK.

The optional Simplified Chinese script comes from [SNS Team's umineko-scripting](https://github.com/Umineko-Project-CN/umineko-scripting/tree/162a77eb764f181236b8044d1f75a93458ea4937). Generated script packages retain its original [BSD-3-Clause notice](https://github.com/Umineko-Project-CN/umineko-scripting/blob/162a77eb764f181236b8044d1f75a93458ea4937/LICENSE.txt), credits and source revision. That license does not grant rights to the game's fonts, images or other assets; these are not distributed in this repository. See [README.zh-CN.md](README.zh-CN.md).
