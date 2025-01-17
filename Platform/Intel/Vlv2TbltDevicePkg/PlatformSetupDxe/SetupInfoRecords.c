/** @file

  Copyright (c) 2004  - 2019, Intel Corporation. All rights reserved.<BR>

  SPDX-License-Identifier: BSD-2-Clause-Patent

Module Name:

    SetupInfoRecords.c

Abstract:

    This is the filter driver to retrieve data hub entries.

Revision History:
--*/

#include "PlatformSetupDxe.h"
#include <Protocol/PciRootBridgeIo.h>
#include <Protocol/SimpleNetwork.h>
#include <Protocol/DevicePath.h>
#include <Protocol/DiskInfo.h>
#include <Protocol/IdeControllerInit.h>
#include <Protocol/MpService.h>
#include <Protocol/PchPlatformPolicy.h>
#include <Protocol/Smbios.h>
#include <IndustryStandard/SmBios.h>
#include <Library/IoLib.h>
#include <Guid/GlobalVariable.h>

#include "Valleyview.h"
#include "VlvAccess.h"
#include "PchAccess.h"
#include "SetupMode.h"
#include "PchCommonDefinitions.h"
#include <PlatformBaseAddresses.h>


typedef struct {
  UINT8  ID;
  CHAR8  String[16];
} VLV_REV;

typedef struct {
  UINT8 RevId;
  CHAR8 String[16];
} SB_REV;

//
// Silicon Steppings
//
SB_REV  SBRevisionTable[] = {
  {V_PCH_LPC_RID_0, "(A0 Stepping)"},
  {V_PCH_LPC_RID_1, "(A0 Stepping)"},
  {V_PCH_LPC_RID_2, "(A1 Stepping)"},
  {V_PCH_LPC_RID_3, "(A1 Stepping)"},
  {V_PCH_LPC_RID_4, "(B0 Stepping)"},
  {V_PCH_LPC_RID_5, "(B0 Stepping)"},
  {V_PCH_LPC_RID_6, "(B1 Stepping)"},
  {V_PCH_LPC_RID_7, "(B1 Stepping)"},
  {V_PCH_LPC_RID_8, "(B2 Stepping)"},
  {V_PCH_LPC_RID_9, "(B2 Stepping)"},
  {V_PCH_LPC_RID_A, "(B3 Stepping)"},
  {V_PCH_LPC_RID_B, "(B3 Stepping)"},
  {V_PCH_LPC_RID_C, "(C0 Stepping)"},
  {V_PCH_LPC_RID_D, "(C0 Stepping)"}
};

#define LEFT_JUSTIFY  0x01
#define PREFIX_SIGN   0x02
#define PREFIX_BLANK  0x04
#define COMMA_TYPE    0x08
#define LONG_TYPE     0x10
#define PREFIX_ZERO   0x20

#define ICH_REG_REV                 0x08
#define MSR_IA32_PLATFORM_ID        0x17


BOOLEAN                         mSetupInfoDone = FALSE;
UINT8                           mUseProductKey = 0;
EFI_GUID                        mProcessorProducerGuid;
EFI_HII_HANDLE                  mHiiHandle;
EFI_PLATFORM_CPU_INFO           mPlatformCpuInfo;
SYSTEM_CONFIGURATION            mSystemConfiguration;
EFI_PLATFORM_INFO_HOB           *mPlatformInfo;


#define memset SetMem

#define CHARACTER_NUMBER_FOR_VALUE  30

typedef enum {
  PCH_SATA_MODE_IDE = 0,
  PCH_SATA_MODE_AHCI,
  PCH_SATA_MODE_RAID,
  PCH_SATA_MODE_MAX
} PCH_SATA_MODE;

/**
  Acquire the string associated with the Index from smbios structure and return it.
  The caller is responsible for free the string buffer.

  @param OptionalStrStart   The start position to search the string
  @param Index              The index of the string to extract
  @param String             The string that is extracted

  @retval EFI_SUCCESS       The function returns EFI_SUCCESS always.

**/
EFI_STATUS
GetOptionalStringByIndex (
  IN      CHAR8                   *OptionalStrStart,
  IN      UINT8                   Index,
  OUT     CHAR16                  **String
  )
{
  UINTN          StrSize;

  if (Index == 0) {
    *String = AllocateZeroPool (sizeof (CHAR16));
    return EFI_SUCCESS;
  }

  StrSize = 0;
  do {
    Index--;
    OptionalStrStart += StrSize;
    StrSize           = AsciiStrSize (OptionalStrStart);
  } while (OptionalStrStart[StrSize] != 0 && Index != 0);

  if ((Index != 0) || (StrSize == 1)) {
    //
    // Meet the end of strings set but Index is non-zero, or
    // Find an empty string
    //
    return EFI_NOT_FOUND;
  } else {
    *String = AllocatePool (StrSize * sizeof (CHAR16));
    AsciiStrToUnicodeStr (OptionalStrStart, *String);
  }

  return EFI_SUCCESS;
}

/**
  VSPrint worker function that prints a Value as a decimal number in Buffer

  @param Buffer  Location to place ascii decimal number string of Value.
  @param Value   Decimal value to convert to a string in Buffer.
  @param Flags   Flags to use in printing decimal string, see file header for details.
  @param Width   Width of hex value.

  Number of characters printed.

**/
UINTN
EfiValueToString (
  IN  OUT CHAR16  *Buffer,
  IN  INT64       Value,
  IN  UINTN       Flags,
  IN  UINTN       Width
  )
{
  CHAR16    TempBuffer[CHARACTER_NUMBER_FOR_VALUE];
  CHAR16    *TempStr;
  CHAR16    *BufferPtr;
  UINTN     Count;
  UINTN     ValueCharNum;
  UINTN     Remainder;
  CHAR16    Prefix;
  UINTN     Index;
  BOOLEAN   ValueIsNegative;
  UINT64    TempValue;

  TempStr         = TempBuffer;
  BufferPtr       = Buffer;
  Count           = 0;
  ValueCharNum    = 0;
  ValueIsNegative = FALSE;

  if (Width > CHARACTER_NUMBER_FOR_VALUE - 1) {
    Width = CHARACTER_NUMBER_FOR_VALUE - 1;
  }

  if (Value < 0) {
    Value           = -Value;
    ValueIsNegative = TRUE;
  }

  do {
    TempValue = Value;
    Value = (INT64)DivU64x32 ((UINT64)Value, 10);
    Remainder = (UINTN)((UINT64)TempValue - 10 * Value);
    *(TempStr++) = (CHAR16)(Remainder + '0');
    ValueCharNum++;
    Count++;
    if ((Flags & COMMA_TYPE) == COMMA_TYPE) {
      if (ValueCharNum % 3 == 0 && Value != 0) {
        *(TempStr++) = ',';
        Count++;
      }
    }
  } while (Value != 0);

  if (ValueIsNegative) {
    *(TempStr++)    = '-';
    Count++;
  }

  if ((Flags & PREFIX_ZERO) && !ValueIsNegative) {
    Prefix = '0';
  } else {
    Prefix = ' ';
  }

  Index = Count;
  if (!(Flags & LEFT_JUSTIFY)) {
    for (; Index < Width; Index++) {
      *(TempStr++) = Prefix;
    }
  }

  //
  // Reverse temp string into Buffer.
  //
  if (Width > 0 && (UINTN) (TempStr - TempBuffer) > Width) {
    TempStr = TempBuffer + Width;
  }
  Index = 0;
  while (TempStr != TempBuffer) {
    *(BufferPtr++) = *(--TempStr);
    Index++;
  }

  *BufferPtr = 0;
  return Index;
}

static CHAR16 mHexStr[] = { L'0', L'1', L'2', L'3', L'4', L'5', L'6', L'7',
                            L'8', L'9', L'A', L'B', L'C', L'D', L'E', L'F' };

/**
  VSPrint worker function that prints a Value as a hex number in Buffer

  @param  Buffer  Location to place ascii hex string of Value.
  @param  Value   Hex value to convert to a string in Buffer.
  @param  Flags   Flags to use in printing Hex string, see file header for details.
  @param  Width   Width of hex value.

  @retval         Number of characters printed.

**/
UINTN
EfiValueToHexStr (
  IN  OUT CHAR16  *Buffer,
  IN  UINT64      Value,
  IN  UINTN       Flags,
  IN  UINTN       Width
  )
{
  CHAR16  TempBuffer[CHARACTER_NUMBER_FOR_VALUE];
  CHAR16  *TempStr;
  CHAR16  Prefix;
  CHAR16  *BufferPtr;
  UINTN   Count;
  UINTN   Index;

  TempStr   = TempBuffer;
  BufferPtr = Buffer;

  //
  // Count starts at one since we will null terminate. Each iteration of the
  // loop picks off one nibble. Oh yea TempStr ends up backwards
  //
  Count = 0;

  if (Width > CHARACTER_NUMBER_FOR_VALUE - 1) {
    Width = CHARACTER_NUMBER_FOR_VALUE - 1;
  }

  do {
    Index = ((UINTN)Value & 0xf);
    *(TempStr++) = mHexStr[Index];
    Value = RShiftU64 (Value, 4);
    Count++;
  } while (Value != 0);

  if (Flags & PREFIX_ZERO) {
    Prefix = '0';
  } else {
    Prefix = ' ';
  }

  Index = Count;
  if (!(Flags & LEFT_JUSTIFY)) {
    for (; Index < Width; Index++) {
      *(TempStr++) = Prefix;
    }
  }

  //
  // Reverse temp string into Buffer.
  //
  if (Width > 0 && (UINTN) (TempStr - TempBuffer) > Width) {
    TempStr = TempBuffer + Width;
  }
  Index = 0;
  while (TempStr != TempBuffer) {
    *(BufferPtr++) = *(--TempStr);
    Index++;
  }

  *BufferPtr = 0;
  return Index;
}

/*++
  Converts MAC address to Unicode string.
  The value is 64-bit and the resulting string will be 12
  digit hex number in pairs of digits separated by dashes.

  @param  String    string that will contain the value
  @param  MacAddr   add argument and description to function comment
  @param  AddrSize  add argument and description to function comment

**/
CHAR16 *
StrMacToString (
  OUT CHAR16              *String,
  IN  EFI_MAC_ADDRESS     *MacAddr,
  IN  UINT32              AddrSize
  )
{
  UINT32  i;

  for (i = 0; i < AddrSize; i++) {

    EfiValueToHexStr (
      &String[2 * i],
      MacAddr->Addr[i] & 0xFF,
      PREFIX_ZERO,
      2
      );
  }

  //
  // Terminate the string.
  //
  String[2 * AddrSize] = L'\0';

  return String;
}

VOID UpdateLatestBootTime() {
  UINTN                         VarSize;
  EFI_STATUS                   Status;
  UINT64                       TimeValue;
  CHAR16                       Buffer[40];
  if (mSystemConfiguration.LogBootTime != 1) {
    return;
  }
  VarSize = sizeof(TimeValue);
  Status = gRT->GetVariable(
                  BOOT_TIME_NAME,
                  &gEfiNormalSetupGuid,
                  NULL,
                  &VarSize,
                  &TimeValue
				          );
  if (EFI_ERROR(Status)) {
    return;
  }
  UnicodeSPrint (Buffer, sizeof (Buffer), L"%d ms", (UINT32)TimeValue);
  HiiSetString(mHiiHandle,STRING_TOKEN(STR_LOG_BOOT_TIME_VALUE), Buffer, NULL);
}

/**
  Setup data filter function. This function is invoked when there is data records
  available in the Data Hub.


  Standard event notification function arguments:
  @param Event          The event that is signaled.
  @param Context        Not used here.

  @retval EFI_STATUS

**/
VOID
PrepareSetupInformation (
  )
{
  EFI_STATUS                  Status;
  CHAR16                      *NewString;
  STRING_REF                  TokenToUpdate;
  CHAR16                      Version[100];         //Assuming that strings are < 100 UCHAR
  CHAR16                      ReleaseDate[100];     //Assuming that strings are < 100 UCHAR
  CHAR16                      ReleaseTime[100];     //Assuming that strings are < 100 UCHAR

  NewString = AllocateZeroPool (0x100);
  SetMem(Version, sizeof(Version), 0);
  SetMem(ReleaseDate, sizeof(ReleaseDate), 0);
  SetMem(ReleaseTime, sizeof(ReleaseTime), 0);

  Status = GetBiosVersionDateTime (
             Version,
             ReleaseDate,
             ReleaseTime
             );

  DEBUG ((EFI_D_ERROR, "GetBiosVersionDateTime :%s %s %s \n", Version, ReleaseDate, ReleaseTime));
  if (!EFI_ERROR (Status)) {
    UINTN         Length = 0;
    CHAR16        *BuildDateTime;

    Length = StrLen(ReleaseDate) + StrLen(ReleaseTime);

    BuildDateTime = AllocateZeroPool ((Length+2) * sizeof(CHAR16));
    StrCpy (BuildDateTime, ReleaseDate);
    StrCat (BuildDateTime, L" ");
    StrCat (BuildDateTime, ReleaseTime);

    TokenToUpdate = (STRING_REF)STR_BIOS_VERSION_VALUE;
    DEBUG ((EFI_D_ERROR, "update STR_BIOS_VERSION_VALUE\n"));
    HiiSetString(mHiiHandle, TokenToUpdate, Version, NULL);

    TokenToUpdate = (STRING_REF)STR_BIOS_BUILD_TIME_VALUE;
    DEBUG ((EFI_D_ERROR, "update STR_BIOS_BUILD_TIME_VALUE\n"));
    HiiSetString(mHiiHandle, TokenToUpdate, BuildDateTime, NULL);
  }

  gBS->FreePool(NewString);
}

/**

  Routine Description: update the SETUP info for "Additional Information" which is SMBIOS info.

  @retval EFI_STATUS

**/
EFI_STATUS
UpdateAdditionalInformation (
  )
{
  EFI_STATUS                      Status;
  EFI_SMBIOS_PROTOCOL             *Smbios;
  EFI_SMBIOS_HANDLE               SmbiosHandle;
  EFI_SMBIOS_TABLE_HEADER         *SmbiosRecord;
  SMBIOS_TABLE_TYPE0              *Type0Record;
  SMBIOS_TABLE_TYPE4              *Type4Record;
  SMBIOS_TABLE_TYPE7              *Type7Record;
  SMBIOS_TABLE_TYPE17             *Type17Record;
  UINT8                           StrIndex;
  CHAR16                          *BiosVersion = NULL;
  CHAR16                          *ProcessorVersion = NULL;
  CHAR16                          *IfwiVersion = NULL;
  UINT16                          SearchIndex;
  EFI_STRING_ID                   TokenToUpdate;
  UINT32                          MicrocodeRevision;
  CHAR16                          NewString[0x100];
  UINTN                           TotalMemorySize;
  UINT16                          MemorySpeed;

  Status = gBS->LocateProtocol (
                  &gEfiSmbiosProtocolGuid,
                  NULL,
                  (VOID **) &Smbios
                  );
  ASSERT_EFI_ERROR (Status);

  SmbiosHandle = SMBIOS_HANDLE_PI_RESERVED;
  TotalMemorySize = 0;
  MemorySpeed = 0xffff;
  do {
    Status = Smbios->GetNext (
                       Smbios,
                       &SmbiosHandle,
                       NULL,
                       &SmbiosRecord,
                       NULL
                       );


    if (SmbiosRecord->Type == EFI_SMBIOS_TYPE_PROCESSOR_INFORMATION) {
      Type4Record = (SMBIOS_TABLE_TYPE4 *) SmbiosRecord;
      UnicodeSPrint (NewString, sizeof (NewString), L"%d.%d GHz",
        Type4Record->CurrentSpeed / 1000,
        Type4Record->CurrentSpeed % 1000
        );
      HiiSetString (mHiiHandle, STR_PROCESSOR_SPEED_VALUE, NewString, NULL);

      StrIndex = Type4Record->ProcessorVersion;
      GetOptionalStringByIndex ((CHAR8*)((UINT8*)Type4Record + Type4Record->Hdr.Length), StrIndex, &ProcessorVersion);
      HiiSetString (mHiiHandle, STR_PROCESSOR_VERSION_VALUE, ProcessorVersion, NULL);

      MicrocodeRevision = (UINT32) RShiftU64 (AsmReadMsr64 (EFI_MSR_IA32_BIOS_SIGN_ID), 32);
      UnicodeSPrint (NewString, sizeof (NewString), L"%8x", MicrocodeRevision);
      HiiSetString (mHiiHandle, STR_PROCESSOR_MICROCODE_VALUE, NewString, NULL);
    }
    if (SmbiosRecord->Type == SMBIOS_TYPE_CACHE_INFORMATION) {
      Type7Record = (SMBIOS_TABLE_TYPE7 *) SmbiosRecord;
      UnicodeSPrint (NewString, sizeof (NewString), L"%d KB", Type7Record->InstalledSize);
      switch (Type7Record->CacheConfiguration & 0x03) {
      case 0:
        //
        // Level 1 Cache
        //
        if (Type7Record->SystemCacheType == CacheTypeInstruction) {
          HiiSetString (mHiiHandle, STR_PROCESSOR_L1_INSTR_CACHE_VALUE, NewString, NULL);
        }
        if (Type7Record->SystemCacheType == CacheTypeData) {
          HiiSetString (mHiiHandle, STR_PROCESSOR_L1_DATA_CACHE_VALUE, NewString, NULL);
        }
        break;
      case 1:
        //
        // Level 2 Cache
        //
        HiiSetString (mHiiHandle, STR_PROCESSOR_L2_CACHE_VALUE, NewString, NULL);
        break;
      default:
        break;
      }
      HiiSetString (mHiiHandle, STR_PROCESSOR_MICROCODE_VALUE, NewString, NULL);
    }
    if (SmbiosRecord->Type == EFI_SMBIOS_TYPE_MEMORY_DEVICE) {
      Type17Record = (SMBIOS_TABLE_TYPE17 *) SmbiosRecord;
      if (Type17Record->Size > 0) {
        if ((Type17Record->Size & BIT15) != 0) {
          //
          // Size is in KB
          //
          TotalMemorySize = TotalMemorySize + Type17Record->Size;
        } else {
          //
          // Size is in MB
          //
          TotalMemorySize = TotalMemorySize + (UINTN)LShiftU64 (Type17Record->Size, 10);
        }
        if (Type17Record->Speed < MemorySpeed) {
          MemorySpeed = Type17Record->Speed;
        }
      }
    }

    if (SmbiosRecord->Type == EFI_SMBIOS_TYPE_BIOS_INFORMATION) {
      Type0Record = (SMBIOS_TABLE_TYPE0 *) SmbiosRecord;
      StrIndex = Type0Record->BiosVersion;
      GetOptionalStringByIndex ((CHAR8*)((UINT8*)Type0Record + Type0Record->Hdr.Length), StrIndex, &BiosVersion);
      TokenToUpdate = STRING_TOKEN (STR_BIOS_VERSION_VALUE);
      for (SearchIndex = 0x0; SearchIndex < SMBIOS_STRING_MAX_LENGTH; SearchIndex++) {
        if (BiosVersion[SearchIndex] == 0x0020) {
          BiosVersion[SearchIndex] = 0x0000;
          IfwiVersion = (CHAR16 *)(&BiosVersion[SearchIndex+1]);
          break;
        } else if (BiosVersion[SearchIndex] == 0x0000) {
          break;
        }
      }
      HiiSetString (mHiiHandle, TokenToUpdate, BiosVersion, NULL);

      //
      // Check IfwiVersion, to avoid no IFWI version in SMBIOS Type 0 strucntion
      //
      if(IfwiVersion) {
        TokenToUpdate = STRING_TOKEN (STR_IFWI_VERSION_VALUE);
        HiiSetString (mHiiHandle, TokenToUpdate, IfwiVersion, NULL);
      }
    }
  } while (!EFI_ERROR(Status));

  if ((TotalMemorySize % 1024) != 0) {
    UnicodeSPrint (NewString, sizeof (NewString), L"%d.%d GB", TotalMemorySize / 1024, ((TotalMemorySize % 1024) * 100) / 1024);
  } else {
    UnicodeSPrint (NewString, sizeof (NewString), L"%d GB", TotalMemorySize / 1024);
  }
  HiiSetString (mHiiHandle, STR_TOTAL_MEMORY_SIZE_VALUE, NewString, NULL);

  UnicodeSPrint (NewString, sizeof (NewString), L"%d MHz", MemorySpeed);
  HiiSetString(mHiiHandle, STR_SYSTEM_MEMORY_SPEED_VALUE, NewString, NULL);

  UpdateLatestBootTime();

  return  EFI_SUCCESS;
}

VOID
UpdateCPUInformation ()
{
  CHAR16								Buffer[40];
  UINT16                                FamilyId;
  UINT8                                 Model;
  UINT8                                 SteppingId;
  UINT8                                 ProcessorType;
  EFI_STATUS                            Status;
  EFI_MP_SERVICES_PROTOCOL              *MpService;
  UINTN                                 MaximumNumberOfCPUs;
  UINTN                                 NumberOfEnabledCPUs;
  UINT32								Buffer32 = 0xFFFFFFFF;   // Keep buffer with unknown device

  EfiCpuVersion (&FamilyId, &Model, &SteppingId, &ProcessorType);

  //
  //we need raw Model data
  //
  Model = Model & 0xf;

  //
  //Family/Model/Step
  //
  UnicodeSPrint (Buffer, sizeof (Buffer), L"%d/%d/%d", FamilyId,  Model, SteppingId);
  HiiSetString(mHiiHandle,STRING_TOKEN(STR_PROCESSOR_ID_VALUE), Buffer, NULL);

  Status = gBS->LocateProtocol (
                  &gEfiMpServiceProtocolGuid,
                  NULL,
                  (void **)&MpService
                  );
  if (!EFI_ERROR (Status)) {
    //
    // Determine the number of processors
    //
    MpService->GetNumberOfProcessors (
                 MpService,
                 &MaximumNumberOfCPUs,
                 &NumberOfEnabledCPUs
                 );
    UnicodeSPrint (Buffer, sizeof (Buffer), L"%d", MaximumNumberOfCPUs);
    HiiSetString(mHiiHandle,STRING_TOKEN(STR_PROCESSOR_CORE_VALUE), Buffer, NULL);
  }
  //
  // Update Mobile / Desktop / Tablet SKU
  //
  Buffer32 =(UINT32) RShiftU64 (EfiReadMsr (MSR_IA32_PLATFORM_ID), 50) & 0x07;

  switch(Buffer32){
      case 0x0:
        UnicodeSPrint (Buffer, sizeof (Buffer), L"(%d) - ISG SKU SOC", Buffer32);
        break;
      case 0x01:
        UnicodeSPrint (Buffer, sizeof (Buffer), L"(%d) - Mobile SKU SOC", Buffer32);
        break;
      case 0x02:
        UnicodeSPrint (Buffer, sizeof (Buffer), L"(%d) - Desktop SKU SOC", Buffer32);
        break;
      case 0x03:
        UnicodeSPrint (Buffer, sizeof (Buffer), L"(%d) - Mobile SKU SOC", Buffer32);
        break;
      default:
        UnicodeSPrint (Buffer, sizeof (Buffer), L"(%d) - Unknown SKU SOC", Buffer32);
        break;
    }
  HiiSetString(mHiiHandle,STRING_TOKEN(STR_PROCESSOR_SKU_VALUE), Buffer, NULL);

}


EFI_STATUS
SearchChildHandle(
  EFI_HANDLE Father,
  EFI_HANDLE *Child
  )
{
  EFI_STATUS                                                 Status;
  UINTN                                                          HandleIndex;
  EFI_GUID                                                     **ProtocolGuidArray = NULL;
  UINTN                                                          ArrayCount;
  UINTN                                                          ProtocolIndex;
  UINTN                                                          OpenInfoCount;
  UINTN                                                          OpenInfoIndex;
  EFI_OPEN_PROTOCOL_INFORMATION_ENTRY  *OpenInfo = NULL;
  UINTN                                                          mHandleCount;
  EFI_HANDLE                                                 *mHandleBuffer= NULL;

  //
  // Retrieve the list of all handles from the handle database
  //
  Status = gBS->LocateHandleBuffer (
                  AllHandles,
                  NULL,
                  NULL,
                  &mHandleCount,
                  &mHandleBuffer
                  );

  for (HandleIndex = 0; HandleIndex < mHandleCount; HandleIndex++)
  {
    //
    // Retrieve the list of all the protocols on each handle
    //
    Status = gBS->ProtocolsPerHandle (
                    mHandleBuffer[HandleIndex],
                    &ProtocolGuidArray,
                    &ArrayCount
                    );
    if (!EFI_ERROR (Status))
    {
      for (ProtocolIndex = 0; ProtocolIndex < ArrayCount; ProtocolIndex++)
      {
        Status = gBS->OpenProtocolInformation (
                        mHandleBuffer[HandleIndex],
                        ProtocolGuidArray[ProtocolIndex],
                        &OpenInfo,
                        &OpenInfoCount
                        );
        if (!EFI_ERROR (Status))
        {
          for (OpenInfoIndex = 0; OpenInfoIndex < OpenInfoCount; OpenInfoIndex++)
          {
            if(OpenInfo[OpenInfoIndex].AgentHandle == Father)
            {
              if ((OpenInfo[OpenInfoIndex].Attributes & EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER) == EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER)
              {
                *Child = mHandleBuffer[HandleIndex];
		  Status = EFI_SUCCESS;
		  goto TryReturn;
              }
            }
          }
	   Status = EFI_NOT_FOUND;
        }
      }
      if(OpenInfo != NULL)
      {
        FreePool(OpenInfo);
	 OpenInfo = NULL;
      }
    }
    FreePool (ProtocolGuidArray);
    ProtocolGuidArray = NULL;
  }
TryReturn:
  if(OpenInfo != NULL)
  {
    FreePool (OpenInfo);
    OpenInfo = NULL;
  }
  if(ProtocolGuidArray != NULL)
  {
    FreePool(ProtocolGuidArray);
    ProtocolGuidArray = NULL;
  }
  if(mHandleBuffer != NULL)
  {
    FreePool (mHandleBuffer);
    mHandleBuffer = NULL;
  }
  return Status;
}

EFI_STATUS
JudgeHandleIsPCIDevice(
  EFI_HANDLE    Handle,
  UINT8            Device,
  UINT8            Funs
  )
{
  EFI_STATUS  Status;
  EFI_DEVICE_PATH   *DPath;

  Status = gBS->HandleProtocol (
                  Handle,
                  &gEfiDevicePathProtocolGuid,
                  (VOID **) &DPath
                  );
  if(!EFI_ERROR(Status))
  {
    while(!IsDevicePathEnd(DPath))
    {
      if((DPath->Type == HARDWARE_DEVICE_PATH) && (DPath->SubType == HW_PCI_DP))
      {
        PCI_DEVICE_PATH   *PCIPath;

        PCIPath = (PCI_DEVICE_PATH*) DPath;
        DPath = NextDevicePathNode(DPath);
        if(IsDevicePathEnd(DPath) && (PCIPath->Device == Device) && (PCIPath->Function == Funs))
        {
          return EFI_SUCCESS;
        }
      }
      else
      {
        DPath = NextDevicePathNode(DPath);
      }
    }
  }
  return EFI_UNSUPPORTED;
}

EFI_STATUS
GetDriverName(
  EFI_HANDLE   Handle,
  CHAR16         *Name
  )
{
  EFI_DRIVER_BINDING_PROTOCOL        *BindHandle = NULL;
  EFI_STATUS                                        Status;
  UINT32                                               Version;
  UINT16                                               *Ptr;
  Status = gBS->OpenProtocol(
                  Handle,
                  &gEfiDriverBindingProtocolGuid,
                  (VOID**)&BindHandle,
                  NULL,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );

  if (EFI_ERROR(Status))
  {
    return EFI_NOT_FOUND;
  }

  Version = BindHandle->Version;
  Ptr = (UINT16*)&Version;
  UnicodeSPrint(Name, 40, L"%d.%d.%d", Version >> 24 , (Version >>16)& 0x0f ,*(Ptr));

  return EFI_SUCCESS;
}

EFI_STATUS
GetGOPDriverName(
  CHAR16 *Name
  )
{
  UINTN                         HandleCount;
  EFI_HANDLE                *Handles= NULL;
  UINTN                         Index;
  EFI_STATUS                Status;
  EFI_HANDLE                Child = 0;

  Status = gBS->LocateHandleBuffer(
		              ByProtocol,
		              &gEfiDriverBindingProtocolGuid,
		              NULL,
		              &HandleCount,
		              &Handles
                  );
  for (Index = 0; Index < HandleCount ; Index++)
  {
    Status = SearchChildHandle(Handles[Index], &Child);
    if(!EFI_ERROR(Status))
    {
      Status = JudgeHandleIsPCIDevice(
                 Child,
                 0x02,
                 0x00
                 );
      if(!EFI_ERROR(Status))
      {
        return GetDriverName(Handles[Index], Name);
      }
    }
  }
  return EFI_UNSUPPORTED;
}

EFI_STATUS
UpdatePlatformInformation (
  )
{
  UINT32                   MicroCodeVersion;
  CHAR16                   Buffer[40];
  EFI_STATUS               Status;
  UINT8                    CpuFlavor=0;
  EFI_PEI_HOB_POINTERS     GuidHob;
  UINTN                    NumHandles;
  EFI_HANDLE                        *HandleBuffer;
  UINTN                             Index;
  DXE_PCH_PLATFORM_POLICY_PROTOCOL  *PchPlatformPolicy;
  UINTN                             PciD31F0RegBase;
  UINT8                             count;
  UINT8                             Data8;
  UINT8                             PIDData8;

  CHAR16                            Name[40];
  UINT32                            MrcVersion;

  //
  // Get the HOB list.  If it is not present, then ASSERT.
  //
  GuidHob.Raw = GetHobList ();
  if (GuidHob.Raw != NULL) {
    if ((GuidHob.Raw = GetNextGuidHob (&gEfiPlatformInfoGuid, GuidHob.Raw)) != NULL) {
      mPlatformInfo = GET_GUID_HOB_DATA (GuidHob.Guid);
    }
  }

  Status = GetGOPDriverName(Name);

  if (!EFI_ERROR(Status))
  {
    HiiSetString(mHiiHandle, STRING_TOKEN(STR_GOP_VALUE), Name, NULL);
  }


  //
  // CpuFlavor
  // ISG-DC Tablet        000
  // VLV-QC Tablet        001
  // VLV-QC Desktop       010
  // VLV-QC Notebook      011
  //
  CpuFlavor = RShiftU64 (EfiReadMsr (MSR_IA32_PLATFORM_ID), 50) & 0x07;

  switch(CpuFlavor){
    case 0x0:
      UnicodeSPrint (Buffer, sizeof (Buffer), L"%s (%01x)", L"VLV-DC Tablet", CpuFlavor);
      break;
    case 0x01:
      UnicodeSPrint (Buffer, sizeof (Buffer), L"%s (%01x)", L"VLV-QC Notebook", CpuFlavor);
      break;
    case 0x02:
      UnicodeSPrint (Buffer, sizeof (Buffer), L"%s (%01x)", L"VLV-QC Desktop", CpuFlavor);
      break;
    case 0x03:
      UnicodeSPrint (Buffer, sizeof (Buffer), L"%s (%01x)", L"VLV-QC Notebook", CpuFlavor);
      break;
    default:
      UnicodeSPrint (Buffer, sizeof (Buffer), L"%s (%01x)", L"Unknown CPU", CpuFlavor);
      break;
  }
  HiiSetString(mHiiHandle,STRING_TOKEN(STR_CPU_FLAVOR_VALUE), Buffer, NULL);

  if ( NULL != mPlatformInfo) {
    //
    //BoardId
    //
    switch(mPlatformInfo->BoardId){
      case 0x2:
        UnicodeSPrint (Buffer, sizeof (Buffer), L"BAY LAKE RVP(%02x)", mPlatformInfo->BoardId);
        break;

      case 0x4:
        UnicodeSPrint (Buffer, sizeof (Buffer), L"BAY LAKE FFRD(%02x)", mPlatformInfo->BoardId);
        break;

      case 0x5:
        UnicodeSPrint (Buffer, sizeof (Buffer), L"BAY ROCK RVP DDR3L (%02x)", mPlatformInfo->BoardId);
        break;

      case 0x20:
        UnicodeSPrint (Buffer, sizeof (Buffer), L"BAYLEY BAY (%02x)", mPlatformInfo->BoardId);
        break;

      case 0x30:
        UnicodeSPrint (Buffer, sizeof (Buffer), L"BAKER SPORT (%02x)", mPlatformInfo->BoardId);
        break;

      case 0x0:
        UnicodeSPrint (Buffer, sizeof (Buffer), L"ALPINE VALLEY (%x)", mPlatformInfo->BoardId);
        break;

      case 0x3:
        UnicodeSPrint (Buffer, sizeof (Buffer), L"BAY LAKE FFD8 (%x)", mPlatformInfo->BoardId);
        break;

      default:
        UnicodeSPrint (Buffer, sizeof (Buffer), L"Unknown BOARD (%02x)", mPlatformInfo->BoardId);
        break;
    }
    HiiSetString(mHiiHandle,STRING_TOKEN(STR_BOARD_ID_VALUE), Buffer, NULL);


    //
    // Get Board FAB ID Info from protocol, update into the NVS area.
    // bit0~bit3 are for Fab ID, 0x0F means unknow FAB.
    //
    if(mPlatformInfo->BoardRev == 0x0F) {
      UnicodeSPrint (Buffer, sizeof (Buffer), L"%s", L"Unknown FAB");
      HiiSetString(mHiiHandle,STRING_TOKEN(STR_FAB_ID_VALUE), Buffer, NULL);
    } else {
      UnicodeSPrint (Buffer, sizeof (Buffer), L"%2x", mPlatformInfo->BoardRev);
      HiiSetString(mHiiHandle,STRING_TOKEN(STR_FAB_ID_VALUE), Buffer, NULL);
    }
  }

  //
  //Update MRC Version
  //
  MrcVersion = 0x00000000;
  MrcVersion &= 0xffff;
  Index = EfiValueToString (Buffer, MrcVersion/100, PREFIX_ZERO, 0);
  StrCat (Buffer, L".");
  EfiValueToString (Buffer + Index + 1, (MrcVersion%100)/10, PREFIX_ZERO, 0);
  EfiValueToString (Buffer + Index + 2, (MrcVersion%100)%10, PREFIX_ZERO, 0);
  HiiSetString(mHiiHandle,STRING_TOKEN(STR_MRC_VERSION_VALUE), Buffer, NULL);

  //
  //Update Soc Version
  //

  //
  // Retrieve all instances of PCH Platform Policy protocol
  //
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gDxePchPlatformPolicyProtocolGuid,
                  NULL,
                  &NumHandles,
                  &HandleBuffer
                  );
  if (!EFI_ERROR (Status)) {
    //
    // Find the matching PCH Policy protocol
    //
    for (Index = 0; Index < NumHandles; Index++) {
      Status = gBS->HandleProtocol (
                      HandleBuffer[Index],
                      &gDxePchPlatformPolicyProtocolGuid,
                      (void **)&PchPlatformPolicy
                      );
      if (!EFI_ERROR (Status)) {
        PciD31F0RegBase = MmPciAddress (
                            0,
                            PchPlatformPolicy->BusNumber,
                            PCI_DEVICE_NUMBER_PCH_LPC,
                            PCI_FUNCTION_NUMBER_PCH_LPC,
                            0
                            );

         Data8 = MmioRead8 (PciD31F0RegBase + R_PCH_LPC_RID_CC);
         count = ARRAY_SIZE (SBRevisionTable);
         for (Index = 0; Index < count; Index++) {
           if(Data8 == SBRevisionTable[Index].RevId) {
              UnicodeSPrint (Buffer, sizeof (Buffer), L"%02x %a", Data8, SBRevisionTable[Index].String);
              HiiSetString(mHiiHandle,STRING_TOKEN(STR_SOC_VALUE), Buffer, NULL);
             break;
           }
         }
        break;
      }
    }
  }

  //
  // Microcode Revision
  //
  EfiWriteMsr (EFI_MSR_IA32_BIOS_SIGN_ID, 0);
  EfiCpuid (EFI_CPUID_VERSION_INFO, NULL);
  MicroCodeVersion = (UINT32) RShiftU64 (EfiReadMsr (EFI_MSR_IA32_BIOS_SIGN_ID), 32);
  UnicodeSPrint (Buffer, sizeof (Buffer), L"%x", MicroCodeVersion);
  HiiSetString(mHiiHandle,STRING_TOKEN(STR_PROCESSOR_MICROCODE_VALUE), Buffer, NULL);

  //
  // Punit Version
  //
  Data8 = 0;
  UnicodeSPrint (Buffer, sizeof (Buffer), L"0x%x", Data8);
  HiiSetString(mHiiHandle,STRING_TOKEN(STR_PUNIT_FW_VALUE), Buffer, NULL);

  //
  //  PMC Version
  //
  Data8 = (UINT8)((MmioRead32 (PMC_BASE_ADDRESS + R_PCH_PMC_PRSTS)>>16)&0x00FF);
  PIDData8 = (UINT8)((MmioRead32 (PMC_BASE_ADDRESS + R_PCH_PMC_PRSTS)>>24)&0x00FF);
  UnicodeSPrint (Buffer, sizeof (Buffer), L"0x%X_%X",PIDData8, Data8);
  HiiSetString(mHiiHandle,STRING_TOKEN(STR_PMC_FW_VALUE), Buffer, NULL);

  return EFI_SUCCESS;
}

/**

  Update SATA Drivesize Strings for Setup and Boot order

  @param NewString - pointer to string.
  @param DeviceSpeed - speed of drive.

**/
VOID
GetDeviceSpeedString (
  CHAR16                      *NewString,
  IN UINTN                    DeviceSpeed
  )
{
  if (DeviceSpeed == 0x01) {
    StrCat (NewString, L"1.5Gb/s");
  } else if (DeviceSpeed == 0x02) {
    StrCat (NewString, L"3.0Gb/s");
  } else if (DeviceSpeed == 0x03) {
    StrCat (NewString, L"6.0Gb/s");
  } else if (DeviceSpeed == 0x0) {

  }
}

UINT8
GetChipsetSataPortSpeed (
  UINTN PortNum
  )
{
  UINT32                      DeviceSpeed;
  UINT8                       DeviceConfigStatus;
  UINT32                      IdeAhciBar;
  EFI_PHYSICAL_ADDRESS        MemBaseAddress = 0;
  UINT8                       FunNum;

  DeviceSpeed = 0x01; // generation 1


  //
  // Allocate the AHCI BAR
  //
    FunNum = PCI_FUNCTION_NUMBER_PCH_SATA;
    MemBaseAddress = 0x0ffffffff;
    gDS->AllocateMemorySpace (
           EfiGcdAllocateMaxAddressSearchBottomUp,
           EfiGcdMemoryTypeMemoryMappedIo,
           N_PCH_SATA_ABAR_ALIGNMENT,  // 2^11: 2K Alignment
           V_PCH_SATA_ABAR_LENGTH,     // 2K Length
           &MemBaseAddress,
           mImageHandle,
           NULL
           );
    IdeAhciBar = MmioRead32 (
                   MmPciAddress (
				     0,
                     0,
                     PCI_DEVICE_NUMBER_PCH_SATA,
                     FunNum,
                     R_PCH_SATA_ABAR
                     )
                   );
    IdeAhciBar &= 0xFFFFF800;
    DeviceConfigStatus = 0;
    if (IdeAhciBar == 0) {
      DeviceConfigStatus = 1;
      IdeAhciBar = (UINT32)MemBaseAddress;
      MmioWrite32 (
        MmPciAddress (0, 0, PCI_DEVICE_NUMBER_PCH_SATA, FunNum, R_PCH_SATA_ABAR),
        IdeAhciBar
        );
      MmioOr16 (
        MmPciAddress (0, 0, PCI_DEVICE_NUMBER_PCH_SATA, FunNum, R_PCH_SATA_COMMAND),
        B_PCH_SATA_COMMAND_MSE
        );
    }

    if (mSystemConfiguration.SataType == PCH_SATA_MODE_IDE){
      //
      // Program the "Ports Implemented Register"
      //
      MmioAndThenOr32 (IdeAhciBar + R_PCH_SATA_AHCI_PI, (UINT32)~(B_PCH_SATA_PORT0_IMPLEMENTED + B_PCH_SATA_PORT1_IMPLEMENTED), (UINT32)(B_PCH_SATA_PORT0_IMPLEMENTED + B_PCH_SATA_PORT1_IMPLEMENTED));
    }

    switch (PortNum)
    {
      case 0:
        DeviceSpeed = *(volatile UINT32 *)(UINTN)(IdeAhciBar + R_PCH_SATA_AHCI_P0SSTS);
        break;
      case 1:
        DeviceSpeed = *(volatile UINT32 *)(UINTN)(IdeAhciBar + R_PCH_SATA_AHCI_P1SSTS);
        break;
    }

    if (MemBaseAddress) {
      gDS->FreeMemorySpace (
             MemBaseAddress,
             V_PCH_SATA_ABAR_LENGTH
             );
    }

  if (DeviceConfigStatus) {
    IdeAhciBar = 0;
    MmioWrite32 (
      MmPciAddress (0, 0, PCI_DEVICE_NUMBER_PCH_SATA, FunNum, R_PCH_SATA_ABAR),
      IdeAhciBar
      );
  }

  DeviceSpeed = (UINT8)((DeviceSpeed >> 4) & 0x0F);

  return (UINT8)DeviceSpeed;
}

/**

  IDE data filter function.

**/
void
IdeDataFilter (void)
{
  EFI_STATUS                  Status;
  UINTN                       HandleCount;
  EFI_HANDLE                  *HandleBuffer;
  EFI_DISK_INFO_PROTOCOL      *DiskInfo;
  EFI_DEVICE_PATH_PROTOCOL    *DevicePath, *DevicePathNode;
  PCI_DEVICE_PATH             *PciDevicePath;
  UINTN                       Index;
  UINT8                       Index1;
  UINT32                      BufferSize;
  UINT32                      DriveSize;
  UINT32                      IdeChannel;
  UINT32                      IdeDevice;
  EFI_ATA_IDENTIFY_DATA       *IdentifyDriveInfo;
  CHAR16                      *NewString;
  CHAR16                      SizeString[20];
  STRING_REF                  NameToUpdate;
  CHAR8                       StringBuffer[0x100];
  UINT32                      DeviceSpeed;
  UINTN                       PortNumber;

  //
  // Assume no line strings is longer than 256 bytes.
  //
  NewString = AllocateZeroPool (0x100);
  PciDevicePath = NULL;

  //
  // Fill IDE Infomation
  //
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiDiskInfoProtocolGuid,
                  NULL,
                  &HandleCount,
                  &HandleBuffer
				  );

  if (EFI_ERROR (Status)) {
    return;
  }

  for (Index = 0; Index < HandleCount; Index++) {

    Status = gBS->HandleProtocol (
                    HandleBuffer[Index],
                    &gEfiDevicePathProtocolGuid,
                    (VOID*)&DevicePath
				    );
    ASSERT_EFI_ERROR (Status);

    DevicePathNode = DevicePath;
    while (!IsDevicePathEnd (DevicePathNode) ) {
      if  ((DevicePathType (DevicePathNode) == HARDWARE_DEVICE_PATH) &&
           ( DevicePathSubType (DevicePathNode) == HW_PCI_DP)) {
        PciDevicePath = (PCI_DEVICE_PATH *) DevicePathNode;
        break;
      }
      DevicePathNode    = NextDevicePathNode (DevicePathNode);
    }

    if (PciDevicePath == NULL) {
      continue;
    }

    //
    // Check for onboard IDE
    //
    if (PciDevicePath->Device== PCI_DEVICE_NUMBER_PCH_SATA) {
      Status = gBS->HandleProtocol (
	                  HandleBuffer[Index],
					  &gEfiDiskInfoProtocolGuid,
					  (void **)&DiskInfo
					  );
      ASSERT_EFI_ERROR (Status);

      Status = DiskInfo->WhichIde (
	                       DiskInfo,
                           &IdeChannel,
                           &IdeDevice
						   );
      ASSERT_EFI_ERROR (Status);

      IdentifyDriveInfo = AllocatePool (sizeof(EFI_ATA_IDENTIFY_DATA));

      BufferSize = sizeof(EFI_ATA_IDENTIFY_DATA);
      Status = DiskInfo->Identify (
	                       DiskInfo,
                           IdentifyDriveInfo,
                           &BufferSize
                           );
      ASSERT_EFI_ERROR(Status);

      //
      // Onboard SATA Devices
      //
      if (PciDevicePath->Function == PCI_FUNCTION_NUMBER_PCH_SATA) {
        if (IdeChannel == 0 && IdeDevice == 0) {
          NameToUpdate = (STRING_REF)STR_SATA0_NAME;
        } else if (IdeChannel == 1 && IdeDevice == 0) {
          NameToUpdate = (STRING_REF)STR_SATA1_NAME;
        } else {
          continue;
        }
      } else {
        continue;
      }

      ZeroMem(StringBuffer, sizeof(StringBuffer));
      CopyMem(
        StringBuffer,
        (CHAR8 *)&IdentifyDriveInfo->ModelName,
        sizeof(IdentifyDriveInfo->ModelName)
        );
      SwapEntries(StringBuffer);
      AsciiToUnicode(StringBuffer, NewString);

	  //
      // Chap it off after 16 characters
	  //
      NewString[16] = 0;

      //
      // For HardDisk append the size. Otherwise display atapi
      //
      if ((IdentifyDriveInfo->config & 0x8000) == 00) {
        //
        // 48 bit address feature set is supported, get maximum capacity
        //
        if ((IdentifyDriveInfo->command_set_supported_83 & 0x0400) == 0) {
        DriveSize = (((((IdentifyDriveInfo->user_addressable_sectors_hi << 16) +
                      IdentifyDriveInfo->user_addressable_sectors_lo) / 1000) * 512) / 1000);
        } else {
          DriveSize    = IdentifyDriveInfo->maximum_lba_for_48bit_addressing[0];
          for (Index1 = 1; Index1 < 4; Index1++) {
            //
            // Lower byte goes first: word[100] is the lowest word, word[103] is highest
            //
            DriveSize |= LShiftU64(IdentifyDriveInfo->maximum_lba_for_48bit_addressing[Index1], 16 * Index1);
          }
          DriveSize = (UINT32) DivU64x32(MultU64x32(DivU64x32(DriveSize, 1000), 512), 1000);
        }

        StrCat (NewString, L"(");
        EfiValueToString (SizeString, DriveSize/1000, PREFIX_BLANK, 0);
        StrCat (NewString, SizeString);
        StrCat (NewString, L".");
        EfiValueToString (SizeString, (DriveSize%1000)/100, PREFIX_BLANK, 0);
        StrCat (NewString, SizeString);
        StrCat (NewString, L"GB");
      } else {
        StrCat (NewString, L"(ATAPI");
      }

      //
      // Update SPEED.
      //
      PortNumber = (IdeDevice << 1) + IdeChannel;
      DeviceSpeed = GetChipsetSataPortSpeed(PortNumber);

      if (DeviceSpeed) {
        StrCat (NewString, L"-");
        GetDeviceSpeedString( NewString, DeviceSpeed);
      }

      StrCat (NewString, L")");

      HiiSetString(mHiiHandle, NameToUpdate, NewString, NULL);

    }
  }

  if (HandleBuffer != NULL) {
    gBS->FreePool (HandleBuffer);
  }

  gBS->FreePool(NewString);

  return;
}


VOID
EFIAPI
SetupInfo (void)
{
  EFI_STATUS                  Status;
  UINTN                       VarSize;
  EFI_PEI_HOB_POINTERS        GuidHob;

  if (mSetupInfoDone) {
      return;
  }

  VarSize = sizeof(SYSTEM_CONFIGURATION);
  Status = gRT->GetVariable(
                  NORMAL_SETUP_NAME,
                  &gEfiNormalSetupGuid,
                  NULL,
                  &VarSize,
                  &mSystemConfiguration
				  );

  if (EFI_ERROR (Status) || VarSize != sizeof(SYSTEM_CONFIGURATION)) {
    //The setup variable is corrupted
    VarSize = sizeof(SYSTEM_CONFIGURATION);
    Status = gRT->GetVariable(
              L"SetupRecovery",
              &gEfiNormalSetupGuid,
              NULL,
              &VarSize,
              &mSystemConfiguration
              );
    ASSERT_EFI_ERROR (Status);
  }  

  //
  // Update HOB variable for PCI resource information
  // Get the HOB list.  If it is not present, then ASSERT.
  //
  GuidHob.Raw = GetHobList ();
  if (GuidHob.Raw != NULL) {
    if ((GuidHob.Raw = GetNextGuidHob (&gEfiPlatformInfoGuid, GuidHob.Raw)) != NULL) {
      mPlatformInfo = GET_GUID_HOB_DATA (GuidHob.Guid);
    }
  }


  PrepareSetupInformation();
  UpdateAdditionalInformation ();
  UpdatePlatformInformation();
  UpdateCPUInformation();
  IdeDataFilter();
  mSetupInfoDone = TRUE;

  return;
}


#define EFI_SECURE_BOOT_MODE_NAME                   L"SecureBoot"

VOID
CheckSystemConfigLoad(SYSTEM_CONFIGURATION *SystemConfigPtr)
{
  EFI_STATUS              Status;
  UINT8                   SecureBoot;
  UINTN                   DataSize;


  DataSize = sizeof(SecureBoot);
  Status = gRT->GetVariable (
                  EFI_SECURE_BOOT_MODE_NAME,
                  &gEfiGlobalVariableGuid,
                  NULL,
                  &DataSize,
                  &SecureBoot
                  );

  if (EFI_ERROR(Status)) {
    SystemConfigPtr->SecureBoot = 0;
  } else {
    SystemConfigPtr->SecureBoot = SecureBoot;
  }
}

VOID
CheckSystemConfigSave(SYSTEM_CONFIGURATION *SystemConfigPtr)
{
}

VOID
ConfirmSecureBootTest()
{

}

