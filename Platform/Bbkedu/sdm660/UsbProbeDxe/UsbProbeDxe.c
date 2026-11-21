/** @file
  h7000 USB 诊断/修复驱动 (v8) - 输出保证可见版:
  * 所有提示都通过 "找到的所有 SimpleTextOutput 协议 + gST->ConOut" 直接写屏,
    同时再用 DEBUG/AsciiPrint 兜底 -> RELEASE 构建也能看到;
  * 第一行无条件打印 ">>> PROBE: entry reached" (用来判断驱动是否被 dispatch);
  * 装 USB Type-C protocol 桩 (所有方法返回 EFI_SUCCESS);
  * 心跳 STAT 页: TypeC / USB_CONFIG / USBFN_IO / volumes(fs) / StartController 返回码;
  * 自动尝试打开每个卷上的 \EFI\hakios\payload.bin 并打印 大小 + 前 16 字节;
  * 心跳第 5~8 拍强制调 USB_CONFIG->StartController(core=0, connType=8/1).
  (本设备无可用键盘, 一切信息必须自动上屏.)
**/
#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/DebugLib.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/SimpleTextOut.h>
#include <Guid/FileInfo.h>

#define HEARTBEAT_MAX 120

STATIC EFI_EVENT  mHeartbeatEvent;
STATIC UINTN      mBeatCount;
STATIC BOOLEAN    mStartTried;
STATIC EFI_STATUS mStartStatus = EFI_NOT_STARTED;
STATIC CHAR8      mLine[320];

STATIC EFI_GUID mTypeCGuid = { 0x6adb9972, 0x083d, 0x4be4,
                               { 0xbb, 0x74, 0xc7, 0x25, 0xa7, 0x30, 0x61, 0x56 } };
STATIC EFI_GUID mUsbConfigGuid = { 0xe722b03f, 0xb250, 0x42ce,
                                   { 0x8e, 0xbd, 0x5b, 0xd5, 0x18, 0x12, 0xd0, 0x37 } };
STATIC EFI_GUID mUsbfnIoGuid = { 0x32d2963a, 0xfe5d, 0x4f30,
                                 { 0xb6, 0x33, 0x6e, 0x5d, 0xc5, 0x58, 0x03, 0xcc } };
STATIC EFI_GUID mSfsGuid = { 0x964e5b22, 0x6459, 0x11d2,
                             { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
STATIC EFI_GUID mTextOutGuid = { 0x387477c2, 0x69c7, 0x11d2,
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

/* 把一行 ASCII 文本写到所有能找到的 SimpleTextOutput (外加 ConOut 兜底) */
STATIC
VOID
OutLine (
  IN CONST CHAR8  *Ascii
  )
{
  CHAR16      Uni[CFG_MAXIMUM_ASCII_STRING_LENGTH];
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles = NULL;
  UINTN       Count = 0;
  UINTN       i;

  AsciiStrToUnicodeStrS (Ascii, Uni, ARRAY_SIZE (Uni));

  Status = gBS->LocateHandleBuffer (ByProtocol, &mTextOutGuid, NULL, &Count, &Handles);
  if (!EFI_ERROR (Status) && (Handles != NULL)) {
    for (i = 0; i < Count; i++) {
      EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *TextOut = NULL;
      if (!EFI_ERROR (gBS->HandleProtocol (Handles[i], &mTextOutGuid, (VOID **)&TextOut)) && (TextOut != NULL)) {
        TextOut->OutputString (TextOut, Uni);
      }
    }
    FreePool (Handles);
  } else if ((gST != NULL) && (gST->ConOut != NULL)) {
    gST->ConOut->OutputString (gST->ConOut, Uni);
  }

  /* 兜底: 早期 ConOut 未就绪时 DEBUG 可能通过串口/帧缓存可见 */
  DEBUG ((DEBUG_ERROR, "%a", Ascii));
}

STATIC
VOID
Say (
  IN CONST CHAR8 *Text,
  IN EFI_STATUS   Status,
  IN UINT64       Value
  )
{
  AsciiSPrint (mLine, sizeof (mLine), ">>> PROBE: %a status=%r val=0x%lx\n", Text, Status, Value);
  OutLine (mLine);
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
    Say ("volumes(fs): none", Status, 0);
    return;
  }
  Say ("volumes(fs)", EFI_SUCCESS, Count);
  if (Handles != NULL) {
    FreePool (Handles);
  }
}

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
    AsciiSPrint (mLine, sizeof (mLine), ">>> PAYLOAD: no volumes (%r)\n", Status);
    OutLine (mLine);
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
      AsciiSPrint (mLine, sizeof (mLine), ">>> PAYLOAD vol%u: no SFS (%r)\n", i, Status);
      OutLine (mLine);
      continue;
    }
    Status = Sfs->OpenVolume (Sfs, &Root);
    if (EFI_ERROR (Status) || (Root == NULL)) {
      AsciiSPrint (mLine, sizeof (mLine), ">>> PAYLOAD vol%u: OpenVolume failed (%r)\n", i, Status);
      OutLine (mLine);
      continue;
    }

    OpenStatus = Root->Open (Root, &File, L"\\EFI\\hakios\\payload.bin", EFI_FILE_MODE_READ, 0);
    AsciiSPrint (mLine, sizeof (mLine), ">>> PAYLOAD vol%u: open payload.bin = %r\n", i, OpenStatus);
    OutLine (mLine);
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
      AsciiSPrint (mLine, sizeof (mLine), ">>> PAYLOAD vol%u: size=%lu (%r)\n", i, FileSize, Status);
      OutLine (mLine);

      HeadSize = sizeof (Head);
      ZeroMem (Head, sizeof (Head));
      Status = File->Read (File, &HeadSize, Head);
      if (!EFI_ERROR (Status) && (HeadSize > 0)) {
        AsciiSPrint (mLine, sizeof (mLine), ">>> PAYLOAD vol%u: head(%u):", i, (UINT32)HeadSize);
        OutLine (mLine);
        for (j = 0; j < HeadSize; j++) {
          AsciiSPrint (mLine, sizeof (mLine), ">>>   byte[%u] = 0x%02x\n", (UINT32)j, Head[j]);
          OutLine (mLine);
        }
      } else {
        AsciiSPrint (mLine, sizeof (mLine), ">>> PAYLOAD vol%u: read head failed (%r)\n", i, Status);
        OutLine (mLine);
      }
      File->Close (File);
    }
    Root->Close (Root);
  }
  FreePool (Handles);
}

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
  if (EFI_ERROR (Status) || (Ifc == NULL)) {
    Say ("StartController: no USB_CONFIG", Status, 0);
    mStartStatus = Status;
    return;
  }
  Func = ((UINTN *)Ifc)[10];
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
  VOID        *TypeC = NULL;
  VOID        *UsbCfg = NULL;
  VOID        *UsbFn = NULL;
  UINTN       VolCount = 0;
  EFI_HANDLE  *Handles = NULL;

  gBS->LocateProtocol (&mTypeCGuid, NULL, &TypeC);
  gBS->LocateProtocol (&mUsbConfigGuid, NULL, &UsbCfg);
  gBS->LocateProtocol (&mUsbfnIoGuid, NULL, &UsbFn);
  if (!EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &mSfsGuid, NULL, &VolCount, &Handles))) {
    if (Handles != NULL) {
      FreePool (Handles);
    }
  } else {
    VolCount = 0;
  }

  OutLine (">>> =============== STAT ===============\n");
  AsciiSPrint (mLine, sizeof (mLine), ">>> STAT TypeC=%a USB_CONFIG=%a USBFN_IO=%a\n",
               (TypeC != NULL) ? "OK" : "NO",
               (UsbCfg != NULL) ? "OK" : "NO",
               (UsbFn != NULL) ? "OK" : "NO");
  OutLine (mLine);
  AsciiSPrint (mLine, sizeof (mLine), ">>> STAT volumes(fs)=%u\n", VolCount);
  OutLine (mLine);
  AsciiSPrint (mLine, sizeof (mLine), ">>> STAT StartController=%r\n", mStartStatus);
  OutLine (mLine);
  OutLine (">>> =====================================\n");
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
EFIAPI
HeartbeatCallback (
  IN EFI_EVENT Event,
  IN VOID      *Context
  )
{
  mBeatCount++;
  AsciiSPrint (mLine, sizeof (mLine), ">>> PROBE: HB %u\n", mBeatCount);
  OutLine (mLine);

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

  /* 第一行无条件打印: 判断驱动是否真的被 dispatch */
  OutLine (">>> PROBE: entry reached (UsbProbeDxe v8)\n");

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

  Say ("---- early probe ----", EFI_SUCCESS, 0);
  PrintVolumes ();

  if (!EFI_ERROR (gBS->CreateEvent (EVT_TIMER, TPL_CALLBACK, HeartbeatCallback, NULL, &mHeartbeatEvent))) {
    gBS->SetTimer (mHeartbeatEvent, TimerPeriodic, 10000000ULL); /* 1s */
  }

  return EFI_SUCCESS;
}
