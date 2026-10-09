# HEXOSCOPE — waveform analyzer plugin (AU / VST3 / Standalone)

By Hamed Ghafari. A scrolling waveform meter. Audio passes through untouched.

## Features
- Channels: Mid, Left, Right, Side, L/R (stacked), M/S (stacked)
- Color modes: Static, Multi-Band (low = red, mid = green, high = blue), Color Map (spectral centroid), HEX (modern blues)
- Scroll or Loop (playhead) display
- Window: 1–30 s across the display
- Resizable UI, settings saved with your DAW project
- Double-click the display to hide or show all controls

## Build on macOS
1. `xcode-select --install`
2. `brew install cmake`
3. From this folder:

       cmake -B build -G Xcode
       cmake --build build --config Release

   (first run downloads JUCE; needs internet)

The plugins are copied automatically to:
- `~/Library/Audio/Plug-Ins/Components/HEXOSCOPE.component`  (AU)
- `~/Library/Audio/Plug-Ins/VST3/HEXOSCOPE.vst3`

Then rescan plugins in your DAW. For Logic, you can validate with:

    auval -v aufx Hexo Hgha

If macOS complains about an unsigned plugin:

    codesign --force --deep -s - ~/Library/Audio/Plug-Ins/Components/HEXOSCOPE.component
    codesign --force --deep -s - ~/Library/Audio/Plug-Ins/VST3/HEXOSCOPE.vst3

A standalone app is in `build/Hexoscope_artefacts/Release/Standalone/`.
