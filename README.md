# HEXOSCOPE — waveform, scope, spectrum & stereo analyzer (AU / VST3 / Standalone)

By Hamed Ghafari. A zero-latency analysis plugin: audio passes through untouched.

## Current features

- Display modes: Waveform, Scope, Spectrum, Goniometer
- Channels: Mid, Left, Right, Side, L/R, M/S
- Colour modes: Static, Band Balance, Energy Map, HEX
- True FFT spectrum with spectral centroid, 85% rolloff and spectral flux
- Per-channel peak/RMS measurement with peak hold
- dBFS-oriented visual guides
- DC offset, waveform asymmetry and stereo correlation readouts
- Trigger controls for Scope mode: threshold + pre-trigger position
- Scroll / Wrap display
- Window: 1–30 s across the display
- Freeze display without stopping audio analysis
- Clear analysis/history
- M/S goniometer
- Resizable UI, settings saved with your DAW project
- Double-click the display to hide or show all controls
- Audio thread never waits for the GUI: visual buffers drop data rather than blocking real-time audio

## Build on macOS

1. `xcode-select --install`
2. `brew install cmake`
3. From this folder:

       cmake -B build -G Xcode
       cmake --build build --config Release

   (first run downloads JUCE 8.0.4; needs internet)

The plugins are copied automatically to:

- `~/Library/Audio/Plug-Ins/Components/HEXOSCOPE.component` (AU)
- `~/Library/Audio/Plug-Ins/VST3/HEXOSCOPE.vst3`

Then rescan plugins in your DAW. For Logic, validate with:

    auval -v aufx Hexo Hgha

For an unsigned local build:

    codesign --force --deep -s - ~/Library/Audio/Plug-Ins/Components/HEXOSCOPE.component
    codesign --force --deep -s - ~/Library/Audio/Plug-Ins/VST3/HEXOSCOPE.vst3

The standalone app is in:

    build/Hexoscope_artefacts/Release/Standalone/

## Testing checklist

Before shipping a release, test at 44.1 / 48 / 96 / 192 kHz and small host blocks such as 32 / 64 / 128 samples.

Recommended signal checks:

- Silence
- 100 Hz / 1 kHz / 10 kHz sine
- L-only and R-only
- L = R (Mid only)
- L = -R (Side only / polarity)
- Impulse / kick transient
- DC-like offset
- Very low and near-full-scale signals

The analyzer must never change the rendered audio, and the GUI must remain responsive while the audio thread is active.

## Release validation note

This source revision has had a static code review for real-time safety, parameter/state handling, FIFO usage, circular history, FFT publication, trigger indexing, stereo analysis, and display clearing. A final AU/VST3 host validation still needs to be performed on macOS with Xcode, including `auval` and tests at multiple sample rates/block sizes.
