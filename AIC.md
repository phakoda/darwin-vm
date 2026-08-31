# Apple Interrupt Controller bring-up

A functional interrupt controller is a prerequisite for interactive devices in the Darwin machine.
The existing QEMU machine only maps zero-filled AIC registers and reports an IRQ count; the ARM timer
is wired directly to FIQ. That is enough for the current root-shell boot, but it gives USB, network,
input, display, and GPU devices no way to signal XNU.

## First implementation target

The first AIC implementation stays deliberately narrow:

- one CPU, matching the current Darwin machine's `max_cpus = 1`;
- one die;
- AIC v1, v2, and v3 MMIO layouts;
- level-triggered hardware interrupt inputs;
- software set/clear registers;
- mask set/clear registers;
- per-IRQ target/config storage;
- event delivery to the CPU IRQ input;
- event reads automatically mask the delivered IRQ, matching hardware;
- lower interrupt numbers have priority;
- timer FIQ remains directly wired as it is today.

IPIs, multi-CPU affinity, multi-die routing, PMU FIQs, and MSI domains can follow when the machine
actually grows those features.

## Register behavior used

Linux's Apple AIC driver documents the essential hardware contract. For AIC v1, the register blocks
are derived from a 0x400-IRQ maximum and land at:

- `0x2004`: event/reason;
- `0x3000`: target CPU array;
- `0x4000`: software set;
- `0x4080`: software clear;
- `0x4100`: mask set;
- `0x4180`: mask clear;
- `0x4200`: hardware line state.

For AIC v2/v3, the per-IRQ configuration block starts at `0x2000`/`0x10000` respectively, followed
by software-set, software-clear, mask-set, mask-clear, and hardware-state bitmaps. The event register
is provided through the second AIC register range.

An IRQ event is encoded with event type `1` and the interrupt number in the low 16 bits. Reading the
event register acknowledges and automatically masks the interrupt. The guest subsequently unmasks it
through the mask-clear register as part of EOI.

## Why this comes before USB/networking

QEMU already has reusable USB and network device models, but adding one before AIC would only create
a device whose interrupt line goes nowhere. Once AIC exposes real per-IRQ inputs, subsequent patches
can connect emulated peripherals to the interrupt numbers already described by the Apple device tree.
