/** @file
  诊断/修复驱动:
  1. 打印关键 protocol 是否存在 (同时走 ConOut 和 DebugLib 两条输出路径, 便于崩溃后仍能在屏上看到);
  2. 若 PMIC USB protocol (6adb9972-083d-4be4-bb74-c725a7306156) 缺失, 安装 stub,
     让 UsbConfigDxe 不再空指针解引用崩溃.
**/
#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>

STATIC EFI_EVENT mTimerEvent;

/* PMIC USB protocol: 6adb9972-083d-4be4-bb74-c725a7306156
 * (从 UsbConfigDxe.efi .data:0xB140 的原始字节 72 99 db 6a 3d 08 e4 4b bb 74 c7 25 a7 30 61 56 解出) */
STATIC EFI_GUID mPmicUsbGuid = { 0x6adb9972, 0x083d, 0x4be4,
                                 { 0xbb, 0x74, 0xc7, 0x25, 0xa7, 0x30, 0x61, 0x56 } };
/* UsbConfigDxe depex 要求之一 */
STATIC EFI_GUID mChipInfoGuid = { 0xb0760469, 0x970c, 0x487a,
                                  { 0xa4, 0xb5, 0x28, 0xdb, 0x7b, 0x45, 0xce, 0xf1 } };
STATIC EFI_GUID mClockGuid = { 0x241afae6, 0x885f, 0x4f6c,
                               { 0xa7, 0xea, 0xc2, 0x8e, 0xab, 0x79, 0xc3, 0xe5 } };
STATIC EFI_GUID mPmicVregGuid = { 0x22d38d3d, 0xe8b6, 0x4f8f,
                                  { 0x9c, 0x26, 0xbc, 0xeb, 0x07, 0xd6, 0xcb, 0x68 } };
STATIC EFI_GUID mSmemGuid = { 0xf4e5c7d0, 0xd239, 0x47cb,
                              { 0xaa, 0xcd, 0x7f, 0x66, 0xef, 0x76, 0x32, 0x38 } };
STATIC EFI_GUID mPlatformInfoGuid = { 0x157a5c45, 0x21b2, 0x43c5,
                                      { 0xba, 0x7c, 0x82, 0x2f, 0xee, 0x5f, 0xe5, 0x99 } };
STATIC EFI_GUID mAcpiSdtGuid = { 0xeb97088e, 0xcfdf, 0x49c6,
                                 { 0xbe, 0x4b, 0xd9, 0x06, 0xa5, 0xb2, 0x0e, 0x86 } };

STATIC VOID *mStubProtocol = NULL;

STATIC
EFI_STATUS
EFIAPI
StubFunction (
  VOID
  )
{
  return EFI_UNSUPPORTED;
}

STATIC
VOID
Say (
  IN CONST CHAR8 *Text,
  IN CONST CHAR8 *Detail,
  IN EFI_STATUS   Status,
  IN UINT64       Value
  )
{
  AsciiPrint ("UsbProbe: %a %a status=%r val=0x%lx\n", Text, Detail, Status, Value);
  DEBUG ((DEBUG_ERROR, "UsbProbe: %a %a status=%r val=0x%lx\n", Text, Detail, Status, Value));
}

STATIC
VOID
PrintOne (
  IN CONST CHAR8 *Name,
  IN EFI_GUID    *Guid
  )
{
  VOID       *Ifc = NULL;
  EFI_STATUS  Status;

  Status = gBS->LocateProtocol (Guid, NULL, &Ifc);
  Say (Name, "", Status, (UINT64)(UINTN)Ifc);
}

STATIC
VOID
PrintAll (
  IN CONST CHAR8 *Tag
  )
{
  Say ("---- probe ----", Tag, EFI_SUCCESS, 0);
  PrintOne ("PmicUsb", &mPmicUsbGuid);
  PrintOne ("ChipInfo", &mChipInfoGuid);
  PrintOne ("Clock", &mClockGuid);
  PrintOne ("PmicVreg", &mPmicVregGuid);
  PrintOne ("Smem", &mSmemGuid);
  PrintOne ("PlatformInfo", &mPlatformInfoGuid);
  PrintOne ("AcpiSdt", &mAcpiSdtGuid);
  Say ("---- end ----", Tag, EFI_SUCCESS, 0);
}

STATIC
VOID
EFIAPI
ProbeTimerCallback (
  IN EFI_EVENT Event,
  IN VOID      *Context
  )
{
  PrintAll ("late");
  gBS->CloseEvent (Event);
  mTimerEvent = NULL;
}

STATIC
EFI_STATUS
InstallStubPmicUsb (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle = NULL;
  UINT8       *Buf;
  UINTN       i;

  if (mStubProtocol != NULL) {
    return EFI_SUCCESS;
  }

  Buf = AllocateZeroPool (0x100);
  if (Buf == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  for (i = 0; i < 0x100 / sizeof (UINTN); i++) {
    ((UINTN *)Buf)[i] = (UINTN)(UINTN)&StubFunction;
  }
  Status = gBS->InstallProtocolInterface (
                  &Handle,
                  &mPmicUsbGuid,
                  EFI_NATIVE_INTERFACE,
                  Buf
                  );
  if (!EFI_ERROR (Status)) {
    mStubProtocol = Buf;
    Say ("STUB installed", "", Status, (UINT64)(UINTN)Handle);
  } else {
    Say ("STUB install FAILED", "", Status, 0);
    FreePool (Buf);
  }
  return Status;
}

EFI_STATUS
EFIAPI
UsbProbeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  VOID        *Ifc = NULL;

  Say ("=== UsbProbeDxe entry ===", "", EFI_SUCCESS, 0);

  Status = gBS->LocateProtocol (&mPmicUsbGuid, NULL, &Ifc);
  if (EFI_ERROR (Status)) {
    Say ("PMIC USB NOT found -> install stub", "", Status, 0);
    InstallStubPmicUsb ();
    /* 再验证一次桩是否装成功 */
    Ifc = NULL;
    Status = gBS->LocateProtocol (&mPmicUsbGuid, NULL, &Ifc);
    Say ("after stub: PMIC USB", "", Status, (UINT64)(UINTN)Ifc);
  } else {
    Say ("PMIC USB found", "", Status, (UINT64)(UINTN)Ifc);
  }

  PrintAll ("early");

  Status = gBS->CreateEvent (
                  EVT_TIMER,
                  TPL_APPLICATION,
                  ProbeTimerCallback,
                  NULL,
                  &mTimerEvent
                  );
  if (!EFI_ERROR (Status)) {
    gBS->SetTimer (mTimerEvent, TimerRelative, 30000000ULL); /* 3s */
  }

  /* 停 1 秒让屏幕上的输出可见 (崩溃发生在更晚的 AcpiTableDxe 之后) */
  gBS->Stall (1000000);

  return EFI_SUCCESS;
}
