<h1 align="center">
<img align="center" src="extras/livearea/startup.png" width="280"><br>
Umineko no Naku Koro ni · PSVita Port
</h1>
<p align="center">
  <a href="#setup-instructions-for-end-users">How to install</a> •
  <a href="#controls">Controls</a> •
  <a href="#customization">Customization</a> •
  <a href="#known-issues">Known issues</a> •
  <a href="#build-instructions-for-developers">How to compile</a> •
  <a href="#credits">Credits</a> •
  <a href="#disclaimer">Disclaimer</a> •
  <a href="#license">License</a>
</p>

> Umineko no Naku Koro ni is a visual novel by 07th Expansion. October 4th, 1986: the Ushiromiya family gathers on the private island of Rokkenjima for the annual family conference, as a typhoon closes in and the legend of the Golden Witch, Beatrice, starts to come true.

This repository contains a PS Vita port of the **[Umineko Project](https://www.umineko-project.org/)** release of the game — the fan port of the PS3 version ("Umineko no Naku Koro ni Rondo of the Witch and Reasoning" / "Nocturne of Truth and Illusions", with the PS3 art, full voice acting and videos) that runs on their open-source **ONScripter-RU** engine.

How it works: the engine is built as an Android ARMv7 library and run on the Vita through the
[Android SO Loader by TheFloW](https://github.com/TheOfficialFloW/gtasa_vita)
and the [soloader-boilerplate](https://github.com/v-atamanenko/soloader-boilerplate), with graphics going through
[vitaGL](https://github.com/Rinnegatamante/vitaGL). ONScripter-RU itself was **heavily modified and optimized
for the Vita** (source in [`engine/onscripter-ru`](engine/onscripter-ru)): hardware video decoding through
the Vita's own decoder, a rewritten textbox/HUD presentation path, memory and loading-time work, all the
assets re-encoded for the Vita's screen and memory, and a lot of smaller fixes.

> **About this release.** The porting work was done by AI models, supervised and tested by a human. Parts
> of every chapter, the menus, saving/loading and the videos were played on hardware, but **the full game
> has not been play-tested from start to finish**. If you run into a problem — a crash, a stuck scene, a
> wrong effect — please [open an issue](https://github.com/stoicpingu/umineko-vita/issues) and say where in
> the story it happened.
>
> **Version 1.0 is English only.** Other translations may come in an update.

A short legal note appears in the [Disclaimer](#disclaimer) section near the bottom of this page — please read it before downloading anything.

## Setup Instructions (For End Users)

In order to properly install the game, you'll have to follow these steps precisely:

- Install [kubridge](https://github.com/bythos14/kubridge/releases/) by copying `kubridge.skprx` to your taiHEN plugins folder (usually `ur0:tai`) and adding it to your `config.txt` under `*KERNEL`:

```
  *KERNEL
  ur0:tai/kubridge.skprx
```

- Make sure you have `libshacccg.suprx` in the `ur0:/data/` folder on your console. If you don't, follow [this guide](https://samilops2.gitbook.io/vita-troubleshooting-guide/shader-compiler/extract-libshacccg.suprx) to extract it.
- <u>Legally</u> own the game. The Umineko Project's [copyright message](https://www.umineko-project.org/en/copyright-message/) allows the use of their port only to people who own **the PC release of Umineko no Naku Koro ni** and **the PS3 release of Umineko no Naku Koro ni Rondo**, and asks everyone to support the people who made the game by buying them. This port follows the same rule.
- Download the game data archive [`umineko-vita-release.zip`](https://drive.google.com/file/d/1onPuW4rIEvKBaPn4ZX_vYy0WexlTkG01/view?usp=sharing) (about 4.8 GB). **You need this archive, not the Umineko Project's PC download**: the assets in it were resized and re-encoded for the Vita's screen, memory and hardware video decoder, and the configuration files were made specifically for this port — the PC files will not work with it. The archive is **password protected** with the Umineko Project's own passphrase, exactly like their releases: owners of the games can build it from their manual and game files as described in the [copyright message](https://www.umineko-project.org/en/copyright-message/) (just google things ;)). The password is not published here.
- Extract the archive. It contains a single folder, `umineko`. Copy that folder to `ux0:data/` on your Vita, so that you end up with `ux0:data/umineko/`. It is about **5 GB** in ~100,000 files, so expect the copy to take a while (use a USB/SD2Vita connection rather than FTP if you can).
- Install `umineko.vpk` (from [Releases](https://github.com/stoicpingu/umineko-vita/releases/latest)).

The final layout under `ux0:data/umineko/` should look like this:

```
ux0:data/umineko/
├── libmain.so          (the game engine)
├── en.file             (the script)
├── ons.cfg             (options, see Customization)
├── default.cfg
├── game.hash
├── render_scale.txt
├── backgrounds/
├── fonts/
├── graphics/
├── legacy/
├── sound/
├── sprites/
├── video/
└── save/               (your saves; ships with a clean save and pre-rendered caches)
```

Don't rename or move anything inside the folder — the game checks its files against `game.hash` on launch.

The first launch is a little slower than the following ones (the game builds its caches); after that the
title screen is reached in a few seconds.

Controls
-----------------

|            Button             | Action                                                                                      |
|:-----------------------------:|:--------------------------------------------------------------------------------------------|
|           ![cross]            | Advance text / confirm / skip an effect                                                     |
|           ![circl]            | Open the Save screen while reading; back / close in menus, the backlog and the hidden textbox |
|           ![trian]            | Hide / show the text window; back / close in menus                                           |
|           ![squar]            | Auto mode on (any input turns it off)                                                        |
|           ![start]            | Open / close the system and save menu                                                        |
|           ![selec]            | Mute toggle (voices included)                                                                |
|           ![trigl]            | Open the backlog / page up; previous page in Bookmarks                                       |
|           ![trigr]            | Skip mode toggle; next page in Bookmarks                                                     |
| ![dpadh] / ![dpadv] / ![joysl] | Menu and choice navigation (with auto-repeat); Right advances text; Up/Down pick a slot and Left/Right change page in Bookmarks |
|           ![joysr]            | Up/Down: page the backlog back / forward while it is open                                   |

Touch is supported too: **1 tap** = advance, **2-finger tap** = menu, **2-finger drag** = scroll the backlog,
**3-finger swipes** = skip / auto / hide textbox / mute.

A button pressed while a line is still appearing (Start, Circle, Triangle, L) is remembered and applied when
the line reaches its click wait, so you never have to press twice.

## Customization

Most things are set from the game's own **Config** screen (open the menu with ![start]): volumes, per-character
voices, text speed, auto-mode speed, textbox style and so on. Those settings are saved in `ux0:data/umineko/save/`.

A couple of options live in a text file instead, `ux0:data/umineko/ons.cfg`. Edit it with any text editor
(one option per line) and restart the game. **These two lines are the only ones meant to be changed:**

| Line | What it does |
|---|---|
| `font-multiplier=b1:1.15,b5:1.15` | Dialogue text size. `1.15` = 15 % larger than the original PS3 size (the default of this port, chosen for the Vita's screen). Keep both numbers equal; `1.0` is the original size, anything up to about `1.3` still fits the textbox (it grows upward when needed). Changing it makes the **next** start slower once, while the game re-renders a few cached images with the new size. |
| `env[loadlog]=false` | Fast loading (about 7 s): after loading a save the message log starts empty and fills as you read. Set it to `true` for the game's original behaviour — the whole episode's log is rebuilt on every load, which takes about 20 s extra on the Vita, but the backlog's "jump to this line" works. |

```diff
! ⚠️ Please don't touch the other lines of ons.cfg, nor default.cfg, game.hash or render_scale.txt.
! They tie the engine to the Vita-specific assets and memory limits; changing them will break the game.
```

## Known Issues

- **Not fully play-tested.** Parts of every chapter, the menus, saving/loading and the videos were played
  on the Vita, but not the whole game from start to finish. Please report anything odd in the
  [issues](https://github.com/stoicpingu/umineko-vita/issues).
- **Reduced motion.** A few of the heaviest screen effects run in a simplified form to fit the Vita's memory.
- **Russian and other languages**: not in 1.0.

## Build Instructions (For Developers)

The port has two halves: the **loader** (this repository, C, built with vitasdk) and the **engine**
(`engine/onscripter-ru`, ONScripter-RU built as an Android ARMv7 shared library with the NDK toolchain in
`engine/onscripter-ru/DerivedData/ndk`).

For the loader you'll need a [vitasdk](https://github.com/vitasdk) build fully compiled with softfp usage.
You can find a precompiled version [here](https://github.com/vitasdk/buildscripts/releases).

Additionally, you'll need these libraries built and installed from source:

- [vitaShaRK](https://github.com/Rinnegatamante/vitaShaRK)

  - ```bash
    make install
    ```

- [kubridge](https://github.com/bythos14/kubridge)

  - ```bash
    mkdir build && cd build
    cmake .. && make install
    ```

- [vitaGL](https://github.com/Rinnegatamante/vitaGL) — the port links a vendored copy with small patches
  (`lib/vitaGL-src`, render-target eviction and a scene-reset fix). Build it with:

  - ```bash
    make SOFTFP_ABI=1 CIRCULAR_VERTEX_POOL=2 HAVE_GLSL_SUPPORT=1
    ```

After all these requirements are met, you can compile the loader with the following commands:

```bash
cmake -S . -B build
cmake --build build
```

This produces `build/eboot.bin` and `build/umineko.vpk`. The engine is rebuilt with
`cd engine/onscripter-ru && make -j8`, and the result (`DerivedData/Droid-arm/onscripter-ru`) is shipped as
`ux0:data/umineko/libmain.so` — after a loader change send `eboot.bin` to `ux0:app/UMNK00001/`, after an
engine change send `libmain.so`; a change to the loader's exports needs both. The game assets are produced
from the Umineko Project's files by `extras/scripts/build_vita_assets.py` (resizes, re-encodes the videos
for the Vita's decoder, regenerates `game.hash`).

The CMakeLists also exposes a few convenience targets (set `PSVITAIP` to your Vita's IP):

```bash
cmake --build build --target send # Upload eboot.bin and relaunch (requires vitacompanion)
cmake --build build --target dump # Fetch the latest coredump and parse it through vita-parse-core
```

Engine addresses in a crash dump can be symbolicated with the NDK's `addr2line` on `libmain.so`; the engine is
loaded at `0x98000000`, so subtract that from the crashing address first.

## Credits
- [Umineko Project](https://www.umineko-project.org/) for the PS3-fication port and the ONScripter-RU engine this port is built on, and for making it open source.
- [07th Expansion](https://07th-expansion.net/) / Ryukishi07 for Umineko no Naku Koro ni, and Alchemist for the PS3 version.
- [Andy "The FloW" Nguyen](https://github.com/TheOfficialFloW/) for the original .so loader.
- [Rinnegatamante](https://github.com/Rinnegatamante/) for vitaGL, vitaShaRK and the reference ports whose video playback code showed the way.
- [Volodymyr Atamanenko](https://github.com/v-atamanenko/) for [soloader-boilerplate](https://github.com/v-atamanenko/soloader-boilerplate), the starter kit this port is built on top of.
- [bythos14](https://github.com/bythos14/) for kubridge.
- [Spazzery](https://gbatemp.net/members/spazzery.495271/), for the motivation to finish the port and release it.
- Everybody on the Vita community on Reddit and Discord who keeps the Vita homebrew scene alive.

## Disclaimer

Umineko no Naku Koro ni is copyright © 07th Expansion. The PlayStation 3 version was developed and published
by Alchemist. *Umineko no Naku Koro ni*, its logo, characters, music, voices, artwork and all other
trademarks and copyrighted material are the property of their respective owners.

The work presented in this repository is not "official" or produced or sanctioned by the owner(s) of the
aforementioned trademark(s), nor by the Umineko Project, nor by any other registered trademark mentioned in
this repository.

This repository contains only the port's source code: the loader and the modified ONScripter-RU engine
(whose own license requires the modifications to be published, which they are here). The game data is
distributed separately as an archive protected with the Umineko Project's passphrase, under the terms of
their [copyright message](https://www.umineko-project.org/en/copyright-message/): private, non-commercial use,
outside Japan only, and only by people who own the games listed there. Please read that page — it is the
Umineko Project's work and their conditions apply to this port as well. The authors of this work do not
promote or condone piracy in any way.

## License
The loader may be modified and distributed under the terms of the MIT license — see the [LICENSE](LICENSE)
file. The engine in `engine/onscripter-ru` is ONScripter-RU, distributed under the GNU GPL v2 (with the
BSD 3-Clause option for the parts owned by the Umineko Project) — see
[engine/onscripter-ru/LICENSE](engine/onscripter-ru/LICENSE).

[cross]: https://raw.githubusercontent.com/v-atamanenko/sdl2sand/master/img/cross.svg "Cross"
[circl]: https://raw.githubusercontent.com/v-atamanenko/sdl2sand/master/img/circle.svg "Circle"
[squar]: https://raw.githubusercontent.com/v-atamanenko/sdl2sand/master/img/square.svg "Square"
[trian]: https://raw.githubusercontent.com/v-atamanenko/sdl2sand/master/img/triangle.svg "Triangle"
[joysl]: https://raw.githubusercontent.com/v-atamanenko/sdl2sand/master/img/joystick-left.svg "Left Joystick"
[joysr]: https://raw.githubusercontent.com/v-atamanenko/sdl2sand/master/img/joystick-right.svg "Right Joystick"
[dpadh]: https://raw.githubusercontent.com/v-atamanenko/sdl2sand/master/img/dpad-left-right.svg "D-Pad Left/Right"
[dpadv]: https://raw.githubusercontent.com/v-atamanenko/sdl2sand/master/img/dpad-top-down.svg "D-Pad Up/Down"
[selec]: https://raw.githubusercontent.com/v-atamanenko/sdl2sand/master/img/dpad-select.svg "Select"
[start]: https://raw.githubusercontent.com/v-atamanenko/sdl2sand/master/img/dpad-start.svg "Start"
[trigl]: https://raw.githubusercontent.com/v-atamanenko/sdl2sand/master/img/trigger-left.svg "Left Trigger"
[trigr]: https://raw.githubusercontent.com/v-atamanenko/sdl2sand/master/img/trigger-right.svg "Right Trigger"
