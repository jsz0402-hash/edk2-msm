/** @file
  诊断驱动: 打印关键 protocol 是否存在; 若 PMIC USB protocol 缺失则装一个 stub,
  避免 UsbConfigDxe 因 LocateProtocol 失败后空指针解引用而崩溃。
**/
#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>

STATIC EFI_EVENT mTimerEvent;

/* 6adb9972-3d08-4be4-bb74-c725a7306156  PMIC USB protocol (UsbConfigDxe 空指针崩溃源的候选) */
STATIC EFI_GUID mPmicUsbGuid = { 0x6adb9972, 0x3d08, 0x4be4,
                                 { 0xbb, 0x74, 0xc7, 0x25, 0xa7, 0x30, 0x61, 0x56 } };
/* b0760469-970c-487a-a4b5-28db7b45cef1  UsbConfigDxe depex 要求之一 (疑似 ChipInfo) */
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
PrintOne (
  IN CONST CHAR8 *Name,
  IN EFI_GUID    *Guid
  )
{
  VOID       *Ifc = NULL;
  EFI_STATUS  Status;

  Status = gBS->LocateProtocol (Guid, NULL, &Ifc);
  AsciiPrint ("UsbProbe: %a status=%r ifc=0x%lx\n", Name, Status, (UINT64)(UINTN)Ifc);
}

STATIC
VOID
PrintAll (
  IN CONST CHAR8 *Tag
  )
{
  AsciiPrint ("\nUsbProbe[%a] ---- protocol probe ----\n", Tag);
  PrintOne ("PmicUsb", &mPmicUsbGuid);
  PrintOne ("ChipInfo", &mChipInfoGuid);
  PrintOne ("Clock", &mClockGuid);
  PrintOne ("PmicVreg", &mPmicVregGuid);
  PrintOne ("Smem", &mSmemGuid);
  PrintOne ("PlatformInfo", &mPlatformInfoGuid);
  PrintOne ("AcpiSdt", &mAcpiSdtGuid);
  AsciiPrint ("UsbProbe[%a] ---- end ----\n\n", Tag);
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
  /* 协议接口全部填成返回 EFI_UNSUPPORTED 的桩函数 */
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
    AsciiPrint ("UsbProbe: installed STUB PMIC USB protocol on handle 0x%lx\n", (UINT64)(UINTN)Handle);
  } else {
    AsciiPrint ("UsbProbe: install stub PMIC USB failed: %r\n", Status);
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

  AsciiPrint ("\nUsbProbe: ==== UsbProbeDxe entry ====\n");

  Status = gBS->LocateProtocol (&mPmicUsbGuid, NULL, &Ifc);
  if (EFI_ERROR (Status)) {
    AsciiPrint ("UsbProbe: PMIC USB protocol NOT found (%r) -> install stub\n", Status);
    InstallStubPmicUsb ();
  } else {
    AsciiPrint ("UsbProbe: PMIC USB protocol found at 0x%lx\n", (UINT64)(UINTN)Ifc);
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

  return EFI_SUCCESS;
}
