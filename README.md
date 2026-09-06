# tapa

A monophonic drum synthesizer combining FM, noise and metallic resonances.
Make kicks, snares, toms, hats, cymbals and synthetic percussion with seven controls.
Available as a standalone app, CLAP, AUv3 and WCLAP plugin.

## Controls

| Control | What it does |
|---|---|
| Decay | Sets the tail length, 20–4000 ms at MIDI note 60. Higher notes decay faster; lower notes ring longer. Cymbal sounds use gentler tracking. |
| FM Ratio | Tunes the FM modulator relative to the body; tunes metallic resonances and noise colour. |
| FM Amount | Adds FM depth; spreads the metallic resonances. |
| Transient | Changes the attack from soft to sharp. Stronger cymbal strikes shorten the noise wash relative to the ringing. |
| Saturation | Adds harmonics and density. |
| Feedback | Adds FM roughness; lengthens metallic ringing. |
| Noise | Moves from the FM body through metallic ringing and wash to pure filtered noise at 100%. |

The **body / linked** button switches between a short FM envelope and FM following
Decay. It affects the FM body, not the independent metallic resonances.

Pitch and velocity follow MIDI. Notes retrigger the voice; releasing a note lets
its tail finish. Control changes take effect on the next hit. The 22 factory
presets include drums, bass, bells, metal, noise and clicks. Each preset includes
a suggested MIDI note in its host description.

Drag vertically to set a slider, or drag across columns to change several.
Double-click resets a control. Values appear while editing. The background reacts
to the controls and sound; the border pulses with each note.

## Build

Requires CMake 3.24+, a C++17 compiler, chardsp, char-clap-utils, Compost,
CLAP, clap-helpers, clap-wrapper and CHOC. Point the `*_ROOT` CMake cache variables
at your dependency checkouts. WCLAP also requires a WASI SDK with pthread support.

```sh
cmake --preset native
cmake --build --preset native
ctest --preset native

WASI_SDK_ROOT=/path/to/wasi-sdk cmake --preset wclap
cmake --build --preset wclap
```

Use the `xcode` preset for the macOS AUv3 build. Outputs are in each build
folder's `artifacts` directory. Browser UI checks are in `tests/UIResize.cjs`
and require Playwright: `NODE_PATH=/path/to/node_modules node tests/UIResize.cjs`.
