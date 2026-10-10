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

  // Find valid Rootfs Chain. If not, select recovery kernel
  Status = ValidateRootfsStatus (BootParams);
  if (EFI_ERROR (Status)) {
    ErrorPrint (L"Failed to validate rootfs status: %r\r\n", Status);
  }

  // Store the current boot chain in volatile variable to allow chain loading
  Status = gRT->SetVariable (BOOT_OS_VARIABLE_NAME, &gNVIDIAPublicVariableGuid, EFI_VARIABLE_BOOTSERVICE_ACCESS|EFI_VARIABLE_RUNTIME_ACCESS, sizeof (BootParams->BootChain), &BootParams->BootChain);
  if (EFI_ERROR (Status)) {
    ErrorPrint (L"Failed to set OS variable: %r\r\n", Status);
  }

  return EFI_SUCCESS;
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

  Status = AndroidBootImgGetImgSize (&ImageHeader, &ImageSize);
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

    Status = AndroidBootImgGetImgSize (&ImageHeader, &ImageSize);
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

  DEBUG ((DEBUG_ERROR, "%a: Cmdline: \n", __FUNCTION__));
  DEBUG ((DEBUG_ERROR, "%a", ImageHeader.KernelArgs));

  Status = AndroidBootImgBoot (Image, ImageSize);
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
