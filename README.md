# FM-1 (JUCE)

The M-VAVE FM-1 emulated as an AU / VST3 / AAX / Standalone instrument. The emulator is
[fm1-emu](https://github.com/joakimeriksson/fm1-emu) (MIT, Joakim Eriksson): its Rust core and its
plugin's machine thread (`engine.rs`) and saves (`savefile.rs`) are compiled unchanged into a static
library (`rust/fm1-ffi`). The plugin shell, the editor and the panel are C++/JUCE.

fm1-emu is a git submodule:

```bash
git clone --recursive https://github.com/diegocapoccitti/fm1-juce.git
# or, in a clone made without it: git submodule update --init
```

## Build (macOS)

```bash
brew install rust cmake ninja
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

AU and VST3 are copied to `~/Library/Audio/Plug-Ins`. The AAX goes to `build/aax-out/`; to load it in
Pro Tools copy it to `/Library/Application Support/Avid/Audio/Plug-Ins` (admin). A release build
for Pro Tools must be signed with PACE (wraptool); unsigned AAX builds load only in Pro Tools Developer.

The users of the plugin need nothing installed: the Rust core is linked inside each binary.

### Distribution builds
Homebrew's Rust has only the host target and its standard library is built for the current macOS.
For a universal binary that also runs on older macOS, use rustup:

```bash
rustup target add aarch64-apple-darwin x86_64-apple-darwin
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DFM1_UNIVERSAL=ON
```

## iOS (app + AUv3)
Needs rustup's toolchain with the iOS targets (`rustup target add aarch64-apple-ios aarch64-apple-ios-sim`):

```bash
cmake -S . -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DFM1_TEAM_ID=<team>
xcodebuild -project build-ios/FM1.xcodeproj -scheme FM1_Standalone -configuration Release \
    -sdk iphoneos -allowProvisioningUpdates build
xcrun devicectl device install app --device <udid> build-ios/FM1_artefacts/Release/Standalone/FM-1.app
```

`-DFM1_IOS_SIMULATOR=ON` (in its own build folder) builds for the Simulator. The app carries the
AUv3 (AUM, GarageBand, Cubasis...) and Felucca, loaded on the first launch and left in the
app's folder in Files. iOS allows no JIT: the core interprets, about real time at 240 MHz on
an Apple-silicon core; lower the CPU clock if a firmware can't keep up.

## MPE
The MPE button (bar) takes an MPE controller: with a MIDI channel set, all its notes go to that
channel and the note played last bends (per-note range 48, the firmware's 2), presses and slides
(CC74) it. With the channel "as sent" MPE passes unchanged, for firmwares that speak MPE.

## The panel
The background is a photographed top view of the black FM-1 (`Resources/fm1_top.png`, cut out
of its white backdrop; the app icon is made from it). The photo is a product picture of the
M-VAVE FM-1 from a store listing: its rights stay with its owner, and it is not covered by this
repository's licence. Delete it (and `Resources/AppIcon`) to build with the rendered panel. `Source/PanelLayout.h` gives every part's position in
that photo's pixels (1478 x 898): the knobs are cut out of the photo and turned, pressed keys and
buttons sink into shadow, the keys' light bars glow with the firmware's LEDs, and the emulated
screen covers the photographed one. Without the image the panel is rendered instead.

## Licence
MIT (`LICENSE`), as fm1-emu. Felucca, in `Resources/Firmware`, is GPL-3.0: see its README there.
Not affiliated with M-VAVE.

## Firmware
Load a `.fwsc` package (M-VAVE's `FM-1.fwsc` or any community firmware) or a `.fm1save`.
The package and the flash sectors the firmware writes are saved with the project.
