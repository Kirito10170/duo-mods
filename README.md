# duo-mods

Second-screen companions for [Eden Duo](https://github.com/igawa6/eden-duo), the Switch emulator
fork for dual-screen Android handhelds such as the AYN Thor. Each companion is a `.dsmod.zip`
package, installed from the game's **Add-ons** menu in Eden Duo (**Install** → **Dual screen mods**).

The package format, module ABI and tools follow
[igawa6/eden-duo-companions](https://github.com/igawa6/eden-duo-companions); its `docs/` folder is
the reference. `tools/build_dualscreen_package.py` and `tools/compact_zip.py` are copied from
there unchanged (GPL-3.0).

## Companions

| Game | Title ID | Game version | Status |
|---|---|---|---|
| The Legend of Zelda: Tears of the Kingdom | `0100F2C0115B6000` | 1.4.2 | In development: Map, Inventory and Settings pages |
| The Legend of Zelda: Breath of the Wild | `01007EF00011E000` | 1.6.0 | Use igawa6's [official companion](https://github.com/igawa6/eden-duo-companions/releases/tag/breath-of-the-wild-1.0.0) |

## Layout

| Path | What it is |
|---|---|
| `packages/<Game>/dualscreen/` | Package source: `manifest.json` and any self-drawn assets |
| `native/modules/<TITLEID>.cpp` | Native module for that game (C ABI, one `.so` per platform) |
| `native/CMakeLists.txt` | Builds the modules against the Eden Duo module SDK headers |
| `tools/` | Package builder and manifest compactor |
| `.github/workflows/build.yml` | Builds the Android (arm64-v8a) and Linux modules and the `.dsmod.zip` |

Packages ship no game art, text or code. Everything shown is read from the player's own game.

## Building

CI builds every push; download the `.dsmod.zip` from the workflow run's artifacts. Locally you
need an [Eden Duo](https://github.com/igawa6/eden-duo) checkout (module SDK headers and
`externals/bc_decoder`) and [zstd](https://github.com/facebook/zstd) v1.5.7 sources:

```sh
cmake -S native -B build/android -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static \
  -DEDEN_SOURCE_ROOT=/path/to/eden-duo -DZSTD_SOURCE_ROOT=/path/to/zstd
cmake --build build/android
python3 tools/build_dualscreen_package.py --package packages/TearsOfTheKingdom --output dist \
  --version 0.1.0 --min-runtime 22 \
  --build-id 5CB42B1CF25469FB0635FD046453D843C18BC8AB000000000000000000000000 \
  --module android-arm64-v8a=build/android/0100F2C0115B6000.so
```

Modules must work on Android, where Eden Duo runs games natively (NCE): no guest calls, only
memory reads/writes and button presses.

## License

GPL-3.0-or-later, see `LICENSE`.
