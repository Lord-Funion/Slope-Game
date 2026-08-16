# Slope CE Native

A from-scratch native TI-84 Plus CE implementation of the classic **Slope** gameplay loop, built to feel far closer to the browser/Unity game than the existing tiny CE port.

## What this version is trying to preserve

- continuous pseudo-3D downhill track rather than a flat 2D imitation
- green rolling ball, neon green roadway/city, black void and red 3D obstacles
- momentum-based left/right steering
- increasing forward speed and score
- curves, elevation changes, narrow sections, ramps and gaps
- procedural endless course generation
- death from falling off the course or striking a red obstacle
- on-calculator best-score save

The browser repository in the root of this project contains a compiled Unity WebGL build, not the original Unity C# project. The TI port therefore does **not** attempt to execute WebAssembly/Unity on the calculator. It reimplements the game loop natively for the eZ80 and uses fixed-point integer projection plus GraphX primitives.

## Performance approach

There is no floating point in the gameplay/render loop. The 3D look comes from integer perspective projection and a small ring of course segments. Road slabs are two triangles each; obstacles and city blocks are projected cuboids; the ball is drawn as a shaded sphere. Everything is double-buffered with GraphX.

This is deliberately a native renderer instead of emulation. That lets the CE spend its CPU budget on the parts that actually make Slope look and feel like Slope.

## Controls

- **Left / Right:** steer
- **2nd / Enter:** start or restart
- **Alpha:** pause / unpause
- **Clear:** return to title; Clear again quits

## Build

Install the current [CE C/C++ Toolchain](https://ce-programming.github.io/toolchain/), then:

```sh
cd ti84ce
make
```

The result is `bin/SLOPECE.8xp`. Transfer it and the standard CE libraries to a TI-84 Plus CE / TI-84 Plus CE Python Edition.

## Source/layout note

The pre-existing third-party "Slope CE" archive by `imatree` is distributed as a calculator binary rather than a public source repository, so this directory is an independent native implementation rather than a source fork of that binary.
