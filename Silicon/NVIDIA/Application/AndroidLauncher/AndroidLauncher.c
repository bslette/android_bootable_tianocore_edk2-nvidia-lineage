/** @file
  The main process for AndroidLauncher application.

  Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
  Copyright (c) 2022 The LineageOS Project

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Library/BaseMemoryLib.h>
#include <Library/UefiLib.h>
#include <Library/ShellLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/HandleParsingLib.h>
#include <Library/PrintLib.h>
#include <Library/FileHandleLib.h>
#include <Library/DevicePathLib.h>
#include <Library/AndroidBootImgLib.h>
#include <Library/IoLib.h>
#include <Library/TegraPlatformInfoLib.h>

#include <Protocol/DevicePath.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/AndroidBootImg.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DiskIo.h>
#include <Protocol/LoadFile2.h>

#include <Guid/LinuxEfiInitrdMedia.h>

#include <Guid/LinuxEfiInitrdMedia.h>
#include <Guid/AuthenticatedVariableFormat.h>
#include <Guid/ImageAuthentication.h>

#include <UefiSecureBoot.h>
#include <Library/SecureBootVariableLib.h>

#include <NVIDIAConfiguration.h>
#include <libfdt.h>
#include <Library/PlatformResourceLib.h>
#include "AndroidLauncher.h"
#include "L4TRootfsValidation.h"

/**
  Find the index of the GPT on disk.

  @param[in]  DeviceHandle     The handle of partition.

  @retval Index of the partition.

**/
STATIC
UINT32
EFIAPI
LocatePartitionIndex (
  IN EFI_HANDLE  DeviceHandle
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  HARDDRIVE_DEVICE_PATH     *HardDrivePath;

  if (DeviceHandle == 0) {
    return 0;
  }

  DevicePath = DevicePathFromHandle (DeviceHandle);
  if (DevicePath == NULL) {
    ErrorPrint (L"%a: Unable to find device path\r\n", __FUNCTION__);
    return 0;
  }

  while (!IsDevicePathEndType (DevicePath)) {
    if ((DevicePathType (DevicePath) == MEDIA_DEVICE_PATH) &&
        (DevicePathSubType (DevicePath) == MEDIA_HARDDRIVE_DP))
    {
      HardDrivePath = (HARDDRIVE_DEVICE_PATH *)DevicePath;
      return HardDrivePath->PartitionNumber;
    }

    DevicePath = NextDevicePathNode (DevicePath);
  }

  ErrorPrint (L"%a: Unable to locate harddrive device path node\r\n", __FUNCTION__);
  return 0;
}

/**
  Find the partition on the same disk as the loaded image

  Will fall back to the other bootchain if needed

  @param[in]  DeviceHandle     The handle of partition where this file lives on.
  @param[out] PartitionIndex   The partition index on the disk
  @param[out] PartitionHandle  The partition handle

  @retval EFI_SUCCESS    The operation completed successfully.
  @retval EFI_NOT_FOUND  The partition is not on the filesystem.

**/
STATIC
EFI_STATUS
EFIAPI
FindPartitionInfo (
  IN EFI_HANDLE    DeviceHandle,
  IN CONST CHAR16  *PartitionBasename,
  IN UINT32        BootChain,
  OUT UINT32       *PartitionIndex OPTIONAL,
  OUT EFI_HANDLE   *PartitionHandle OPTIONAL
  )
{
  EFI_STATUS                   Status;
  EFI_HANDLE                   *ParentHandles;
  UINTN                        ParentCount;
  UINTN                        ParentIndex;
  EFI_HANDLE                   *ChildHandles;
  UINTN                        ChildCount;
  UINTN                        ChildIndex;
  UINT32                       FoundIndex = 0;
  EFI_PARTITION_INFO_PROTOCOL  *PartitionInfo;
  EFI_HANDLE                   FoundHandle        = 0;
  EFI_HANDLE                   FoundHandleGeneric = 0;
  EFI_HANDLE                   FoundHandleAlt     = 0;
  CHAR16                       *SubString;
  UINTN                        PartitionBasenameLen;

  if (BootChain > 1) {
    return EFI_UNSUPPORTED;
  }

  if (PartitionBasename == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  PartitionBasenameLen = StrnLenS (PartitionBasename, MAX_PARTITION_NAME_SIZE);

  Status = PARSE_HANDLE_DATABASE_PARENTS (DeviceHandle, &ParentCount, &ParentHandles);
  if (EFI_ERROR (Status)) {
    ErrorPrint (L"%a: Failed to find parents - %r\r\n", __FUNCTION__, Status);
    return Status;
  }

  for (ParentIndex = 0; ParentIndex < ParentCount; ParentIndex++) {
    Status = ParseHandleDatabaseForChildControllers (ParentHandles[ParentIndex], &ChildCount, &ChildHandles);
    if (EFI_ERROR (Status)) {
      ErrorPrint (L"%a: Failed to find child controllers - %r\r\n", __FUNCTION__, Status);
      return Status;
    }

    for (ChildIndex = 0; ChildIndex < ChildCount; ChildIndex++) {
      Status = gBS->HandleProtocol (ChildHandles[ChildIndex], &gEfiPartitionInfoProtocolGuid, (VOID **)&PartitionInfo);
      if (EFI_ERROR (Status)) {
        continue;
      }

      // Only GPT partitions are supported
      if (PartitionInfo->Type != PARTITION_TYPE_GPT) {
        continue;
      }

      // Look for A/B Names
      if (StrCmp (PartitionInfo->Info.Gpt.PartitionName, PartitionBasename) == 0) {
        ASSERT (FoundHandleGeneric == 0);
        FoundHandleGeneric = ChildHandles[ChildIndex];
      } else if ((PartitionBasenameLen + 2) == StrLen (PartitionInfo->Info.Gpt.PartitionName)) {
        SubString = StrStr (PartitionInfo->Info.Gpt.PartitionName, PartitionBasename);
        if (SubString != NULL) {
          // See if it is a prefix
          if ((SubString == (PartitionInfo->Info.Gpt.PartitionName + 2)) &&
              (PartitionInfo->Info.Gpt.PartitionName[1] == L'_'))
          {
            if ((PartitionInfo->Info.Gpt.PartitionName[0] == (L'A' + BootChain)) ||
                (PartitionInfo->Info.Gpt.PartitionName[0] == (L'a' + BootChain)))
            {
              ASSERT (FoundHandle == 0);
              FoundHandle = ChildHandles[ChildIndex];
            }

            if ((PartitionInfo->Info.Gpt.PartitionName[0] == (L'B' - BootChain)) ||
                (PartitionInfo->Info.Gpt.PartitionName[0] == (L'b' - BootChain)))
            {
              ASSERT (FoundHandleAlt == 0);
              FoundHandleAlt = ChildHandles[ChildIndex];
            }

            // See if it is a postfix
          } else if ((SubString == PartitionInfo->Info.Gpt.PartitionName) &&
                     (PartitionInfo->Info.Gpt.PartitionName[PartitionBasenameLen] == L'_'))
          {
            if ((PartitionInfo->Info.Gpt.PartitionName[PartitionBasenameLen + 1] == (L'a' + BootChain)) ||
                (PartitionInfo->Info.Gpt.PartitionName[PartitionBasenameLen + 1] == (L'A' + BootChain)))
            {
              ASSERT (FoundHandle == 0);
              FoundHandle = ChildHandles[ChildIndex];
            } else if ((PartitionInfo->Info.Gpt.PartitionName[PartitionBasenameLen + 1] == (L'b' - BootChain)) ||
                       (PartitionInfo->Info.Gpt.PartitionName[PartitionBasenameLen + 1] == (L'B' - BootChain)))
            {
              ASSERT (FoundHandleAlt == 0);
              FoundHandleAlt = ChildHandles[ChildIndex];
            }
          }
        }
      }
    }

    FreePool (ChildHandles);
  }

  FreePool (ParentHandles);

  if ((FoundHandle == 0) && (FoundHandleGeneric == 0) && (FoundHandleAlt == 0)) {
    return EFI_NOT_FOUND;
  } else if (FoundHandle == 0) {
    if (FoundHandleGeneric != 0) {
      FoundHandle = FoundHandleGeneric;
    } else {
      FoundHandle = FoundHandleAlt;
      Print (L"Falling back to alternative boot path\r\n");
    }
  }

  FoundIndex = LocatePartitionIndex (FoundHandle);

  if (FoundIndex == 0) {
    ErrorPrint (L"%a: Failed to find both partitions index\r\n", __FUNCTION__);
    return EFI_DEVICE_ERROR;
  }

  if (PartitionIndex != NULL) {
    *PartitionIndex = FoundIndex;
  }

  if (PartitionHandle != NULL) {
    *PartitionHandle = FoundHandle;
  }

  return EFI_SUCCESS;
}

/**
  Get BootRecovery flag

**/
STATIC
EFI_STATUS
EFIAPI
GetBootRecovery (
  IN  UINT32  *BootRecovery
  )
{
  UINTN Address = 0;

  switch (TegraGetChipID ()) {
    case T234_CHIP_ID:
      Address = FixedPcdGet64 (PcdScratch0RegisterBaseAddressT234);
      break;
    case T194_CHIP_ID:
      Address = FixedPcdGet64 (PcdScratch0RegisterBaseAddressT194);
      break;
    default:
      return EFI_UNSUPPORTED;
  }

  *BootRecovery = MmioBitFieldRead32 (
                    Address,
                    SCRATCH0_RECOVERY_BIT_FIELD,
                    SCRATCH0_RECOVERY_BIT_FIELD
                    );

  return EFI_SUCCESS;
}

/**
  Process the boot mode selection from command line and variables

  @param[in]  LoadedImage     The LoadedImage protocol for this execution
  @param[out] BootParams      The current boot parameters

  @retval EFI_SUCCESS    The operation completed successfully.

**/
STATIC
EFI_STATUS
EFIAPI
ProcessBootParams (
  IN  EFI_LOADED_IMAGE_PROTOCOL  *LoadedImage,
  OUT L4T_BOOT_PARAMS            *BootParams
  )
{
  CONST CHAR16  *CurrentBootOption;
  EFI_STATUS    Status;
  UINT32        BootChain;
  UINTN         DataSize;
  UINT64        StringValue;
  UINT32        BootRecovery;

  if ((LoadedImage == NULL) || (BootParams == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  BootParams->BootChain = 0;
  BootParams->BootMode = NVIDIA_L4T_BOOTMODE_BOOTIMG;

  Status = GetBootRecovery (&BootRecovery);
  if ((!EFI_ERROR (Status) && BootRecovery > 0) ||
      FixedPcdGetBool (PcdBootToRecovery)) {
    BootParams->BootMode = NVIDIA_L4T_BOOTMODE_RECOVERY;
  }

  DataSize = sizeof (BootChain);
  Status   = gRT->GetVariable (BOOT_FW_VARIABLE_NAME, &gNVIDIAPublicVariableGuid, NULL, &DataSize, &BootChain);
  // If variable does not exist, is >4 bytes or has a value larger than 1, boot partition A
  if (!EFI_ERROR (Status) && (BootChain <= 1)) {
    BootParams->BootChain = BootChain;
  }

  // Read current OS boot type to allow for chaining
  DataSize = sizeof (BootChain);
  Status   = gRT->GetVariable (BOOT_OS_VARIABLE_NAME, &gNVIDIAPublicVariableGuid, NULL, &DataSize, &BootChain);
  // If variable does not exist, is >4 bytes or has a value larger than 1, boot partition A
  if (!EFI_ERROR (Status) && (BootChain <= 1)) {
    BootParams->BootChain = BootChain;
  }

  if (LoadedImage->LoadOptionsSize) {
    CurrentBootOption = StrStr (LoadedImage->LoadOptions, BOOTMODE_BOOTIMG_STRING);
    if (CurrentBootOption != NULL) {
      BootParams->BootMode = NVIDIA_L4T_BOOTMODE_BOOTIMG;
    }

    CurrentBootOption = StrStr (LoadedImage->LoadOptions, BOOTMODE_RECOVERY_STRING);
    if (CurrentBootOption != NULL) {
      BootParams->BootMode = NVIDIA_L4T_BOOTMODE_RECOVERY;
    }

    // See if boot option is passed in
    CurrentBootOption = StrStr (LoadedImage->LoadOptions, BOOTCHAIN_OVERRIDE_STRING);
    if (CurrentBootOption != NULL) {
      CurrentBootOption += StrLen (BOOTCHAIN_OVERRIDE_STRING);
      Status             = StrDecimalToUint64S (CurrentBootOption, NULL, &StringValue);
      if (EFI_ERROR (Status)) {
        ErrorPrint (L"Failed to read boot chain override: %r\r\n", Status);
      } else if (StringValue <= 1) {
        BootParams->BootChain = (UINT32)StringValue;
      } else {
        ErrorPrint (L"Boot chain override value out of range, ignoring\r\n");
      }
    }
  }

  // Find valid Rootfs Chain. If not, select recovery kernel.
  // Without an Android boot HAL that resets RootfsRetrySlot<A|B>, this flips the boot chain
  // after three boots; targets whose HAL keeps slot state in SMD (MB1 picks the chain) leave it off.
  if (FixedPcdGetBool (PcdRootfsRetryValidation)) {
    Status = ValidateRootfsStatus (BootParams);
    if (EFI_ERROR (Status)) {
      ErrorPrint (L"Failed to validate rootfs status: %r\r\n", Status);
    }
  }

  // Store the current boot chain in volatile variable to allow chain loading
  Status = gRT->SetVariable (BOOT_OS_VARIABLE_NAME, &gNVIDIAPublicVariableGuid, EFI_VARIABLE_BOOTSERVICE_ACCESS|EFI_VARIABLE_RUNTIME_ACCESS, sizeof (BootParams->BootChain), &BootParams->BootChain);
  if (EFI_ERROR (Status)) {
    ErrorPrint (L"Failed to set OS variable: %r\r\n", Status);
  }

  return EFI_SUCCESS;
}

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH          VenMediaNode;
  EFI_DEVICE_PATH_PROTOCOL    EndNode;
} INITRD_DEVICE_PATH;

typedef struct {
  MEMMAP_DEVICE_PATH          Node1;
  EFI_DEVICE_PATH_PROTOCOL    End;
} KERNEL_DEVICE_PATH;
#pragma pack()

STATIC CONST INITRD_DEVICE_PATH  mInitrdDevicePath = {
  {
    { MEDIA_DEVICE_PATH,    MEDIA_VENDOR_DP,                { sizeof (VENDOR_DEVICE_PATH), 0 }
    },
    LINUX_EFI_INITRD_MEDIA_GUID
  },
  {
    END_DEVICE_PATH_TYPE,   END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 }
  }
};

STATIC CONST KERNEL_DEVICE_PATH  mKernelDevicePathTemplate = {
  {
    { HARDWARE_DEVICE_PATH, HW_MEMMAP_DP,                   { sizeof (MEMMAP_DEVICE_PATH), 0 }
    },
    EfiBootServicesData,
    0,
    0
  },
  {
    END_DEVICE_PATH_TYPE,   END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 }
  }
};

STATIC VOID   *mInitrd;
STATIC UINTN  mInitrdSize;

/**
  LoadFile2 for the Linux initrd media device path: hands the combined ramdisk to the
  kernel EFI stub.

**/
STATIC
EFI_STATUS
EFIAPI
InitrdLoadFile2 (
  IN EFI_LOAD_FILE2_PROTOCOL   *This,
  IN EFI_DEVICE_PATH_PROTOCOL  *FilePath,
  IN BOOLEAN                   BootPolicy,
  IN OUT UINTN                 *BufferSize,
  IN VOID                      *Buffer OPTIONAL
  )
{
  if (BootPolicy) {
    return EFI_UNSUPPORTED;
  }

  if ((BufferSize == NULL) || !IsDevicePathValid (FilePath, 0)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((FilePath->Type != END_DEVICE_PATH_TYPE) ||
      (FilePath->SubType != END_ENTIRE_DEVICE_PATH_SUBTYPE))
  {
    return EFI_NOT_FOUND;
  }

  if ((Buffer == NULL) || (*BufferSize < mInitrdSize)) {
    *BufferSize = mInitrdSize;
    return EFI_BUFFER_TOO_SMALL;
  }

  CopyMem (Buffer, mInitrd, mInitrdSize);
  *BufferSize = mInitrdSize;
  return EFI_SUCCESS;
}

STATIC EFI_LOAD_FILE2_PROTOCOL  mInitrdLoadFile2 = {
  InitrdLoadFile2
};

/**
  Get the size of a boot image of any header version

  @param[in]  Header     Start of the boot image
  @param[out] ImageSize  Bytes to read from the partition

  @retval EFI_SUCCESS    The operation completed successfully.

**/
STATIC
EFI_STATUS
EFIAPI
GetBootImgSize (
  IN  ANDROID_BOOTIMG_HEADER  *Header,
  OUT UINTN                   *ImageSize
  )
{
  ANDROID_BOOTIMG_V3_HEADER  *V3Header;

  // HeaderVersion is at the same offset in every header version (Reserved[0] in v0).
  V3Header = (ANDROID_BOOTIMG_V3_HEADER *)Header;
  if ((CompareMem (V3Header->BootMagic, ANDROID_BOOT_MAGIC, ANDROID_BOOT_MAGIC_LENGTH) == 0) &&
      (V3Header->HeaderVersion >= 3))
  {
    *ImageSize = ANDROID_BOOTIMG_V3_PAGE_SIZE +
                 ALIGN_VALUE (V3Header->KernelSize, ANDROID_BOOTIMG_V3_PAGE_SIZE) +
                 ALIGN_VALUE (V3Header->RamdiskSize, ANDROID_BOOTIMG_V3_PAGE_SIZE);
    return EFI_SUCCESS;
  }

  return AndroidBootImgGetImgSize (Header, ImageSize);
}

/**
  Read the vendor_boot image of the boot chain

  @param[in]  DeviceHandle  The handle of partition where this file lives on.
  @param[in]  BootChain     Numeric version of the chain
  @param[out] VendorBoot    Allocated buffer holding the image, header first

  @retval EFI_SUCCESS    The operation completed successfully.

**/
STATIC
EFI_STATUS
EFIAPI
ReadVendorBootImage (
  IN  EFI_HANDLE  DeviceHandle,
  IN  UINT32      BootChain,
  OUT VOID        **VendorBoot
  )
{
  EFI_STATUS             Status;
  EFI_HANDLE             PartitionHandle;
  EFI_BLOCK_IO_PROTOCOL  *BlockIo;
  EFI_DISK_IO_PROTOCOL   *DiskIo;
  VENDOR_BOOTIMG_HEADER  Header;
  UINT32                 Offset;
  UINT32                 PageSize;
  UINT64                 Size;
  UINT64                 PartitionSize;
  VOID                   *Buffer;

  Status = FindPartitionInfo (DeviceHandle, VENDOR_BOOT_BASE_NAME, BootChain, NULL, &PartitionHandle);
  if (EFI_ERROR (Status)) {
    ErrorPrint (L"%a: Unable to locate %s partition\r\n", __FUNCTION__, VENDOR_BOOT_BASE_NAME);
    return Status;
  }

  Status = gBS->HandleProtocol (PartitionHandle, &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->HandleProtocol (PartitionHandle, &gEfiDiskIoProtocolGuid, (VOID **)&DiskIo);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Offset = 0;
  Status = DiskIo->ReadDisk (DiskIo, BlockIo->Media->MediaId, Offset, sizeof (Header), &Header);
  if (!EFI_ERROR (Status) && (CompareMem (Header.Magic, VENDOR_BOOT_MAGIC, VENDOR_BOOT_MAGIC_LENGTH) != 0)) {
    Offset = PcdGet32 (PcdSignedImageHeaderSize);
    Status = DiskIo->ReadDisk (DiskIo, BlockIo->Media->MediaId, Offset, sizeof (Header), &Header);
  }

  if (EFI_ERROR (Status)) {
    ErrorPrint (L"Failed to read disk\r\n");
    return Status;
  }

  if (CompareMem (Header.Magic, VENDOR_BOOT_MAGIC, VENDOR_BOOT_MAGIC_LENGTH) != 0) {
    ErrorPrint (L"%a: No vendor boot image header\r\n", __FUNCTION__);
    return EFI_NOT_FOUND;
  }

  PageSize = Header.PageSize;
  if ((Header.HeaderVersion < 3) || (PageSize == 0) || ((PageSize & (PageSize - 1)) != 0)) {
    ErrorPrint (L"%a: Unsupported vendor boot header v%u, page size %u\r\n", __FUNCTION__, Header.HeaderVersion, PageSize);
    return EFI_UNSUPPORTED;
  }

  if (Header.HeaderVersion < 4) {
    Header.VendorRamdiskTableSize = 0;
    Header.BootconfigSize         = 0;
  }

  Size = (UINT64)ALIGN_VALUE (Header.HeaderSize, PageSize) +
         ALIGN_VALUE (Header.VendorRamdiskSize, PageSize) +
         ALIGN_VALUE (Header.DtbSize, PageSize) +
         ALIGN_VALUE (Header.VendorRamdiskTableSize, PageSize) +
         ALIGN_VALUE (Header.BootconfigSize, PageSize);
  PartitionSize = MultU64x32 (BlockIo->Media->LastBlock + 1, BlockIo->Media->BlockSize);
  if (Offset + Size > PartitionSize) {
    ErrorPrint (L"%a: Vendor boot image (%lu bytes) larger than its partition\r\n", __FUNCTION__, Size);
    return EFI_VOLUME_CORRUPTED;
  }

  Buffer = AllocatePool (Size);
  if (Buffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = DiskIo->ReadDisk (DiskIo, BlockIo->Media->MediaId, Offset, Size, Buffer);
  if (EFI_ERROR (Status)) {
    ErrorPrint (L"Failed to read disk\r\n");
    FreePool (Buffer);
    return Status;
  }

  // Keep the v3 adjustments in the returned copy.
  CopyMem (Buffer, &Header, sizeof (Header));
  *VendorBoot = Buffer;
  return EFI_SUCCESS;
}

/**
  Append space separated arguments to an ASCII kernel command line

  @param[in, out] Cmdline  Command line of MAX_KERNEL_CMDLINE_SIZE bytes
  @param[in]      Args     Arguments, not necessarily NUL terminated
  @param[in]      ArgsMax  Maximum length of Args

**/
STATIC
VOID
EFIAPI
AppendCmdline (
  IN OUT CHAR8        *Cmdline,
  IN     CONST CHAR8  *Args,
  IN     UINTN        ArgsMax
  )
{
  UINTN  Used;
  UINTN  Length;

  Length = AsciiStrnLenS (Args, ArgsMax);
  if (Length == 0) {
    return;
  }

  Used = AsciiStrLen (Cmdline);
  if (Used + 1 + Length + 1 > MAX_KERNEL_CMDLINE_SIZE) {
    ErrorPrint (L"%a: Kernel command line too long, dropping %u bytes\r\n", __FUNCTION__, Length);
    return;
  }

  if (Used != 0) {
    Cmdline[Used++] = ' ';
  }

  CopyMem (Cmdline + Used, Args, Length);
  Cmdline[Used + Length] = '\0';
}

/**
  Boot a v3/v4 boot image together with its vendor_boot image

  The initrd is the vendor ramdisk, then the generic ramdisk, then the vendor bootconfig
  with its trailer; it reaches the kernel EFI stub through LoadFile2. The command line is
  the DT bootargs, the vendor and boot image command lines, the Android boot properties
  and the platform additions.

  @param[in]  DeviceHandle  The handle of partition where this file lives on.
  @param[in]  Image         The boot image, header first
  @param[in]  BootParams    The current boot parameters

  @retval EFI_SUCCESS    The operation completed successfully.

**/
STATIC
EFI_STATUS
EFIAPI
BootAndroidV3Image (
  IN EFI_HANDLE       DeviceHandle,
  IN VOID             *Image,
  IN L4T_BOOT_PARAMS  *BootParams
  )
{
  EFI_STATUS                 Status;
  ANDROID_BOOTIMG_V3_HEADER  *Header;
  VENDOR_BOOTIMG_HEADER      *VendorHeader;
  VOID                       *VendorBoot;
  UINT8                      *VendorSection;
  UINT32                     PageSize;
  UINT8                      *Kernel;
  UINT8                      *Ramdisk;
  UINT8                      *Bootconfig;
  UINT8                      *Trailer;
  UINT32                     Checksum;
  UINTN                      Index;
  CHAR8                      *Cmdline;
  CHAR16                     *Cmdline16;
  CHAR8                      Args[64];
  VOID                       *Fdt;
  INTN                       NodeOffset;
  CONST CHAR8                *Bootargs;
  INT32                      BootargsLength;
  ANDROID_BOOTIMG_PROTOCOL   *AndroidBootImg;
  KERNEL_DEVICE_PATH         KernelDevicePath;
  EFI_HANDLE                 KernelHandle;
  EFI_HANDLE                 InitrdHandle;
  EFI_LOADED_IMAGE_PROTOCOL  *KernelImage;

  Header       = (ANDROID_BOOTIMG_V3_HEADER *)Image;
  VendorBoot   = NULL;
  mInitrd      = NULL;
  Cmdline      = NULL;
  Cmdline16    = NULL;
  KernelHandle = NULL;
  InitrdHandle = NULL;

  Status = ReadVendorBootImage (DeviceHandle, BootParams->BootChain, &VendorBoot);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  VendorHeader  = (VENDOR_BOOTIMG_HEADER *)VendorBoot;
  PageSize      = VendorHeader->PageSize;
  VendorSection = (UINT8 *)VendorBoot + ALIGN_VALUE (VendorHeader->HeaderSize, PageSize);
  Bootconfig    = VendorSection +
                  ALIGN_VALUE (VendorHeader->VendorRamdiskSize, PageSize) +
                  ALIGN_VALUE (VendorHeader->DtbSize, PageSize) +
                  ALIGN_VALUE (VendorHeader->VendorRamdiskTableSize, PageSize);

  Kernel  = (UINT8 *)Image + ANDROID_BOOTIMG_V3_PAGE_SIZE;
  Ramdisk = Kernel + ALIGN_VALUE (Header->KernelSize, ANDROID_BOOTIMG_V3_PAGE_SIZE);

  // All vendor ramdisk fragments are loaded: there is no board id to select by.
  mInitrdSize = VendorHeader->VendorRamdiskSize + Header->RamdiskSize;
  if (VendorHeader->BootconfigSize != 0) {
    mInitrdSize += VendorHeader->BootconfigSize + BOOTCONFIG_TRAILER_SIZE;
  }

  mInitrd = AllocatePool (mInitrdSize);
  if (mInitrd == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Exit;
  }

  CopyMem (mInitrd, VendorSection, VendorHeader->VendorRamdiskSize);
  CopyMem ((UINT8 *)mInitrd + VendorHeader->VendorRamdiskSize, Ramdisk, Header->RamdiskSize);
  if (VendorHeader->BootconfigSize != 0) {
    Trailer = (UINT8 *)mInitrd + VendorHeader->VendorRamdiskSize + Header->RamdiskSize;
    CopyMem (Trailer, Bootconfig, VendorHeader->BootconfigSize);
    Checksum = 0;
    for (Index = 0; Index < VendorHeader->BootconfigSize; Index++) {
      Checksum += Bootconfig[Index];
    }

    Trailer += VendorHeader->BootconfigSize;
    WriteUnaligned32 ((UINT32 *)Trailer, VendorHeader->BootconfigSize);
    WriteUnaligned32 ((UINT32 *)(Trailer + sizeof (UINT32)), Checksum);
    CopyMem (Trailer + 2 * sizeof (UINT32), BOOTCONFIG_MAGIC, BOOTCONFIG_MAGIC_LENGTH);
  }

  Cmdline   = AllocateZeroPool (MAX_KERNEL_CMDLINE_SIZE);
  Cmdline16 = AllocateZeroPool (MAX_KERNEL_CMDLINE_SIZE * sizeof (CHAR16));
  if ((Cmdline == NULL) || (Cmdline16 == NULL)) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Exit;
  }

  // The EFI stub replaces /chosen/bootargs with the load options, so carry them over.
  Status = EfiGetSystemConfigurationTable (&gFdtTableGuid, &Fdt);
  if (!EFI_ERROR (Status)) {
    NodeOffset = fdt_path_offset (Fdt, "/chosen");
    if (NodeOffset >= 0) {
      Bootargs = fdt_getprop (Fdt, NodeOffset, "bootargs", &BootargsLength);
      if ((Bootargs != NULL) && (BootargsLength > 0)) {
        AppendCmdline (Cmdline, Bootargs, BootargsLength);
      }
    }
  }

  AppendCmdline (Cmdline, VendorHeader->Cmdline, VENDOR_BOOT_CMDLINE_SIZE);
  AppendCmdline (Cmdline, Header->Cmdline, ANDROID_BOOTIMG_V3_CMDLINE_SIZE);
  AsciiSPrint (
    Args,
    sizeof (Args),
    "androidboot.slot_suffix=_%c androidboot.verifiedbootstate=orange",
    'a' + BootParams->BootChain
    );
  AppendCmdline (Cmdline, Args, sizeof (Args));

  // With the QSPI handed to the OS, UEFI runtime variable writes would race the kernel
  // QSPI driver on the same controller: give the OS no runtime services.
  if (PcdGetBool (PcdAllowOsAccessQspi)) {
    AppendCmdline (Cmdline, "efi=noruntime", sizeof ("efi=noruntime"));
  }

  Status = AsciiStrToUnicodeStrS (Cmdline, Cmdline16, MAX_KERNEL_CMDLINE_SIZE);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  Status = gBS->LocateProtocol (&gAndroidBootImgProtocolGuid, NULL, (VOID **)&AndroidBootImg);
  if (!EFI_ERROR (Status) && (AndroidBootImg->AppendArgs != NULL)) {
    Status = AndroidBootImg->AppendArgs (Cmdline16, MAX_KERNEL_CMDLINE_SIZE * sizeof (CHAR16));
    if (EFI_ERROR (Status)) {
      ErrorPrint (L"%a: Failed to append platform kernel args: %r\r\n", __FUNCTION__, Status);
    }
  }

  DEBUG ((DEBUG_ERROR, "%a: Cmdline: %s\n", __FUNCTION__, Cmdline16));

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &InitrdHandle,
                  &gEfiLoadFile2ProtocolGuid,
                  &mInitrdLoadFile2,
                  &gEfiDevicePathProtocolGuid,
                  &mInitrdDevicePath,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    ErrorPrint (L"%a: Failed to install initrd LoadFile2: %r\r\n", __FUNCTION__, Status);
    InitrdHandle = NULL;
    goto Exit;
  }

  KernelDevicePath                       = mKernelDevicePathTemplate;
  KernelDevicePath.Node1.StartingAddress = (EFI_PHYSICAL_ADDRESS)(UINTN)Kernel;
  KernelDevicePath.Node1.EndingAddress   = (EFI_PHYSICAL_ADDRESS)(UINTN)Kernel + Header->KernelSize;

  Status = gBS->LoadImage (
                  TRUE,
                  gImageHandle,
                  (EFI_DEVICE_PATH *)&KernelDevicePath,
                  Kernel,
                  Header->KernelSize,
                  &KernelHandle
                  );
  if (EFI_ERROR (Status)) {
    ErrorPrint (L"%a: Failed to load kernel: %r\r\n", __FUNCTION__, Status);
    goto Exit;
  }

  Status = gBS->HandleProtocol (KernelHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&KernelImage);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  KernelImage->LoadOptions     = Cmdline16;
  KernelImage->LoadOptionsSize = (UINT32)StrSize (Cmdline16);

  // Same watchdog as AndroidBootImgBoot: 5 minutes for the kernel to take over
  gBS->SetWatchdogTimer (5 * 60, 0x10000, 0, NULL);
  Status = gBS->StartImage (KernelHandle, NULL, NULL);
  gBS->SetWatchdogTimer (0, 0x10000, 0, NULL);
  ErrorPrint (L"%a: Kernel returned: %r\r\n", __FUNCTION__, Status);

Exit:
  if (KernelHandle != NULL) {
    gBS->UnloadImage (KernelHandle);
  }

  if (InitrdHandle != NULL) {
    gBS->UninstallMultipleProtocolInterfaces (
           InitrdHandle,
           &gEfiLoadFile2ProtocolGuid,
           &mInitrdLoadFile2,
           &gEfiDevicePathProtocolGuid,
           &mInitrdDevicePath,
           NULL
           );
  }

  if (mInitrd != NULL) {
    FreePool (mInitrd);
    mInitrd = NULL;
  }

  if (Cmdline != NULL) {
    FreePool (Cmdline);
  }

  if (Cmdline16 != NULL) {
    FreePool (Cmdline16);
  }

  if (VendorBoot != NULL) {
    FreePool (VendorBoot);
  }

  return EFI_ERROR (Status) ? Status : EFI_LOAD_ERROR;
}

/**
  Boots an android style partition located with Partition base name and bootchain

  @param[in]  DeviceHandle      The handle of partition where this file lives on.
  @param[in]  PartitionBasename The base name of the partion where the image to boot is located.
  @param[in]  BootChain         Numeric version of the chain


  @retval EFI_SUCCESS    The operation completed successfully.

**/
STATIC
EFI_STATUS
EFIAPI
BootAndroidStylePartition (
  IN EFI_HANDLE       DeviceHandle,
  IN CONST CHAR16     *BootImgPartitionBasename,
  IN CONST CHAR16     *BootImgDtbPartitionBasename,
  IN L4T_BOOT_PARAMS  *BootParams
  )
{
  EFI_STATUS              Status;
  EFI_HANDLE              PartitionHandle;
  EFI_BLOCK_IO_PROTOCOL   *BlockIo = NULL;
  EFI_DISK_IO_PROTOCOL    *DiskIo  = NULL;
  ANDROID_BOOTIMG_HEADER  ImageHeader;
  UINTN                   ImageSize;
  VOID                    *Image;
  UINT32                  Offset = 0;
  UINT64                  Size;
  VOID                    *KernelDtb;
  VOID                    *Dtb;
  VOID                    *ExpandedDtb;
  VOID                    *CurrentDtb = NULL;
  VOID                    *AcpiBase;
  INTN                    NodeOffset;
  CHAR8                   BootSlot[3];
  CHAR16                  *FirmwareVersion;
  CHAR8                   *AsciiFirmwareVersion;

  Status = FindPartitionInfo (DeviceHandle, BootImgPartitionBasename, BootParams->BootChain, NULL, &PartitionHandle);
  if (EFI_ERROR (Status)) {
    ErrorPrint (L"%a: Unable to located partition\r\n", __FUNCTION__);
    return Status;
  }

  Status = gBS->HandleProtocol (PartitionHandle, &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
  if (EFI_ERROR (Status)) {
    ErrorPrint (L"%a: Unable to locate block io protocol on partition\r\n", __FUNCTION__);
    goto Exit;
  }

  Status = gBS->HandleProtocol (PartitionHandle, &gEfiDiskIoProtocolGuid, (VOID **)&DiskIo);
  if (EFI_ERROR (Status)) {
    ErrorPrint (L"%a: Unable to locate disk io protocol on partition\r\n", __FUNCTION__);
    goto Exit;
  }

  Status = DiskIo->ReadDisk (
                     DiskIo,
                     BlockIo->Media->MediaId,
                     Offset,
                     sizeof (ANDROID_BOOTIMG_HEADER),
                     &ImageHeader
                     );
  if (EFI_ERROR (Status)) {
    ErrorPrint (L"Failed to read disk\r\n");
    goto Exit;
  }

  Status = GetBootImgSize (&ImageHeader, &ImageSize);
  if (EFI_ERROR (Status)) {
    Offset = FixedPcdGet32 (PcdSignedImageHeaderSize);
    Status = DiskIo->ReadDisk (
                       DiskIo,
                       BlockIo->Media->MediaId,
                       Offset,
                       sizeof (ANDROID_BOOTIMG_HEADER),
                       &ImageHeader
                       );
    if (EFI_ERROR (Status)) {
      ErrorPrint (L"Failed to read disk\r\n");
      goto Exit;
    }

    Status = GetBootImgSize (&ImageHeader, &ImageSize);
    if (EFI_ERROR (Status)) {
      ErrorPrint (L"Header not seen at either offset 0 or offset 0x%x\r\n", Offset);
      goto Exit;
    }
  }

  Image = AllocatePool (ImageSize);
  if (Image == NULL) {
    ErrorPrint (L"Failed to allocate buffer for Image\r\n");
    Status = EFI_OUT_OF_RESOURCES;
    goto Exit;
  }

  Status = DiskIo->ReadDisk (
                     DiskIo,
                     BlockIo->Media->MediaId,
                     Offset,
                     ImageSize,
                     Image
                     );
  if (EFI_ERROR (Status)) {
    ErrorPrint (L"Failed to read disk\r\n");
    goto Exit;
  }

  Status = EfiGetSystemConfigurationTable (&gEfiAcpiTableGuid, &AcpiBase);
  if (EFI_ERROR (Status)) {
    Status = FindPartitionInfo (DeviceHandle, BootImgDtbPartitionBasename, BootParams->BootChain, NULL, &PartitionHandle);
    if (EFI_ERROR (Status)) {
      ErrorPrint (L"%a: Unable to located partition\r\n", __FUNCTION__);
      return Status;
    }

    Status = gBS->HandleProtocol (PartitionHandle, &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
    if (EFI_ERROR (Status)) {
      ErrorPrint (L"%a: Unable to locate block io protocol on partition\r\n", __FUNCTION__);
      goto Exit;
    }

    Status = gBS->HandleProtocol (PartitionHandle, &gEfiDiskIoProtocolGuid, (VOID **)&DiskIo);
    if (EFI_ERROR (Status)) {
      ErrorPrint (L"%a: Unable to locate disk io protocol on partition\r\n", __FUNCTION__);
      goto Exit;
    }

    Size = MultU64x32 (BlockIo->Media->LastBlock+1, BlockIo->Media->BlockSize);

    KernelDtb = AllocatePool (Size);
    if (KernelDtb == NULL) {
      ErrorPrint (L"Failed to allocate buffer for dtb\r\n");
      Status = EFI_OUT_OF_RESOURCES;
      goto Exit;
    }

    Status = DiskIo->ReadDisk (
                       DiskIo,
                       BlockIo->Media->MediaId,
                       0,
                       Size,
                       KernelDtb
                       );
    if (EFI_ERROR (Status)) {
      ErrorPrint (L"Failed to read disk\r\n");
      goto Exit;
    }

    Dtb = KernelDtb;
    if (fdt_check_header (Dtb) != 0) {
      Dtb += PcdGet32 (PcdSignedImageHeaderSize);
      if (fdt_check_header (Dtb) != 0) {
        ErrorPrint (L"DTB on partition was corrupted, attempt use to UEFI DTB\r\n");
        goto Exit;
      }
    }

    ExpandedDtb = AllocatePages (EFI_SIZE_TO_PAGES (2 * fdt_totalsize (Dtb)));
    if ((ExpandedDtb != NULL) &&
        (fdt_open_into (Dtb, ExpandedDtb, 2 * fdt_totalsize (Dtb)) == 0))
    {
      DEBUG ((DEBUG_ERROR, "%a: Installing Kernel DTB\r\n", __FUNCTION__));
      Status = EfiGetSystemConfigurationTable (&gFdtTableGuid, &CurrentDtb);
      if (EFI_ERROR (Status)) {
        ErrorPrint (L"No existing DTB\r\n");
        goto Exit;
      }

      NodeOffset = fdt_path_offset (ExpandedDtb, "/firmware/android");
      if (NodeOffset >= 0) {
        AsciiSPrint (BootSlot, sizeof (BootSlot), "_%c", 'a' + BootParams->BootChain);
        fdt_setprop (ExpandedDtb, NodeOffset, "slot_suffix", BootSlot, sizeof (BootSlot));
        fdt_setprop (ExpandedDtb, NodeOffset, "verifiedbootstate", "orange", sizeof ("orange"));
        FirmwareVersion = (CHAR16 *)PcdGetPtr (PcdFirmwareVersionString);
        AsciiFirmwareVersion = AllocatePool (StrLen (FirmwareVersion) + 1);
        UnicodeStrToAsciiStrS (FirmwareVersion, AsciiFirmwareVersion, StrLen (FirmwareVersion) + 1);
        fdt_setprop (ExpandedDtb, NodeOffset, "bootloader", AsciiFirmwareVersion, StrLen (FirmwareVersion));
        if (BootParams->BootMode == NVIDIA_L4T_BOOTMODE_BOOTIMG) {
          fdt_setprop (ExpandedDtb, NodeOffset, "kerneltype", "normal", StrLen (L"normal"));
        } else if (BootParams->BootMode == NVIDIA_L4T_BOOTMODE_RECOVERY) {
          fdt_setprop (ExpandedDtb, NodeOffset, "kerneltype", "recovery", StrLen (L"recovery"));
        }
      }

      Status = gBS->InstallConfigurationTable (&gFdtTableGuid, ExpandedDtb);
      if (EFI_ERROR (Status)) {
        ErrorPrint (L"DTB Installation Failed\r\n");
        gBS->FreePages ((EFI_PHYSICAL_ADDRESS)ExpandedDtb, EFI_SIZE_TO_PAGES (fdt_totalsize (ExpandedDtb)));
        ExpandedDtb = NULL;
        goto Exit;
      }
    }
  }

  if (((ANDROID_BOOTIMG_V3_HEADER *)Image)->HeaderVersion >= 3) {
    Status = BootAndroidV3Image (DeviceHandle, Image, BootParams);
  } else {
    DEBUG ((DEBUG_ERROR, "%a: Cmdline: \n", __FUNCTION__));
    DEBUG ((DEBUG_ERROR, "%a", ImageHeader.KernelArgs));

    Status = AndroidBootImgBoot (Image, ImageSize);
  }

  if (EFI_ERROR (Status)) {
    ErrorPrint (L"Failed to boot image: %r\r\n", Status);
    gBS->FreePages ((EFI_PHYSICAL_ADDRESS)ExpandedDtb, EFI_SIZE_TO_PAGES (fdt_totalsize (ExpandedDtb)));
    ExpandedDtb = NULL;
  }

  if (CurrentDtb != NULL) {
    Status = gBS->InstallConfigurationTable (&gFdtTableGuid, CurrentDtb);
  }

Exit:
  return Status;
}

/**
  This is the declaration of an EFI image entry point. This entry point is
  the same for UEFI Applications, UEFI OS Loaders, and UEFI Drivers, including
  both device drivers and bus drivers.

  The entry point for StackCheck application that should casue an abort due to stack overwrite.

  @param[in] ImageHandle    The image handle of this application.
  @param[in] SystemTable    The pointer to the EFI System Table.

  @retval EFI_SUCCESS    The operation completed successfully.

**/
EFI_STATUS
EFIAPI
AndroidLauncher (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_LOADED_IMAGE_PROTOCOL  *LoadedImage;
  EFI_STATUS                 Status;
  L4T_BOOT_PARAMS            BootParams;

  Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&LoadedImage);
  if (EFI_ERROR (Status)) {
    ErrorPrint (L"%a: Unable to locate loaded image: %r\r\n", __FUNCTION__, Status);
    return Status;
  }

  Status = ProcessBootParams (LoadedImage, &BootParams);
  if (EFI_ERROR (Status)) {
    ErrorPrint (L"%a: Unable to process boot parameters: %r\r\n", __FUNCTION__, Status);
    return Status;
  }

  // Not in else to allow fallback
  if (BootParams.BootMode == NVIDIA_L4T_BOOTMODE_BOOTIMG) {
    Status = BootAndroidStylePartition (LoadedImage->DeviceHandle, BOOTIMG_BASE_NAME, BOOTIMG_DTB_BASE_NAME, &BootParams);
    if (EFI_ERROR (Status)) {
      ErrorPrint (L"Failed to boot %s:%d partition\r\n", BOOTIMG_BASE_NAME, BootParams.BootChain);
    }
  } else if (BootParams.BootMode == NVIDIA_L4T_BOOTMODE_RECOVERY) {
    Status = BootAndroidStylePartition (LoadedImage->DeviceHandle, RECOVERY_BASE_NAME, RECOVERY_DTB_BASE_NAME, &BootParams);
    if (EFI_ERROR (Status)) {
      ErrorPrint (L"Failed to boot %s:%d partition\r\n", RECOVERY_BASE_NAME, BootParams.BootChain);
    }
  }

  return Status;
}
