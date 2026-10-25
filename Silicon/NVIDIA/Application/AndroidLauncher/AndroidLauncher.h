/** @file

  Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
  Copyright (c) 2022 The LineageOS Project

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef __ANDROID_LAUNCHER_H_
#define __ANDROID_LAUNCHER_H_

#define MAX_BOOTCONFIG_CONTENT_SIZE     512
#define MAX_CBOOTARG_SIZE               256
#define DETACHED_SIG_FILE_EXTENSION     L".sig"

#define BOOTMODE_BOOTIMG_STRING   L"bootmode=bootimg"
#define BOOTMODE_RECOVERY_STRING  L"bootmode=recovery"

#define BOOTCHAIN_OVERRIDE_STRING  L"bootchain="

#define MAX_PARTITION_NAME_SIZE  36       // From the UEFI spec for GPT partitions

#define BOOT_FW_VARIABLE_NAME  L"BootChainFwCurrent"
#define BOOT_OS_VARIABLE_NAME  L"BootChainOsCurrent"

#define ROOTFS_BASE_NAME        L"system"
#define BOOTIMG_BASE_NAME       L"boot"
#define BOOTIMG_DTB_BASE_NAME   L"kernel-dtb"
#define RECOVERY_BASE_NAME      L"recovery"
#define RECOVERY_DTB_BASE_NAME  L"recovery-dtb"

#define SCRATCH0_RECOVERY_BIT_FIELD 31

#define VENDOR_BOOT_BASE_NAME   L"vendor_boot"

//
// Boot image header v3/v4 and vendor boot image header v3/v4, from AOSP
// system/tools/mkbootimg/include/bootimg/bootimg.h. The v3+ boot image keeps the kernel
// and the generic ramdisk; vendor_boot holds the vendor ramdisk, cmdline and bootconfig.
//
#define ANDROID_BOOTIMG_V3_PAGE_SIZE      4096
#define ANDROID_BOOTIMG_V3_CMDLINE_SIZE   1536
#define VENDOR_BOOT_MAGIC                 "VNDRBOOT"
#define VENDOR_BOOT_MAGIC_LENGTH          8
#define VENDOR_BOOT_CMDLINE_SIZE          2048
#define VENDOR_BOOT_NAME_SIZE             16

#define BOOTCONFIG_MAGIC          "#BOOTCONFIG\n"
#define BOOTCONFIG_MAGIC_LENGTH   12
#define BOOTCONFIG_TRAILER_SIZE   (sizeof (UINT32) * 2 + BOOTCONFIG_MAGIC_LENGTH)

#define MAX_KERNEL_CMDLINE_SIZE   8192

#pragma pack(1)
typedef struct {
  UINT8     BootMagic[8];
  UINT32    KernelSize;
  UINT32    RamdiskSize;
  UINT32    OsVersion;
  UINT32    HeaderSize;
  UINT32    Reserved[4];
  UINT32    HeaderVersion;
  CHAR8     Cmdline[ANDROID_BOOTIMG_V3_CMDLINE_SIZE];
  UINT32    SignatureSize;              // v4 only
} ANDROID_BOOTIMG_V3_HEADER;

typedef struct {
  UINT8     Magic[VENDOR_BOOT_MAGIC_LENGTH];
  UINT32    HeaderVersion;
  UINT32    PageSize;
  UINT32    KernelAddress;
  UINT32    RamdiskAddress;
  UINT32    VendorRamdiskSize;
  CHAR8     Cmdline[VENDOR_BOOT_CMDLINE_SIZE];
  UINT32    TagsAddress;
  CHAR8     Name[VENDOR_BOOT_NAME_SIZE];
  UINT32    HeaderSize;
  UINT32    DtbSize;
  UINT64    DtbAddress;
  UINT32    VendorRamdiskTableSize;     // v4 only from here
  UINT32    VendorRamdiskTableEntryNum;
  UINT32    VendorRamdiskTableEntrySize;
  UINT32    BootconfigSize;
} VENDOR_BOOTIMG_HEADER;
#pragma pack()

//
// cboot slot metadata (hardware/nvidia/boot_control bootctrl_nvidia.h). Its location is in
// the T194 BR BCT; the r35 MB1 does not read it, the launcher applies it to the boot chain.
//
#define BOOT_FW_NEXT_VARIABLE_NAME    L"BootChainFwNext"
#define BOOT_FW_STATUS_VARIABLE_NAME  L"BootChainFwStatus"
#define SMD_SWITCH_VARIABLE_NAME      L"AndroidLauncherSmdSwitch"
#define SMD_INFO_OFFSET_T194          0x7DC
#define SMD_STORAGE_QSPI              3
#define SMD_MAGIC                     SIGNATURE_32 (0, 'N', 'B', 'C')
#define SMD_NUM_SLOTS                 2
#define SMD_MAX_RETRY                 7

#pragma pack(1)
typedef struct {
  UINT16    DeviceType;
  UINT16    DeviceInstance;
  UINT32    StartSector;
  UINT32    PartitionSize;
} SMD_INFO;

typedef struct {
  UINT8    Priority;          // 0: unbootable, 15: highest
  CHAR8    Suffix[2];
  UINT8    RetryCount;
  UINT8    BootSuccessful;
} SMD_SLOT_INFO;

typedef struct {
  UINT32           Magic;
  UINT16           Version;
  UINT16           NumSlots;
  SMD_SLOT_INFO    Slot[SMD_NUM_SLOTS];
  UINT32           Crc32;
} SMD_PARTITION;
#pragma pack()

#endif /* __ANDROID_LAUNCHER_H_ */
