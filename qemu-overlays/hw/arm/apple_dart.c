/*
 * Minimal Apple DART IOMMU model for darwin-vm.
 *
 * This models the translation path needed by DMA-capable Apple peripherals:
 * stream TCR/TTBR programming, 4K/16K page-table walks, bypass, permission
 * checks, TLB invalidation, and fault reporting.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/arm/apple_dart.h"
#include "qapi/error.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/address-spaces.h"
#include "system/dma.h"

#define DART_MAX_INSTANCES 2
#define DART_MAX_STREAMS   16
#define DART_MAX_TTBRS     4
#define DART_REG_SIZE      0x4000
#define DART_MAX_VA_BITS   38

#define DART_PARAMS1             0x0000
#define DART_PARAMS1_PAGE_SHIFT  24
#define DART_PARAMS2             0x0004
#define DART_PARAMS2_BYPASS      BIT(0)
#define DART_TLB_OP              0x0020
#define DART_TLB_OP_BUSY         BIT(2)
#define DART_TLB_OP_INVALIDATE   BIT(20)
#define DART_SID_MASK_LOW        0x0034
#define DART_SID_MASK_HIGH       0x0038
#define DART_ERROR_STATUS        0x0040
#define DART_ERROR_ADDRESS_LO    0x0050
#define DART_ERROR_ADDRESS_HI    0x0054
#define DART_SID_REMAP(sid)      (0x0080 + 4 * (sid))
#define DART_TCR(sid)            (0x0100 + 4 * (sid))
#define DART_TCR_TXEN            BIT(7)
#define DART_TCR_BYPASS_DART     BIT(8)
#define DART_TTBR(sid, idx)      (0x0200 + 16 * (sid) + 4 * (idx))
#define DART_TTBR_VALID          BIT(31)
#define DART_TTBR_SHIFT          12
#define DART_TTBR_MASK           0x0fffffffU

#define DART_ERROR_TTBR_INVALID  BIT(0)
#define DART_ERROR_L2_INVALID    BIT(1)
#define DART_ERROR_PTE_INVALID   BIT(2)
#define DART_ERROR_WRITE_PROT    BIT(3)
#define DART_ERROR_READ_PROT     BIT(4)
#define DART_ERROR_STREAM_SHIFT  24
#define DART_ERROR_FLAG          BIT(31)

#define DART_PTE_VALID           BIT_ULL(0)
#define DART_PTE_NO_WRITE        BIT_ULL(7)
#define DART_PTE_NO_READ         BIT_ULL(8)
#define DART_PTE_ADDR_MASK       0x000000ffffffffffULL

struct AppleDARTState;
typedef struct AppleDARTInstance AppleDARTInstance;

typedef struct AppleDARTTLBEntry {
    hwaddr pa_page;
    IOMMUAccessFlags perm;
} AppleDARTTLBEntry;

struct AppleDARTIOMMUMemoryRegion {
    IOMMUMemoryRegion parent_obj;
    AppleDARTInstance *instance;
    uint32_t sid;
};

struct AppleDARTInstance {
    struct AppleDARTState *dart;
    uint32_t id;
    MemoryRegion mmio;
    uint32_t regs[DART_REG_SIZE / sizeof(uint32_t)];
    AppleDARTIOMMUMemoryRegion *iommu[DART_MAX_STREAMS];
    GHashTable *tlb;
    QemuMutex lock;
};

struct AppleDARTState {
    SysBusDevice parent_obj;
    char *name;
    qemu_irq fault_irq;
    AppleDARTInstance instance[DART_MAX_INSTANCES];
    uint32_t num_instances;
    uint32_t page_size;
    uint32_t page_shift;
    hwaddr page_mask;
    hwaddr page_offset_mask;
    uint32_t level_mask[3];
    uint32_t level_shift[3];
    uint32_t sids;
    uint32_t bypass;
    uint64_t bypass_address;
};

static void apple_dart_update_irq(AppleDARTState *s)
{
    bool fault = false;

    for (uint32_t i = 0; i < s->num_instances; i++) {
        fault |= s->instance[i].regs[DART_ERROR_STATUS >> 2] != 0;
    }
    qemu_set_irq(s->fault_irq, fault);
}

static uint64_t dart_tlb_key(uint32_t sid, hwaddr iova_page)
{
    return ((uint64_t)sid << 56) | iova_page;
}

static void apple_dart_flush_instance(AppleDARTInstance *o)
{
    IOMMUTLBEvent event = {
        .type = IOMMU_NOTIFIER_UNMAP,
        .entry = {
            .target_as = &address_space_memory,
            .iova = 0,
            .translated_addr = 0,
            .addr_mask = ~(hwaddr)0,
            .perm = IOMMU_NONE,
        },
    };

    g_hash_table_remove_all(o->tlb);
    for (uint32_t sid = 0; sid < DART_MAX_STREAMS; sid++) {
        if (o->iommu[sid]) {
            memory_region_notify_iommu(IOMMU_MEMORY_REGION(o->iommu[sid]),
                                       0, event);
        }
    }
}

static void apple_dart_set_fault(AppleDARTInstance *o, uint32_t sid,
                                 hwaddr addr, uint32_t reason)
{
    uint32_t *status = &o->regs[DART_ERROR_STATUS >> 2];

    *status |= DART_ERROR_FLAG | reason |
               ((sid & 0xf) << DART_ERROR_STREAM_SHIFT);
    o->regs[DART_ERROR_ADDRESS_LO >> 2] = addr;
    o->regs[DART_ERROR_ADDRESS_HI >> 2] = addr >> 32;
    apple_dart_update_irq(o->dart);
}

static uint32_t apple_dart_stream_remap(AppleDARTInstance *o, uint32_t sid)
{
    return o->regs[DART_SID_REMAP(sid) >> 2] & 0xf;
}

static uint32_t apple_dart_stream_tcr(AppleDARTInstance *o, uint32_t sid)
{
    return o->regs[DART_TCR(sid) >> 2];
}

static uint32_t apple_dart_stream_ttbr(AppleDARTInstance *o,
                                       uint32_t sid, uint32_t idx)
{
    return o->regs[DART_TTBR(sid, idx) >> 2];
}

static AppleDARTTLBEntry *apple_dart_walk(AppleDARTInstance *o,
                                          uint32_t sid,
                                          hwaddr iova_page,
                                          uint32_t *fault_reason)
{
    AppleDARTState *s = o->dart;
    uint64_t pte = 0;
    uint32_t root = (iova_page & s->level_mask[0]) >> s->level_shift[0];
    uint32_t ttbr;
    hwaddr table;

    if (root >= DART_MAX_TTBRS) {
        *fault_reason = DART_ERROR_TTBR_INVALID;
        return NULL;
    }

    ttbr = apple_dart_stream_ttbr(o, sid, root);
    if (!(ttbr & DART_TTBR_VALID)) {
        *fault_reason = DART_ERROR_TTBR_INVALID;
        return NULL;
    }
    table = (hwaddr)(ttbr & DART_TTBR_MASK) << DART_TTBR_SHIFT;

    for (int level = 1; level < 3; level++) {
        uint32_t index = (iova_page & s->level_mask[level]) >>
                         s->level_shift[level];
        uint64_t raw;
        hwaddr pte_addr = table + (hwaddr)index * sizeof(uint64_t);

        if (dma_memory_read(&address_space_memory, pte_addr, &raw, sizeof(raw),
                            MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            *fault_reason = DART_ERROR_L2_INVALID;
            return NULL;
        }
        pte = le64_to_cpu(raw);
        if (!(pte & DART_PTE_VALID)) {
            *fault_reason = (level == 1) ? DART_ERROR_L2_INVALID
                                         : DART_ERROR_PTE_INVALID;
            return NULL;
        }
        table = pte & s->page_mask & DART_PTE_ADDR_MASK;
    }

    AppleDARTTLBEntry *entry = g_new0(AppleDARTTLBEntry, 1);
    entry->pa_page = pte & s->page_mask & DART_PTE_ADDR_MASK;
    entry->perm = IOMMU_ACCESS_FLAG(!(pte & DART_PTE_NO_READ),
                                    !(pte & DART_PTE_NO_WRITE));
    return entry;
}

static int apple_dart_attrs_to_index(IOMMUMemoryRegion *iommu,
                                     MemTxAttrs attrs)
{
    return 0;
}

static IOMMUTLBEntry apple_dart_translate(IOMMUMemoryRegion *mr,
                                          hwaddr addr,
                                          IOMMUAccessFlags access,
                                          int iommu_idx)
{
    AppleDARTIOMMUMemoryRegion *iommu = APPLE_DART_IOMMU(mr);
    AppleDARTInstance *o = iommu->instance;
    AppleDARTState *s = o->dart;
    uint32_t original_sid = iommu->sid;
    uint32_t sid;
    uint32_t tcr;
    hwaddr iova_page = addr >> s->page_shift;
    uint64_t key_value;
    AppleDARTTLBEntry *cached;
    IOMMUTLBEntry result = {
        .target_as = &address_space_memory,
        .iova = addr,
        .translated_addr = s->bypass_address + addr,
        .addr_mask = s->page_offset_mask,
        .perm = IOMMU_RW,
    };

    qemu_mutex_lock(&o->lock);
    sid = apple_dart_stream_remap(o, original_sid);
    tcr = apple_dart_stream_tcr(o, sid);

    if ((s->bypass & BIT(sid)) || !(tcr & DART_TCR_TXEN) ||
        (tcr & DART_TCR_BYPASS_DART)) {
        goto out;
    }

    key_value = dart_tlb_key(sid, iova_page);
    cached = g_hash_table_lookup(o->tlb, &key_value);
    if (!cached) {
        uint32_t fault_reason = 0;
        uint64_t *stored_key;

        cached = apple_dart_walk(o, sid, iova_page, &fault_reason);
        if (!cached) {
            result.perm = IOMMU_NONE;
            apple_dart_set_fault(o, original_sid, addr, fault_reason);
            goto out;
        }

        stored_key = g_new(uint64_t, 1);
        *stored_key = key_value;
        g_hash_table_insert(o->tlb, stored_key, cached);
    }

    result.translated_addr = cached->pa_page | (addr & s->page_offset_mask);
    result.perm = cached->perm;

    if ((access & IOMMU_WO) && !(result.perm & IOMMU_WO)) {
        result.perm = IOMMU_NONE;
        apple_dart_set_fault(o, original_sid, addr, DART_ERROR_WRITE_PROT);
    } else if ((access & IOMMU_RO) && !(result.perm & IOMMU_RO)) {
        result.perm = IOMMU_NONE;
        apple_dart_set_fault(o, original_sid, addr, DART_ERROR_READ_PROT);
    }

out:
    qemu_mutex_unlock(&o->lock);
    return result;
}

static uint64_t apple_dart_mmio_read(void *opaque, hwaddr offset,
                                     unsigned size)
{
    AppleDARTInstance *o = opaque;

    if (offset >= DART_REG_SIZE || size != 4) {
        return 0;
    }
    return o->regs[offset >> 2];
}

static void apple_dart_mmio_write(void *opaque, hwaddr offset,
                                  uint64_t value, unsigned size)
{
    AppleDARTInstance *o = opaque;
    AppleDARTState *s = o->dart;
    uint32_t val = value;

    if (offset >= DART_REG_SIZE || size != 4) {
        return;
    }

    qemu_mutex_lock(&o->lock);
    switch (offset) {
    case DART_TLB_OP:
        if (val & DART_TLB_OP_INVALIDATE) {
            o->regs[DART_TLB_OP >> 2] = DART_TLB_OP_BUSY;
            apple_dart_flush_instance(o);
            o->regs[DART_TLB_OP >> 2] = 0;
        } else {
            o->regs[DART_TLB_OP >> 2] = val & ~DART_TLB_OP_BUSY;
        }
        break;
    case DART_ERROR_STATUS:
        o->regs[DART_ERROR_STATUS >> 2] &= ~val;
        break;
    default:
        o->regs[offset >> 2] = val;
        break;
    }
    qemu_mutex_unlock(&o->lock);
    apple_dart_update_irq(s);
}

static const MemoryRegionOps apple_dart_mmio_ops = {
    .read = apple_dart_mmio_read,
    .write = apple_dart_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
};

static void apple_dart_init_instance(AppleDARTState *s,
                                     AppleDARTInstance *o,
                                     uint32_t id,
                                     uint64_t mmio_size)
{
    o->dart = s;
    o->id = id;
    qemu_mutex_init(&o->lock);
    o->tlb = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);

    memory_region_init_io(&o->mmio, OBJECT(s), &apple_dart_mmio_ops, o,
                          TYPE_APPLE_DART ".regs",
                          MIN(mmio_size, (uint64_t)DART_REG_SIZE));
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &o->mmio);

    for (uint32_t sid = 0; sid < DART_MAX_STREAMS; sid++) {
        if (!(s->sids & BIT(sid))) {
            continue;
        }

        g_autofree char *name = g_strdup_printf("%s.%u.%u",
                                                s->name ? s->name : "dart",
                                                id, sid);
        o->iommu[sid] = g_new0(AppleDARTIOMMUMemoryRegion, 1);
        o->iommu[sid]->instance = o;
        o->iommu[sid]->sid = sid;
        memory_region_init_iommu(o->iommu[sid], sizeof(*o->iommu[sid]),
                                 TYPE_APPLE_DART_IOMMU, OBJECT(s), name,
                                 1ULL << DART_MAX_VA_BITS);
    }
}

static void apple_dart_reset(DeviceState *dev)
{
    AppleDARTState *s = APPLE_DART(dev);

    for (uint32_t i = 0; i < s->num_instances; i++) {
        AppleDARTInstance *o = &s->instance[i];

        qemu_mutex_lock(&o->lock);
        memset(o->regs, 0, sizeof(o->regs));
        o->regs[DART_PARAMS1 >> 2] = s->page_shift << DART_PARAMS1_PAGE_SHIFT;
        o->regs[DART_PARAMS2 >> 2] = DART_PARAMS2_BYPASS;
        for (uint32_t sid = 0; sid < DART_MAX_STREAMS; sid++) {
            o->regs[DART_SID_REMAP(sid) >> 2] = sid;
        }
        apple_dart_flush_instance(o);
        qemu_mutex_unlock(&o->lock);
    }
    apple_dart_update_irq(s);
}

static void apple_dart_unrealize(DeviceState *dev)
{
    AppleDARTState *s = APPLE_DART(dev);

    for (uint32_t i = 0; i < s->num_instances; i++) {
        AppleDARTInstance *o = &s->instance[i];
        if (o->tlb) {
            g_hash_table_destroy(o->tlb);
            o->tlb = NULL;
        }
        qemu_mutex_destroy(&o->lock);
    }
    g_clear_pointer(&s->name, g_free);
}

static void apple_dart_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, apple_dart_reset);
    dc->unrealize = apple_dart_unrealize;
    dc->desc = "Apple DART IOMMU";
}

static void apple_dart_iommu_class_init(ObjectClass *klass, const void *data)
{
    IOMMUMemoryRegionClass *imrc = IOMMU_MEMORY_REGION_CLASS(klass);

    imrc->translate = apple_dart_translate;
    imrc->attrs_to_index = apple_dart_attrs_to_index;
}

static const TypeInfo apple_dart_info = {
    .name = TYPE_APPLE_DART,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AppleDARTState),
    .class_init = apple_dart_class_init,
};

static const TypeInfo apple_dart_iommu_info = {
    .name = TYPE_APPLE_DART_IOMMU,
    .parent = TYPE_IOMMU_MEMORY_REGION,
    .instance_size = sizeof(AppleDARTIOMMUMemoryRegion),
    .class_init = apple_dart_iommu_class_init,
};

static void apple_dart_register_types(void)
{
    type_register_static(&apple_dart_info);
    type_register_static(&apple_dart_iommu_info);
}

type_init(apple_dart_register_types)

AppleDARTState *apple_dart_create(const char *name,
                                  uint32_t page_size,
                                  uint32_t sids,
                                  uint32_t bypass,
                                  uint64_t bypass_address,
                                  uint32_t num_instances,
                                  const hwaddr *bases,
                                  const uint64_t *sizes,
                                  qemu_irq fault_irq)
{
    DeviceState *dev;
    AppleDARTState *s;

    if (!num_instances || num_instances > DART_MAX_INSTANCES ||
        (page_size != 4096 && page_size != 16384)) {
        return NULL;
    }

    dev = qdev_new(TYPE_APPLE_DART);
    s = APPLE_DART(dev);
    s->name = g_strdup(name ? name : "dart");
    s->page_size = page_size;
    s->page_shift = ctz32(page_size);
    s->page_offset_mask = page_size - 1;
    s->page_mask = ~s->page_offset_mask;
    s->sids = sids ? sids : 0xffff;
    s->bypass = bypass;
    s->bypass_address = bypass_address;
    s->num_instances = num_instances;

    if (s->page_shift == 12) {
        s->level_mask[0] = 0x000c0000;
        s->level_mask[1] = 0x0003fe00;
        s->level_mask[2] = 0x000001ff;
        s->level_shift[0] = 18;
        s->level_shift[1] = 9;
        s->level_shift[2] = 0;
    } else {
        s->level_mask[0] = 0x00c00000;
        s->level_mask[1] = 0x003ff800;
        s->level_mask[2] = 0x000007ff;
        s->level_shift[0] = 22;
        s->level_shift[1] = 11;
        s->level_shift[2] = 0;
    }

    for (uint32_t i = 0; i < num_instances; i++) {
        apple_dart_init_instance(s, &s->instance[i], i, sizes[i]);
    }
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->fault_irq);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s), &error_fatal);

    for (uint32_t i = 0; i < num_instances; i++) {
        sysbus_mmio_map(SYS_BUS_DEVICE(s), i, bases[i]);
    }
    if (fault_irq) {
        sysbus_connect_irq(SYS_BUS_DEVICE(s), 0, fault_irq);
    }
    device_cold_reset(dev);
    return s;
}

IOMMUMemoryRegion *apple_dart_get_iommu(AppleDARTState *s,
                                        uint32_t instance,
                                        uint32_t sid)
{
    if (!s || instance >= s->num_instances || sid >= DART_MAX_STREAMS) {
        return NULL;
    }
    return IOMMU_MEMORY_REGION(s->instance[instance].iommu[sid]);
}
