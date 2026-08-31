/*
 * Apple Interrupt Controller model for the Darwin machine
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Event encoding and mask/ack behavior follow the public Linux
 * irq-apple-aic driver and Asahi Linux hardware documentation. Public
 * Apple-Silicon QEMU work was also used as a cross-check for the v1 MMIO
 * layout and XNU-facing event values.
 *
 * The first implementation is intentionally single-CPU/single-die, matching
 * darwin-vm today. It is designed to become the interrupt fabric for USB,
 * networking, HID, display, and GPU devices as those models are added.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/arm/apple_aic.h"
#include "qemu/bitops.h"
#include "qemu/module.h"

#define AIC1_INFO                  0x0004
#define AIC1_RESET                 0x000c
#define AIC1_CONFIG                0x0010
#define AIC1_WHOAMI                0x2000
#define AIC1_EVENT                 0x2004
#define AIC1_IPI_SEND              0x2008
#define AIC1_IPI_ACK               0x200c
#define AIC1_IPI_MASK_SET          0x2024
#define AIC1_IPI_MASK_CLR          0x2028
#define AIC1_TARGET_CPU            0x3000
#define AIC1_MAX_IRQ               0x400

#define AIC2_VERSION               0x0000
#define AIC2_INFO1                 0x0004
#define AIC2_INFO2                 0x0008
#define AIC2_INFO3                 0x000c
#define AIC2_RESET                 0x0010
#define AIC2_CONFIG                0x0014
#define AIC2_CONFIG_ENABLE         BIT(0)
#define AIC2_IRQ_CFG               0x2000
#define AIC3_IRQ_CFG               0x10000

#define AIC_EVENT_TYPE_IRQ         1
#define AIC_EVENT_TYPE_IPI         4
#define AIC_EVENT_IRQ(n)           ((AIC_EVENT_TYPE_IRQ << 16) | ((n) & 0xffff))
#define AIC_EVENT_IPI_OTHER        ((AIC_EVENT_TYPE_IPI << 16) | 1)
#define AIC_EVENT_IPI_SELF         ((AIC_EVENT_TYPE_IPI << 16) | 2)

#define AIC_IPI_OTHER              BIT(0)
#define AIC_IPI_SELF               BIT(31)

struct AppleAICState {
    SysBusDevice parent_obj;

    MemoryRegion regs;
    MemoryRegion event_regs;
    qemu_irq cpu_irq;

    uint32_t version;
    uint32_t nr_irq;
    uint32_t max_irq;
    uint32_t num_words;

    uint64_t base_size;
    uint64_t event_size;
    bool separate_event;
    bool enabled;

    uint32_t config;
    uint32_t irq_cfg_base;
    uint32_t sw_set;
    uint32_t sw_clr;
    uint32_t mask_set;
    uint32_t mask_clr;
    uint32_t hw_state;

    uint32_t *mask;
    uint32_t *hw_level;
    uint32_t *pending;
    uint32_t *sw_pending;
    uint32_t *route;

    uint32_t ipi_pending;
    uint32_t ipi_mask;
};

static inline uint32_t aic_word(uint32_t irq)
{
    return irq >> 5;
}

static inline uint32_t aic_bit(uint32_t irq)
{
    return BIT(irq & 31);
}

static bool apple_aic_irq_targets_cpu0(AppleAICState *s, uint32_t irq)
{
    if (s->version == 1) {
        return s->route[irq] & BIT(0);
    }

    /* AIC2/3 target value zero is CPU0; there is only one CPU for now. */
    return true;
}

static bool apple_aic_has_deliverable_irq(AppleAICState *s)
{
    uint32_t word;

    if (s->version >= 2 && !s->enabled) {
        return false;
    }

    if (s->version == 1 &&
        (s->ipi_pending & ~s->ipi_mask & (AIC_IPI_OTHER | AIC_IPI_SELF))) {
        return true;
    }

    for (word = 0; word < s->num_words; word++) {
        uint32_t active = s->pending[word] & ~s->mask[word];

        while (active) {
            uint32_t bitno = __builtin_ctz(active);
            uint32_t irq = word * 32 + bitno;

            if (irq < s->nr_irq && apple_aic_irq_targets_cpu0(s, irq)) {
                return true;
            }
            active &= ~BIT(bitno);
        }
    }

    return false;
}

static void apple_aic_update(AppleAICState *s)
{
    qemu_set_irq(s->cpu_irq, apple_aic_has_deliverable_irq(s));
}

static void apple_aic_reset_state(AppleAICState *s)
{
    memset(s->mask, 0xff, sizeof(*s->mask) * s->num_words);
    memset(s->hw_level, 0, sizeof(*s->hw_level) * s->num_words);
    memset(s->pending, 0, sizeof(*s->pending) * s->num_words);
    memset(s->sw_pending, 0, sizeof(*s->sw_pending) * s->num_words);
    memset(s->route, 0, sizeof(*s->route) * s->max_irq);

    s->config = 0;
    s->enabled = s->version == 1;
    s->ipi_pending = 0;
    s->ipi_mask = AIC_IPI_OTHER | AIC_IPI_SELF;
    apple_aic_update(s);
}

static void apple_aic_reset(DeviceState *dev)
{
    apple_aic_reset_state(APPLE_AIC(dev));
}

static void apple_aic_set_irq(void *opaque, int irq, int level)
{
    AppleAICState *s = APPLE_AIC(opaque);
    uint32_t word;
    uint32_t bit;

    if (irq < 0 || irq >= s->nr_irq) {
        return;
    }

    word = aic_word(irq);
    bit = aic_bit(irq);

    if (level) {
        s->hw_level[word] |= bit;
        s->pending[word] |= bit;
    } else {
        s->hw_level[word] &= ~bit;
    }

    apple_aic_update(s);
}

static uint32_t apple_aic_ack(AppleAICState *s)
{
    uint32_t word;

    if (s->version == 1) {
        uint32_t active_ipi = s->ipi_pending & ~s->ipi_mask;

        if (active_ipi & AIC_IPI_SELF) {
            s->ipi_mask |= AIC_IPI_SELF;
            apple_aic_update(s);
            return AIC_EVENT_IPI_SELF;
        }
        if (active_ipi & AIC_IPI_OTHER) {
            s->ipi_mask |= AIC_IPI_OTHER;
            apple_aic_update(s);
            return AIC_EVENT_IPI_OTHER;
        }
    }

    if (s->version >= 2 && !s->enabled) {
        return 0;
    }

    for (word = 0; word < s->num_words; word++) {
        uint32_t active = s->pending[word] & ~s->mask[word];

        while (active) {
            uint32_t bitno = __builtin_ctz(active);
            uint32_t bit = BIT(bitno);
            uint32_t irq = word * 32 + bitno;

            if (irq < s->nr_irq && apple_aic_irq_targets_cpu0(s, irq)) {
                /* Event reads acknowledge and auto-mask the selected IRQ. */
                s->pending[word] &= ~bit;
                s->mask[word] |= bit;
                apple_aic_update(s);
                return AIC_EVENT_IRQ(irq);
            }
            active &= ~bit;
        }
    }

    apple_aic_update(s);
    return 0;
}

static bool apple_aic_bitmap_offset(AppleAICState *s, hwaddr addr,
                                    uint32_t base, uint32_t *word)
{
    hwaddr end = base + s->num_words * sizeof(uint32_t);

    if (addr < base || addr >= end || (addr & 3)) {
        return false;
    }

    *word = (addr - base) / sizeof(uint32_t);
    return true;
}

static uint64_t apple_aic_read(void *opaque, hwaddr addr, unsigned size)
{
    AppleAICState *s = APPLE_AIC(opaque);
    uint32_t word;

    (void)size;

    if (s->version == 1) {
        if (addr == 0x0000) {
            return 1;
        }
        if (addr == AIC1_INFO) {
            return s->nr_irq & 0xffff;
        }
        if (addr == AIC1_CONFIG) {
            return s->config;
        }
        if (addr == AIC1_WHOAMI) {
            return 0;
        }
        if (addr == AIC1_EVENT) {
            return apple_aic_ack(s);
        }
        if (addr >= AIC1_TARGET_CPU &&
            addr < AIC1_TARGET_CPU + s->max_irq * sizeof(uint32_t) &&
            !(addr & 3)) {
            return s->route[(addr - AIC1_TARGET_CPU) / sizeof(uint32_t)];
        }
    } else {
        if (addr == AIC2_VERSION) {
            return s->version;
        }
        if (addr == AIC2_INFO1) {
            return s->nr_irq & 0xffff;
        }
        if (addr == AIC2_INFO2) {
            return 0;
        }
        if (addr == AIC2_INFO3) {
            return s->max_irq & 0xffff;
        }
        if (addr == AIC2_CONFIG) {
            return s->config;
        }
        if (addr >= s->irq_cfg_base &&
            addr < s->irq_cfg_base + s->max_irq * sizeof(uint32_t) &&
            !(addr & 3)) {
            return s->route[(addr - s->irq_cfg_base) / sizeof(uint32_t)];
        }
    }

    if (apple_aic_bitmap_offset(s, addr, s->mask_set, &word) ||
        apple_aic_bitmap_offset(s, addr, s->mask_clr, &word)) {
        return s->mask[word];
    }

    if (apple_aic_bitmap_offset(s, addr, s->hw_state, &word)) {
        return s->hw_level[word];
    }

    if (apple_aic_bitmap_offset(s, addr, s->sw_set, &word) ||
        apple_aic_bitmap_offset(s, addr, s->sw_clr, &word)) {
        return s->sw_pending[word];
    }

    return 0;
}

static void apple_aic_repend_sources(AppleAICState *s, uint32_t word,
                                     uint32_t bits)
{
    s->pending[word] |=
        (s->hw_level[word] | s->sw_pending[word]) & bits;
}

static void apple_aic_write(void *opaque, hwaddr addr, uint64_t value,
                            unsigned size)
{
    AppleAICState *s = APPLE_AIC(opaque);
    uint32_t val = value;
    uint32_t word;

    (void)size;

    if (s->version == 1) {
        if (addr == AIC1_RESET) {
            if (val & BIT(0)) {
                apple_aic_reset_state(s);
            }
            return;
        }
        if (addr == AIC1_CONFIG) {
            s->config = val;
            apple_aic_update(s);
            return;
        }
        if (addr == AIC1_IPI_SEND) {
            if (val & BIT(0)) {
                s->ipi_pending |= AIC_IPI_OTHER;
            }
            if (val & AIC_IPI_SELF) {
                s->ipi_pending |= AIC_IPI_SELF;
            }
            apple_aic_update(s);
            return;
        }
        if (addr == AIC1_IPI_ACK) {
            s->ipi_pending &= ~val;
            apple_aic_update(s);
            return;
        }
        if (addr == AIC1_IPI_MASK_SET) {
            s->ipi_mask |= val & (AIC_IPI_OTHER | AIC_IPI_SELF);
            apple_aic_update(s);
            return;
        }
        if (addr == AIC1_IPI_MASK_CLR) {
            s->ipi_mask &= ~(val & (AIC_IPI_OTHER | AIC_IPI_SELF));
            apple_aic_update(s);
            return;
        }
        if (addr >= AIC1_TARGET_CPU &&
            addr < AIC1_TARGET_CPU + s->max_irq * sizeof(uint32_t) &&
            !(addr & 3)) {
            s->route[(addr - AIC1_TARGET_CPU) / sizeof(uint32_t)] = val;
            apple_aic_update(s);
            return;
        }
    } else {
        if (addr == AIC2_RESET) {
            if (val & BIT(0)) {
                apple_aic_reset_state(s);
            }
            return;
        }
        if (addr == AIC2_CONFIG) {
            s->config = val;
            s->enabled = val & AIC2_CONFIG_ENABLE;
            apple_aic_update(s);
            return;
        }
        if (addr >= s->irq_cfg_base &&
            addr < s->irq_cfg_base + s->max_irq * sizeof(uint32_t) &&
            !(addr & 3)) {
            s->route[(addr - s->irq_cfg_base) / sizeof(uint32_t)] = val;
            apple_aic_update(s);
            return;
        }
    }

    if (apple_aic_bitmap_offset(s, addr, s->sw_set, &word)) {
        s->sw_pending[word] |= val;
        s->pending[word] |= val;
        apple_aic_update(s);
        return;
    }

    if (apple_aic_bitmap_offset(s, addr, s->sw_clr, &word)) {
        s->sw_pending[word] &= ~val;
        s->pending[word] &= ~(val & ~s->hw_level[word]);
        apple_aic_update(s);
        return;
    }

    if (apple_aic_bitmap_offset(s, addr, s->mask_set, &word)) {
        s->mask[word] |= val;
        apple_aic_update(s);
        return;
    }

    if (apple_aic_bitmap_offset(s, addr, s->mask_clr, &word)) {
        s->mask[word] &= ~val;
        apple_aic_repend_sources(s, word, val);
        apple_aic_update(s);
        return;
    }
}

static uint64_t apple_aic_event_read(void *opaque, hwaddr addr, unsigned size)
{
    AppleAICState *s = APPLE_AIC(opaque);

    (void)size;
    return addr == 0 ? apple_aic_ack(s) : 0;
}

static void apple_aic_event_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned size)
{
    (void)opaque;
    (void)addr;
    (void)value;
    (void)size;
}

static const MemoryRegionOps apple_aic_ops = {
    .read = apple_aic_read,
    .write = apple_aic_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static const MemoryRegionOps apple_aic_event_ops = {
    .read = apple_aic_event_read,
    .write = apple_aic_event_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void apple_aic_realize(DeviceState *dev, Error **errp)
{
    AppleAICState *s = APPLE_AIC(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    if (!s->nr_irq || !s->max_irq || s->nr_irq > s->max_irq) {
        error_setg(errp, "apple-aic: invalid IRQ geometry %u/%u",
                   s->nr_irq, s->max_irq);
        return;
    }

    if (s->version < 1 || s->version > 3) {
        error_setg(errp, "apple-aic: unsupported version %u", s->version);
        return;
    }

    s->num_words = DIV_ROUND_UP(s->max_irq, 32);
    s->mask = g_new0(uint32_t, s->num_words);
    s->hw_level = g_new0(uint32_t, s->num_words);
    s->pending = g_new0(uint32_t, s->num_words);
    s->sw_pending = g_new0(uint32_t, s->num_words);
    s->route = g_new0(uint32_t, s->max_irq);

    if (s->version == 1) {
        s->irq_cfg_base = AIC1_TARGET_CPU;
        s->sw_set = AIC1_TARGET_CPU + AIC1_MAX_IRQ * sizeof(uint32_t);
    } else {
        s->irq_cfg_base = s->version == 2 ? AIC2_IRQ_CFG : AIC3_IRQ_CFG;
        s->sw_set = s->irq_cfg_base + s->max_irq * sizeof(uint32_t);
    }

    s->sw_clr = s->sw_set + s->num_words * sizeof(uint32_t);
    s->mask_set = s->sw_clr + s->num_words * sizeof(uint32_t);
    s->mask_clr = s->mask_set + s->num_words * sizeof(uint32_t);
    s->hw_state = s->mask_clr + s->num_words * sizeof(uint32_t);

    memory_region_init_io(&s->regs, OBJECT(dev), &apple_aic_ops, s,
                          "apple-aic", s->base_size);
    sysbus_init_mmio(sbd, &s->regs);

    if (s->separate_event) {
        memory_region_init_io(&s->event_regs, OBJECT(dev),
                              &apple_aic_event_ops, s,
                              "apple-aic-event", s->event_size);
        sysbus_init_mmio(sbd, &s->event_regs);
    }

    sysbus_init_irq(sbd, &s->cpu_irq);
    qdev_init_gpio_in(dev, apple_aic_set_irq, s->nr_irq);
    apple_aic_reset_state(s);
}

static void apple_aic_unrealize(DeviceState *dev)
{
    AppleAICState *s = APPLE_AIC(dev);

    g_free(s->mask);
    g_free(s->hw_level);
    g_free(s->pending);
    g_free(s->sw_pending);
    g_free(s->route);
}

static void apple_aic_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    (void)data;
    dc->realize = apple_aic_realize;
    dc->unrealize = apple_aic_unrealize;
    device_class_set_legacy_reset(dc, apple_aic_reset);
    dc->desc = "Apple Interrupt Controller";
}

static const TypeInfo apple_aic_type_info = {
    .name = TYPE_APPLE_AIC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AppleAICState),
    .class_init = apple_aic_class_init,
};

static void apple_aic_register_types(void)
{
    type_register_static(&apple_aic_type_info);
}

type_init(apple_aic_register_types)

AppleAICState *apple_aic_create(uint32_t version,
                                uint32_t nr_irq,
                                uint32_t max_irq,
                                hwaddr base,
                                uint64_t base_size,
                                hwaddr event_base,
                                uint64_t event_size,
                                qemu_irq cpu_irq)
{
    DeviceState *dev = qdev_new(TYPE_APPLE_AIC);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    AppleAICState *s = APPLE_AIC(dev);

    s->version = version;
    s->nr_irq = nr_irq;
    s->max_irq = max_irq;
    s->base_size = base_size;
    s->event_size = event_size;
    s->separate_event = version >= 2 && event_size != 0;

    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, base);
    if (s->separate_event) {
        sysbus_mmio_map(sbd, 1, event_base);
    }
    sysbus_connect_irq(sbd, 0, cpu_irq);

    return s;
}

qemu_irq apple_aic_get_irq(AppleAICState *s, uint32_t irq)
{
    g_assert_cmpuint(irq, <, s->nr_irq);
    return qdev_get_gpio_in(DEVICE(s), irq);
}
