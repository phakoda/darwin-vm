/*
 * Apple Interrupt Controller model for the Darwin machine
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_ARM_APPLE_AIC_H
#define HW_ARM_APPLE_AIC_H

#include "hw/core/irq.h"
#include "hw/core/sysbus.h"

#define TYPE_APPLE_AIC "apple-aic"
OBJECT_DECLARE_SIMPLE_TYPE(AppleAICState, APPLE_AIC)

AppleAICState *apple_aic_create(uint32_t version,
                                uint32_t nr_irq,
                                uint32_t max_irq,
                                hwaddr base,
                                uint64_t base_size,
                                hwaddr event_base,
                                uint64_t event_size,
                                qemu_irq cpu_irq);

qemu_irq apple_aic_get_irq(AppleAICState *s, uint32_t irq);

#endif
