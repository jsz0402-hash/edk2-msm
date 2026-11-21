/** @file
  诊断/修复驱动 (v4):
  1. 打印关键 protocol 状态 (ConOut + DebugLib 双通道);
  2. 若 USB Type-C protocol 缺失, 装 stub;
  3. 周期心跳 (1s): 打印计数 + USB_CONFIG / USBFN_IO 是否存在.
     若心跳不再打印 => 系统卡死在 DXE 调度阶段(不在 BDS 空闲).
**/
#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>

#define HEARTBEAT_MAX 40

STATIC EFI_EVENT mHeartbeatEvent;
STATIC UINTN     mBeatCount;

/* USB Type-C protocol: 6adb9972-083d-4be4-bb74-c725a7306156 */
STATIC EFI_GUID mTypeCGuid = { 0x6adb9972, 0x083d, 0x4be4,
                               { 0xbb, 0x74, 0xc7, 0x25, 0xa7, 0x30, 0x61, 0x56 } };
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
STATIC EFI_GUID mUsbConfigGuid = { 0xe722b03f, 0xb250, 0x42ce,
                                   { 0x8e, 0xbd, 0x5b, 0xd5, 0x18, 0x12, 0xd0, 0x37 } };
STATIC EFI_GUID mUsbfnIoGuid = { 0x32d2963a, 0xfe5d, 0x4f30,
                                 { 0xb6, 0x33, 0x6e, 0x5d, 0xc5, 0x58, 0x03, 0xcc } };

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
  IN EFI_STATUS   Status,
  IN UINT64       Value
  )
{
  AsciiPrint ("UsbProbe: %a status=%r val=0x%lx\n", Text, Status, Value);
  DEBUG ((DEBUG_ERROR, "UsbProbe: %a status=%r val=0x%lx\n", Text, Status, Value));
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
  Say (Name, Status, (UINT64)(UINTN)Ifc);
}

STATIC
VOID
PrintAll (
  IN CONST CHAR8 *Tag
  )
{
  Say ("---- probe ----", EFI_SUCCESS, 0);
  PrintOne ("TypeC", &mTypeCGuid);
  PrintOne ("ChipInfo", &mChipInfoGuid);
  PrintOne ("Clock", &mClockGuid);
  PrintOne ("PmicVreg", &mPmicVregGuid);
  PrintOne ("Smem", &mSmemGuid);
  PrintOne ("PlatformInfo", &mPlatformInfoGuid);
  PrintOne ("AcpiSdt", &mAcpiSdtGuid);
  PrintOne ("USB_CONFIG", &mUsbConfigGuid);
  PrintOne ("USBFN_IO", &mUsbfnIoGuid);
  Say ("---- end ----", EFI_SUCCESS, 0);
}

STATIC
VOID
EFIAPI
HeartbeatCallback (
  IN EFI_EVENT Event,
  IN VOID      *Context
  )
{
  VOID  *Ifc;

  mBeatCount++;
  Ifc = NULL;
  Say ("HB", EFI_SUCCESS, mBeatCount);
  PrintOne ("  USB_CONFIG", &mUsbConfigGuid);
  PrintOne ("  USBFN_IO", &mUsbfnIoGuid);
  PrintOne ("  AcpiSdt", &mAcpiSdtGuid);
  if (mBeatCount >= HEARTBEAT_MAX) {
    gBS->CloseEvent (Event);
  }
}

STATIC
EFI_STATUS
InstallStub (
  IN EFI_GUID    *Guid,
  IN CONST CHAR8 *Name
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
  Status = gBS->InstallProtocolInterface (&Handle, Guid, EFI_NATIVE_INTERFACE, Buf);
  if (!EFI_ERROR (Status)) {
    mStubProtocol = Buf;
    Say (Name, Status, (UINT64)(UINTN)Handle);
  } else {
    Say (Name, Status, 0);
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

  Say ("=== UsbProbeDxe entry ===", EFI_SUCCESS, 0);

  Status = gBS->LocateProtocol (&mTypeCGuid, NULL, &Ifc);
  if (EFI_ERROR (Status)) {
    Say ("TypeC NOT found -> install stub", Status, 0);
    InstallStub (&mTypeCGuid, "STUB installed");
    Ifc = NULL;
    Status = gBS->LocateProtocol (&mTypeCGuid, NULL, &Ifc);
    Say ("after stub: TypeC", Status, (UINT64)(UINTN)Ifc);
  } else {
    Say ("TypeC found", Status, (UINT64)(UINTN)Ifc);
  }

  PrintAll ("early");

  /* 周期心跳: 用来判断 DXE 调度是否卡死 */
  if (!EFI_ERROR (gBS->CreateEvent (EVT_TIMER, TPL_CALLBACK, HeartbeatCallback, NULL, &mHeartbeatEvent))) {
    gBS->SetTimer (mHeartbeatEvent, TimerPeriodic, 10000000ULL); /* 1s */
  }

  return EFI_SUCCESS;
}
