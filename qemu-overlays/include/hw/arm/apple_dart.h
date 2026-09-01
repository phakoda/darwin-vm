/*
 * Minimal Apple DART IOMMU model for darwin-vm.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_ARM_APPLE_DART_H
#define HW_ARM_APPLE_DART_H

#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "qom/object.h"

#define TYPE_APPLE_DART "apple-dart"
OBJECT_DECLARE_SIMPLE_TYPE(AppleDARTState, APPLE_DART)

#define TYPE_APPLE_DART_IOMMU "apple-dart-iommu"
OBJECT_DECLARE_SIMPLE_TYPE(AppleDARTIOMMUMemoryRegion, APPLE_DART_IOMMU)

/*
 * Create and map a T8020/T8030-style Apple DART with up to two MMIO
 * instances. Callers select the instance/SID used by a peripheral through
 * apple_dart_get_iommu(). Newer T8110-style DARTs use a different register
 * layout and are intentionally outside this first USB bring-up model.
 */
AppleDARTState *apple_dart_create(const char *name,
                                  uint32_t page_size,
                                  uint32_t sids,
                                  uint32_t bypass,
                                  uint64_t bypass_address,
                                  uint32_t num_instances,
                                  const hwaddr *bases,
                                  const uint64_t *sizes,
                                  qemu_irq fault_irq);

IOMMUMemoryRegion *apple_dart_get_iommu(AppleDARTState *s,
                                        uint32_t instance,
                                        uint32_t sid);

#endif /* HW_ARM_APPLE_DART_H */
