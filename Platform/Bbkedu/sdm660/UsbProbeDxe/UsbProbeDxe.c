/** @file
  诊断/修复驱动 (v7):
  1. 装 USB Type-C protocol 桩 (所有方法返回 EFI_SUCCESS) - 防止 vendor 空指针/失败退出;
  2. 心跳 (>>> 前缀), 定时打印 STAT 诊断块:
       STAT TypeC/USB_CONFIG/USBFN_IO 是否存在;
       STAT volumes(fs) = N;
       STAT StartController = 返回码;
       STAT payload(\EFI\hakios\payload.bin) = 打开结果/大小/前16字节;
  3. 心跳第 5 拍起尝试 USB_CONFIG->StartController(core=0, connType=8/1) 强制 device 模式.
  所有信息自动打印到屏幕 (本设备没有可用键盘, 不能依赖 Shell 输入).
**/
#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>

#define HEARTBEAT_MAX 120

STATIC EFI_EVENT mHeartbeatEvent;
STATIC UINTN     mBeatCount;
STATIC BOOLEAN   mStartTried;
STATIC EFI_STATUS mStartStatus = EFI_NOT_STARTED;

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

/* 自动尝试打开每个卷上的 \EFI\hakios\payload.bin, 打印结果/大小/前 16 字节 */
STATIC
VOID
CheckPayload (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles = NULL;
  UINTN       Count = 0;
  UINTN       i;
  UINTN       j;
  UINT8       Head[16];
  UINTN       HeadSize;

  Status = gBS->LocateHandleBuffer (ByProtocol, &mSfsGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status) || (Handles == NULL)) {
    AsciiPrint (">>> PAYLOAD: no volumes (%r)\n", Status);
    return;
  }

  for (i = 0; (i < Count) && (i < 4); i++) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Sfs = NULL;
    EFI_FILE_PROTOCOL                *Root = NULL;
    EFI_FILE_PROTOCOL                *File = NULL;
    EFI_FILE_INFO                    *Info = NULL;
    UINTN                            InfoSize = 0;
    UINT64                           FileSize = 0;
    EFI_STATUS                       OpenStatus;

    Status = gBS->HandleProtocol (Handles[i], &mSfsGuid, (VOID **)&Sfs);
    if (EFI_ERROR (Status) || (Sfs == NULL)) {
      AsciiPrint (">>> PAYLOAD vol%u: no SFS (%r)\n", i, Status);
      continue;
    }
    Status = Sfs->OpenVolume (Sfs, &Root);
    if (EFI_ERROR (Status) || (Root == NULL)) {
      AsciiPrint (">>> PAYLOAD vol%u: OpenVolume failed (%r)\n", i, Status);
      continue;
    }

    OpenStatus = Root->Open (Root, &File, L"\\EFI\\hakios\\payload.bin", EFI_FILE_MODE_READ, 0);
    AsciiPrint (">>> PAYLOAD vol%u: open \\EFI\\hakios\\payload.bin = %r\n", i, OpenStatus);
    if (!EFI_ERROR (OpenStatus) && (File != NULL)) {
      Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
      if (Status == EFI_BUFFER_TOO_SMALL) {
        Info = AllocateZeroPool (InfoSize);
        if (Info != NULL) {
          Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
          if (!EFI_ERROR (Status)) {
            FileSize = Info->FileSize;
          }
          FreePool (Info);
        }
      }
      AsciiPrint (">>> PAYLOAD vol%u: size=%lu (%r)\n", i, FileSize, Status);

      HeadSize = sizeof (Head);
      ZeroMem (Head, sizeof (Head));
      Status = File->Read (File, &HeadSize, Head);
      if (!EFI_ERROR (Status) && (HeadSize > 0)) {
        AsciiPrint (">>> PAYLOAD vol%u: first %u bytes:", i, (UINT32)HeadSize);
        for (j = 0; j < HeadSize; j++) {
          AsciiPrint (" %02x", Head[j]);
        }
        AsciiPrint ("\n");
      } else {
        AsciiPrint (">>> PAYLOAD vol%u: read head failed (%r)\n", i, Status);
      }
      File->Close (File);
    }
    Root->Close (Root);
  }
  FreePool (Handles);
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
    mStartStatus = Status;
    return;
  }
  Func = ((UINTN *)Ifc)[10]; /* vtable offset 0x50 = UsbConfigStartController */
  Say ("StartController->vtable[10]", EFI_SUCCESS, (UINT64)Func);
  if (Func == 0) {
    mStartStatus = EFI_UNSUPPORTED;
    return;
  }
  Status = ((EFI_STATUS (EFIAPI *)(VOID *, UINT32, UINT32))Func)(Ifc, 0, ConnType);
  Say ("StartController(0,connType)", Status, ConnType);
  mStartStatus = Status;
}

STATIC
VOID
PrintStat (
  VOID
  )
{
  EFI_STATUS  Status;
  VOID        *TypeC = NULL;
  VOID        *UsbCfg = NULL;
  VOID        *UsbFn = NULL;
  UINTN       VolCount = 0;
  EFI_HANDLE  *Handles = NULL;

  Status = gBS->LocateProtocol (&mTypeCGuid, NULL, &TypeC);
  Status = gBS->LocateProtocol (&mUsbConfigGuid, NULL, &UsbCfg);
  Status = gBS->LocateProtocol (&mUsbfnIoGuid, NULL, &UsbFn);
  if (!EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &mSfsGuid, NULL, &VolCount, &Handles))) {
    if (Handles != NULL) {
      FreePool (Handles);
    }
  } else {
    VolCount = 0;
  }

  AsciiPrint (">>> ================ STAT ================\n");
  AsciiPrint (">>> STAT TypeC=%a USB_CONFIG=%a USBFN_IO=%a\n",
              (TypeC != NULL) ? "OK" : "NO",
              (UsbCfg != NULL) ? "OK" : "NO",
              (UsbFn != NULL) ? "OK" : "NO");
  AsciiPrint (">>> STAT volumes(fs)=%u\n", VolCount);
  AsciiPrint (">>> STAT StartController=%r\n", mStartStatus);
  AsciiPrint (">>> ======================================\n");
  (VOID)Status;
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
  Say ("TypeC", (mStubProtocol != NULL) ? EFI_SUCCESS : EFI_NOT_FOUND, (UINT64)(UINTN)mStubProtocol);
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

  if ((mBeatCount >= 5) && (mBeatCount <= 8) && !mStartTried) {
    TryStartController ((mBeatCount <= 6) ? 8 : 1);
    if (mBeatCount == 8) {
      mStartTried = TRUE;
    }
  }

  if ((mBeatCount == 9) || (mBeatCount == 15) || (mBeatCount == 30) || (mBeatCount == 60)) {
    PrintStat ();
    CheckPayload ();
  }

  if (mBeatCount >= HEARTBEAT_MAX) {
    gBS->CloseEvent (Event);
  }
  (VOID)Context;
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
  AsciiPrint (">>> UsbProbe build v7 (STAT page + payload check + force StartController)\n");

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
