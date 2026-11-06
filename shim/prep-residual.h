/* SPDX-License-Identifier: GPL-2.0-only */
/* prep-residual.h -- the PReP residual data, as much of it as the shim fills.
 *
 * PReP firmware hands an operating system's boot loader a block of "residual
 * data" describing the machine.  It is defined by the PowerPC Reference
 * Platform specification (IBM, Motorola, 1995) and read, field by field and
 * big-endian, by IBM's boot loader.  This declares the layout the loader
 * reads -- the vital product data, the CPU table, the memory map and the
 * SIMM table -- and leaves the device list and its PnP heap opaque: the shim
 * reports no devices.  The offsets and sizes below are those of the
 * specification's version 0 layout (the loader rejects any other size), and
 * each is checked at compile time.
 */
#ifndef PREP_RESIDUAL_H
#define PREP_RESIDUAL_H

#include <stddef.h>
#include <stdint.h>

/* The specification's fixed table sizes; they decide the block's size. */
#define MAX_CPUS     32
#define MAX_MEMS     64
#define MAX_MEM_SEGS 64
#define MAX_DEVICES  256
#define DEVICE_SIZE  36     /* a device's ID, bus access and three heap offsets */
#define PNP_HEAP     (2 * MAX_DEVICES * 32)

/* Vital product data: the machine, its firmware and its processor. */
typedef struct {
    uint8_t PrintableModel[32];   /* "vvv model", NUL-terminated (vvv: vendor) */
    uint8_t Serial[16];           /* "vvv<serial>", space-padded */
    uint8_t Reserved[48];
    uint32_t FirmwareSupplier;
    uint32_t FirmwareSupports;
    uint32_t NvramSize;           /* bytes */
    uint32_t NumSIMMSlots;
    uint16_t EndianSwitchMethod;  /* 1: port $92 */
    uint16_t SpreadIOMethod;      /* 0: port $850 */
    uint32_t SmpIar;
    uint32_t RAMErrLogOffset;
    uint32_t Reserved5;
    uint32_t Reserved6;
    uint32_t ProcessorHz;
    uint32_t ProcessorBusHz;
    uint32_t Reserved7;
    uint32_t TimeBaseDivisor;     /* bus clocks per time-base tick, x1000 */
    uint32_t WordWidth;
    uint32_t PageSize;
    uint32_t CoherenceBlockSize;
    uint32_t GranuleSize;
    uint32_t CacheSize;           /* L1, KB in all */
    uint32_t CacheAttrib;         /* 0 none, 1 split, 2 combined */
    uint32_t CacheAssoc;
    uint32_t CacheLineSize;
    uint32_t I_CacheSize;
    uint32_t I_CacheAssoc;
    uint32_t I_CacheLineSize;
    uint32_t D_CacheSize;
    uint32_t D_CacheAssoc;
    uint32_t D_CacheLineSize;
    uint32_t TLBSize;
    uint32_t TLBAttrib;
    uint32_t TLBAssoc;
    uint32_t I_TLBSize;
    uint32_t I_TLBAssoc;
    uint32_t D_TLBSize;
    uint32_t D_TLBAssoc;
    uint32_t ExtendedVPD;
} VPD;

typedef struct {
    uint32_t CpuType;             /* the processor's PVR */
    uint8_t CpuNumber;
    uint8_t CpuState;             /* 0: present and running */
    uint16_t Reserved;
} PPC_CPU;

/* A memory-map entry: what a range of 4 KB pages is used for. */
typedef struct {
    uint32_t Usage;
    uint32_t BasePage;
    uint32_t PageCount;
} MEM_MAP;

enum {                            /* MEM_MAP.Usage */
    Free = 0x10,
    BootImage = 0x08,
    PCIConfig = 0x80,
    PCIAddr = 0x100,
    SystemIO = 0x400,
    SystemROM = 0x2000,
};

typedef struct {
    uint32_t SIMMSize;            /* MB; 0 for an empty slot */
} PPC_MEM;

typedef struct {
    uint32_t ResidualLength;      /* the whole block, in bytes */
    uint8_t Version;
    uint8_t Revision;
    uint16_t EC;
    VPD VitalProductData;
    uint16_t MaxNumCpus;
    uint16_t ActualNumCpus;
    PPC_CPU Cpus[MAX_CPUS];
    uint32_t TotalMemory;         /* bytes */
    uint32_t GoodMemory;
    uint32_t ActualNumMemSegs;
    MEM_MAP Segs[MAX_MEM_SEGS];
    uint32_t ActualNumMemories;
    PPC_MEM Memories[MAX_MEMS];
    uint32_t ActualNumDevices;
    uint8_t Devices[MAX_DEVICES][DEVICE_SIZE];
    uint8_t DevicePnPHeap[PNP_HEAP];
} RESIDUAL;

_Static_assert(sizeof(VPD) == 0xEC, "VPD layout");
_Static_assert(offsetof(VPD, FirmwareSupplier) == 0x60, "VPD layout");
_Static_assert(offsetof(VPD, EndianSwitchMethod) == 0x70, "VPD layout");
_Static_assert(offsetof(VPD, SmpIar) == 0x74, "VPD layout");
_Static_assert(offsetof(VPD, TimeBaseDivisor) == 0x90, "VPD layout");
_Static_assert(offsetof(VPD, ExtendedVPD) == 0xE8, "VPD layout");
_Static_assert(sizeof(PPC_CPU) == 8 && sizeof(MEM_MAP) == 12, "table entries");
_Static_assert(offsetof(RESIDUAL, VitalProductData) == 0x08, "residual layout");
_Static_assert(offsetof(RESIDUAL, MaxNumCpus) == 0xF4, "residual layout");
_Static_assert(offsetof(RESIDUAL, Cpus) == 0xF8, "residual layout");
_Static_assert(offsetof(RESIDUAL, TotalMemory) == 0x1F8, "residual layout");
_Static_assert(offsetof(RESIDUAL, Segs) == 0x204, "residual layout");
_Static_assert(offsetof(RESIDUAL, ActualNumMemories) == 0x504, "residual layout");
_Static_assert(offsetof(RESIDUAL, ActualNumDevices) == 0x608, "residual layout");
_Static_assert(offsetof(RESIDUAL, DevicePnPHeap) == 0x2A0C, "residual layout");
_Static_assert(sizeof(RESIDUAL) == 27148, "the residual data the loader reads");

#endif
