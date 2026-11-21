/** @file
  诊断/修复驱动 (v6):
  1. 装 USB Type-C protocol 桩 (所有方法返回 EFI_SUCCESS);
  2. 心跳打印 (>>> 前缀): Type-C / USB_CONFIG / USBFN_IO / FS 卷数量;
  3. 心跳第 5 拍开始尝试调用 USB_CONFIG->StartController(core=0, connType=8/1)
     强制启动 device 模式 (mode 1 = device), 打印返回状态.
**/
#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>

#define HEARTBEAT_MAX 90

STATIC EFI_EVENT mHeartbeatEvent;
STATIC UINTN     mBeatCount;
STATIC BOOLEAN   mStartTried;

STATIC EFI_GUID mTypeCGuid = { 0x6adb9972, 0x083d, 0x4be4,
                               { 0xbb, 0x74, 0xc7, 0x25, 0xa7, 0x30, 0x61, 0x56 } };
STATIC EFI_GUID mUsbConfigGuid = { 0xe722b03f, 0xb250, 0x42ce,
                                   { 0x8e, 0xbd, 0x5b, 0xd5, 0x18, 0x12, 0xd0, 0x37 } };
STATIC EFI_GUID mUsbfnIoGuid = { 0x32d2963a, 0xfe5d, 0x4f30,
                                 { 0xb6, 0x33, 0x6e, 0x5d, 0xc5, 0x58, 0x03, 0xcc } };
STATIC EFI_GUID mSfsGuid = { 0x964e5b22, 0x6459, 0x11d2,
                             { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };

STATIC VOID *mStubProtocol = NULL;

STATIC
EFI_STATUS
EFIAPI
StubOk (
  VOID
  )
{
  return EFI_SUCCESS;
}

STATIC
VOID
Say (
  IN CONST CHAR8 *Text,
  IN EFI_STATUS   Status,
  IN UINT64       Value
  )
{
  AsciiPrint (">>> UsbProbe: %a status=%r val=0x%lx\n", Text, Status, Value);
  DEBUG ((DEBUG_ERROR, ">>> UsbProbe: %a status=%r val=0x%lx\n", Text, Status, Value));
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
  return;
}

STATIC
VOID
PrintVolumes (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles = NULL;
  UINTN       Count = 0;

  Status = gBS->LocateHandleBuffer (ByProtocol, &mSfsGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    Say ("volumes(fs)", Status, 0);
    return;
  }
  Say ("volumes(fs)", EFI_SUCCESS, Count);
  if (Handles != NULL) {
    FreePool (Handles);
  }
}

/* 尝试用 USB_CONFIG 协议的 StartController 强制启动 device 模式 */
STATIC
VOID
TryStartController (
  IN UINT32 ConnType
  )
{
  EFI_STATUS  Status;
  VOID        *Ifc = NULL;
  UINTN       Func;

  Status = gBS->LocateProtocol (&mUsbConfigGuid, NULL, &Ifc);
  if (EFI_ERROR (Status) || Ifc == NULL) {
    Say ("StartController: no USB_CONFIG", Status, 0);
    return;
  }
  Func = ((UINTN *)Ifc)[10]; /* vtable offset 0x50 = StartController */
  Say ("StartController->vtable[10]", EFI_SUCCESS, (UINT64)Func);
  if (Func == 0) {
    return;
  }
  Status = ((EFI_STATUS (EFIAPI *)(VOID *, UINT32, UINT32))Func)(Ifc, 0, ConnType);
  Say ("StartController(0,connType)", Status, ConnType);
}

STATIC
VOID
InstallStub (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle = NULL;
  UINT8       *Buf;
  UINTN       i;

  if (mStubProtocol != NULL) {
    return;
  }
  Buf = AllocateZeroPool (0x100);
  if (Buf == NULL) {
    return;
  }
  for (i = 0; i < 0x100 / sizeof (UINTN); i++) {
    ((UINTN *)Buf)[i] = (UINTN)(UINTN)&StubOk;
  }
  Status = gBS->InstallProtocolInterface (&Handle, &mTypeCGuid, EFI_NATIVE_INTERFACE, Buf);
  if (!EFI_ERROR (Status)) {
    mStubProtocol = Buf;
    Say ("TypeC STUB installed", Status, (UINT64)(UINTN)Handle);
  } else {
    Say ("TypeC STUB install FAILED", Status, 0);
    FreePool (Buf);
  }
}

STATIC
VOID
PrintAll (
  IN CONST CHAR8 *Tag
  )
{
  Say ("---- probe ----", EFI_SUCCESS, 0);
  PrintOne ("TypeC", &mTypeCGuid);
  PrintOne ("USB_CONFIG", &mUsbConfigGuid);
  PrintOne ("USBFN_IO", &mUsbfnIoGuid);
  PrintVolumes ();
  Say ("---- end ----", EFI_SUCCESS, 0);
  (VOID)Tag;
}

STATIC
VOID
EFIAPI
HeartbeatCallback (
  IN EFI_EVENT Event,
  IN VOID      *Context
  )
{
  mBeatCount++;
  AsciiPrint (">>> UsbProbe: HB %u\n", mBeatCount);
  DEBUG ((DEBUG_ERROR, ">>> UsbProbe: HB %u\n", mBeatCount));

  PrintOne ("TypeC", &mTypeCGuid);
  PrintVolumes ();

  if ((mBeatCount % 3) == 0) {
    PrintOne ("USB_CONFIG", &mUsbConfigGuid);
    PrintOne ("USBFN_IO", &mUsbfnIoGuid);
  }

  if ((mBeatCount >= 5) && (mBeatCount <= 8) && !mStartTried) {
    if (mBeatCount == 5 || mBeatCount == 6) {
      TryStartController (8); /* 8 -> device mode(1) */
    } else {
      TryStartController (1); /* 1 -> mode 1 */
    }
    if (mBeatCount == 8) {
      mStartTried = TRUE;
    }
  }

  if (mBeatCount >= HEARTBEAT_MAX) {
    gBS->CloseEvent (Event);
  }
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
  AsciiPrint (">>> UsbProbe build v6 (stub=SUCCESS, heartbeat, force StartController)\n");

  Status = gBS->LocateProtocol (&mTypeCGuid, NULL, &Ifc);
  if (EFI_ERROR (Status)) {
    Say ("TypeC NOT found -> install stub", Status, 0);
    InstallStub ();
    Ifc = NULL;
    Status = gBS->LocateProtocol (&mTypeCGuid, NULL, &Ifc);
    Say ("after stub: TypeC", Status, (UINT64)(UINTN)Ifc);
  } else {
    Say ("TypeC found", Status, (UINT64)(UINTN)Ifc);
  }

  PrintAll ("early");

  if (!EFI_ERROR (gBS->CreateEvent (EVT_TIMER, TPL_CALLBACK, HeartbeatCallback, NULL, &mHeartbeatEvent))) {
    gBS->SetTimer (mHeartbeatEvent, TimerPeriodic, 10000000ULL); /* 1s */
  }

  return EFI_SUCCESS;
}
