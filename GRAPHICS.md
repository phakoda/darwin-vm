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

## Probe the guest graphics stack

Before choosing a GPU implementation for a firmware target, inspect the extracted BootKC:

```bash
./scripts/probe_graphics_support.sh firmware/bootkc
```

The probe looks for stock `AppleParavirtGPU` / `AppleParavirtDisplay`, AGX-family, and IOSurface
markers. It is deliberately a heuristic string scan rather than a kext parser, but a positive PVG
result is useful evidence that the guest may be able to bind a Reims-compatible paravirtual GPU
without a custom kernel extension.

This matters because the macOS and iOS paths may diverge: a Mac kernel collection with the stock
Apple paravirtual drivers can use the PVG protocol, while a target that only ships AGX drivers needs
AGX emulation or a guest-side compatibility layer.

## Milestone 2: functional device interrupts

The current `darwin` QEMU machine only allocates zeroed memory for Apple's AIC. That is sufficient
for the existing serial/root-shell bring-up because the platform timer is hard-wired to FIQ, but it
cannot deliver ordinary device IRQs. A real GPU, network interface, Wi-Fi device, Bluetooth
controller, keyboard, or pointing device needs a working interrupt path.

`patches/qemu-sptm/0002-darwin-aic-v1.patch` starts with an opt-in AIC v1 implementation. For an
AIC v1 firmware target, enable it with:

```bash
AIC_V1=1 ./run.sh
```

The initial device implements:

- hardware IRQ level state;
- mask-set / mask-clear registers;
- software-set / software-clear state;
- the event register and its automatic mask-on-delivery behavior;
- one CPU target;
- 1024 QEMU GPIO interrupt inputs and a CPU IRQ output.

AIC2/AIC3 support can extend the same device model with their per-die register layout. Keeping this
as a reusable QEMU device is important: Reims, networking, Wi-Fi, Bluetooth and HID should all
connect to the same interrupt controller rather than inventing one-off notification paths.

## Milestone 3: display/input services

After the framebuffer and interrupt path are proven on real firmware:

1. derive native panel dimensions/rotation/scale from the selected device tree instead of using a
   fixed development resolution;
2. expose host keyboard/mouse/touch events through an Apple-compatible HID path;
3. model enough display/IOSurface/IOMobileFramebuffer behavior for higher-level display services to
   keep running rather than falling back to the boot console;
4. add screenshot and deterministic framebuffer tests to make display regressions observable in CI.

## Milestone 4: accelerated Metal via Reims + Vulkan

A full AGX command-processor implementation is a very large reverse-engineering project. There is
now a stronger bring-up path than a new Metal API shim: reuse the existing Apple paravirtual GPU
protocol implemented by Reims where the guest already contains Apple's stock PVG drivers.

The relevant upstream projects are:

- `steelbrain-bot/reims-vgpu`: a host implementation of the protocol consumed by
  `AppleParavirtGPU.kext`, with a Vulkan backend and IOSurface/display handling;
- `steelbrain/qemu-reims-vgpu`: thin QEMU PCI/MMIO device shims that connect guest MMIO/IRQs and
  guest memory to Reims;
- `steelbrain/metal2vulkan`: Metal AIR → Vulkan 1.2 SPIR-V translation used by the Vulkan renderer;
- `steelbrain/experiment-macOS-arm64-on-asahi-linux-arm64`: evidence that this stack can bring an
  ARM64 macOS guest on Asahi Linux to graphical Setup Assistant/WindowServer using Vulkan.

For `darwin-vm`, the intended Mac-side split is therefore:

```text
macOS app / CoreAnimation / WindowServer
              |
      Apple Metal / IOSurface stack
              |
    AppleParavirtGPU + Display
              |
        PVG MMIO + IRQ protocol
              |
       Reims QEMU thin shim
              |
       Reims Vulkan backend
              |
 metal2vulkan (AIR -> SPIR-V)
              |
          host Vulkan
              |
         Linux display
```

This does **not** mean Reims automatically solves iPhone graphics. We first need to probe whether a
selected iOS kernel collection contains the Apple PVG drivers. If it does not, the iOS path remains
AGX emulation or an explicit guest compatibility layer.

### Why keep AGX emulation as a second path?

Some system components may talk to IOGPU/AGX interfaces below Metal.framework or a firmware target
may not ship the paravirtual GPU driver at all. For those cases we should reuse public AGX
reverse-engineering work (where licensing permits) to model the minimum kernel/user-client ABI. The
emulator can then choose between:

- **Apple PVG/Reims/Vulkan** for fast GUI bring-up on compatible guests; and
- progressively more faithful **AGX device emulation** for compatibility.

## Milestone 5: WindowServer / SpringBoard

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
