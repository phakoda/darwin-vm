# Graphics roadmap

The long-term goal is a usable iPhone/macOS emulator: visible display output, interactive input,
accelerated graphics, GUI applications, networking, Wi-Fi/Bluetooth integration, and eventually
WindowServer/SpringBoard.

This file tracks the graphics path specifically. The implementation is intentionally staged so each
step is testable without requiring a complete Apple GPU implementation.

## Milestone 1: XNU boot framebuffer

`patches/qemu-sptm/0001-darwin-boot-framebuffer.patch` adds an opt-in early framebuffer to the
pinned `qemu-sptm` revision. `0002-darwin-select-graphics-console.patch` selects XNU's bootloader
graphics mode so `PE_create_console()` actually initializes the framebuffer-backed video console.

The patched Darwin machine:

- reserves a 1280×720×32-bit framebuffer inside XNU's boot-data region;
- passes that physical address through `boot_args.Video`;
- sets `Boot_Video.v_display` to the bootloader graphics-mode value (`1`);
- uses the ARM64 XNU boot-console pixel layout (`0x00RRGGBB`, B/G/R/X bytes in little endian);
- maps the same guest RAM into a QEMU `DisplaySurface`;
- refreshes the QEMU display from that shared RAM;
- leaves the existing serial-only boot unchanged unless `boot-fb=on` is requested.

Build the patched QEMU with:

```bash
./build_qemu.sh
```

Keep using the known-good serial workflow with:

```bash
./run.sh
```

Open the framebuffer window with:

```bash
GRAPHICS=1 ./run.sh
```

The normal serial boot uses `serial=3 -noprogress`. XNU switches its active console to the UART when
serial output is requested, and `-noprogress` suppresses framebuffer boot graphics. Therefore, when
`GRAPHICS=1` is used without an explicit `BOOT_ARGS`, `run.sh` drops those two settings and keeps
verbose mode enabled so XNU can paint boot output into the framebuffer. You can still override the
complete command line for experiments, for example:

```bash
GRAPHICS=1 BOOT_ARGS="rd=md0 -v wdt=-1 wlan-olyhal-abort" ./run.sh
```

This is an **early boot framebuffer**, not GPU emulation. It is meant to prove the entire
XNU → guest RAM → QEMU display path and give us visible kernel/boot-console pixels.

## Milestone 2: display/input services

After the framebuffer is proven on real firmware:

1. derive native panel dimensions/rotation/scale from the selected device tree instead of using a
   fixed development resolution;
2. expose host keyboard/mouse/touch events through an Apple-compatible HID path;
3. model enough display/IOSurface/IOMobileFramebuffer behavior for higher-level display services to
   keep running rather than falling back to the boot console;
4. add screenshot and deterministic framebuffer tests to make display regressions observable in CI.

## Milestone 3: accelerated Metal via Vulkan

A full AGX command-processor implementation is a very large reverse-engineering project. A faster
bring-up path is a paravirtual graphics stack that preserves the guest's high-level Metal behavior
while executing work through Vulkan on the Linux host.

[`steelbrain/metal2vulkan`](https://github.com/steelbrain/metal2vulkan) is useful here because it
translates Metal AIR (LLVM bitcode/sanitized LLVM IR) to Vulkan 1.2 SPIR-V and provides reflection
for descriptors, stage interfaces, argument buffers, function constants, and conservative buffer
footprints.

It does **not** implement Metal.framework, `.metallib` loading, command queues, resources, textures,
IOSurface, presentation, or the Apple GPU kernel driver. The proposed emulator split is therefore:

```text
Darwin app / CoreAnimation / SpringBoard
              |
        Metal API shim
              |
   pipeline state + AIR extraction
        |                 |
        |                 +--> metal2vulkan --> SPIR-V
        |
 shared command/resource ring
              |
      QEMU paravirtual GPU
              |
        host Vulkan 1.2+
              |
        Linux display
```

The guest shim must preserve enough Metal ABI behavior that system frameworks can create devices,
buffers, textures, render/compute pipelines, command buffers, fences/events, and drawables. The host
side owns Vulkan objects and presentation. `metal2vulkan` supplies shader translation inside that
pipeline rather than replacing the pipeline itself.

### Why keep AGX emulation as a second path?

Some system components may talk to IOGPU/AGX interfaces below Metal.framework or rely on behavior a
shim cannot reproduce safely. For those cases we should reuse public AGX reverse-engineering work
(where licensing permits) to model the minimum kernel/user-client ABI. The emulator can then choose
between:

- **paravirtual Metal/Vulkan** for fast GUI bring-up; and
- progressively more faithful **AGX device emulation** for compatibility.

## Milestone 4: WindowServer / SpringBoard

The first GUI success criterion is not "GPU benchmark passes"; it is:

- display services stay alive;
- IOSurface-backed content can be presented;
- CoreAnimation can submit a frame;
- input reaches the foreground UI;
- macOS reaches a visible login/desktop path or iOS reaches a visible SpringBoard path.

Only after that baseline should correctness/performance work expand Metal feature coverage.

## Networking, Wi-Fi, and Bluetooth

These should be developed in parallel with graphics but do not need to block the first visible UI.
The pragmatic order is:

1. generic guest networking sufficient for TCP/IP;
2. Apple-facing network interface compatibility;
3. Wi-Fi control-plane emulation backed by the host network;
4. Bluetooth HCI/controller emulation backed by a host adapter or a virtual controller.

The first goal is functional OS services, not radio-accurate hardware simulation.
