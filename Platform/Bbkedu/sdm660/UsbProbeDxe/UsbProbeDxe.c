/** @file
  h7000 USB 诊断/修复驱动 (v9) —— 屏幕存活信道版
  * 心跳里直接往 GOP framebuffer 画色块(不依赖 Console, 拍一张照片即可读出状态):
      红块 = probe 在跑 (执行到心跳)
      绿块 = Type-C 桩已装上
      蓝块 = StartController 返回成功
      白格 = 心跳计数 (最多 10 格)
  * Console 可用时同时打印 STAT 页 / volumes / payload / StartController 返回码;
  * 强制 Type-C 桩(方法都返回 EFI_SUCCESS);
  * 心跳第 5~8 拍尝试 USB_CONFIG->StartController(core=0, connType=8/1).
  本设备无键盘, 所有信息必须自动上屏.
**/
#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/DebugLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/SimpleTextOut.h>
#include <Guid/FileInfo.h>

#define HEARTBEAT_MAX 240
#define BAR_W         120
#define BAR_H         10
#define BAR_GAP       14

STATIC EFI_EVENT        mHeartbeatEvent;
STATIC UINTN            mBeatCount;
STATIC BOOLEAN          mStartTried;
STATIC EFI_STATUS       mStartStatus = EFI_NOT_STARTED;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL  *mGop = NULL;
STATIC BOOLEAN          mStubInstalled = FALSE;
STATIC VOID             *mStubProtocol = NULL;
STATIC CHAR8            mLine[320];

STATIC EFI_GUID mTypeCGuid = { 0x6adb9972, 0x083d, 0x4be4,
                               { 0xbb, 0x74, 0xc7, 0x25, 0xa7, 0x30, 0x61, 0x56 } };
STATIC EFI_GUID mUsbConfigGuid = { 0xe722b03f, 0xb250, 0x42ce,
                                   { 0x8e, 0xbd, 0x5b, 0xd5, 0x18, 0x12, 0xd0, 0x37 } };
STATIC EFI_GUID mUsbfnIoGuid = { 0x32d2963a, 0xfe5d, 0x4f30,
                                 { 0xb6, 0x33, 0x6e, 0x5d, 0xc5, 0x58, 0x03, 0xcc } };
STATIC EFI_GUID mSfsGuid = { 0x964e5b22, 0x6459, 0x11d2,
                             { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };

STATIC
EFI_STATUS
EFIAPI
StubOk (
  VOID
  )
{
  return EFI_SUCCESS;
}

/* ---------------- 屏幕信道: GOP 画色块 ---------------- */
STATIC
VOID
DrawBar (
  IN UINTN  X,
  IN UINTN  Y,
  IN UINTN  Width,
  IN UINTN  Height,
  IN UINT8  R,
  IN UINT8  G,
  IN UINT8  B
  )
{
  EFI_STATUS                      Status;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL   *Buf;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL   Pixel;
  UINTN                           i;

  if (mGop == NULL) {
    return;
  }
  if ((X + Width > mGop->Mode->Info->HorizontalResolution) ||
      (Y + Height > mGop->Mode->Info->VerticalResolution)) {
    return;
  }
  Buf = AllocateZeroPool (Width * Height * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
  if (Buf == NULL) {
    return;
  }
  Pixel.Blue  = B;
  Pixel.Green = G;
  Pixel.Red   = R;
  Pixel.Reserved = 0;
  for (i = 0; i < Width * Height; i++) {
    Buf[i] = Pixel;
  }
  Status = mGop->Blt (mGop, Buf, EfiBltBufferToVideo,
                      0, 0, (UINTN)X, (UINTN)Y, Width, Height, 0);
  FreePool (Buf);
  (VOID)Status;
}

/* 顶部状态条: 红=活着 绿=TypeC桩 蓝=StartController成功 白格=心跳 */
STATIC
VOID
DrawStatus (
  VOID
  )
{
  UINTN  i;

  if (mGop == NULL) {
    return;
  }
  DrawBar (8, 8, BAR_W, BAR_H, 0xFF, 0x00, 0x00);                 /* 红: probe 活着 */
  if (mStubInstalled) {
    DrawBar (8 + BAR_W + BAR_GAP, 8, BAR_W, BAR_H, 0x00, 0xFF, 0x00);   /* 绿 */
  }
  if (!EFI_ERROR (mStartStatus)) {
    DrawBar (8 + 2 * (BAR_W + BAR_GAP), 8, BAR_W, BAR_H, 0x00, 0x00, 0xFF); /* 蓝 */
  }
  for (i = 0; (i < mBeatCount) && (i < 10); i++) {
    DrawBar (8 + 3 * (BAR_W + BAR_GAP) + i * 12, 8, 8, BAR_H, 0xFF, 0xFF, 0xFF);
  }
}

/* ---------------- Console 输出 (仅在确实可用时) ---------------- */
STATIC
VOID
OutLine (
  IN CONST CHAR8  *Ascii
  )
{
  CHAR16  Uni[512];

  AsciiStrToUnicodeStrS (Ascii, Uni, ARRAY_SIZE (Uni));
  if ((gST != NULL) && (gST->ConOut != NULL) && (gST->ConOut->Mode != NULL)) {
    gST->ConOut->OutputString (gST->ConOut, Uni);
  }
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

/* ---------------- 桩 ---------------- */
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
    mStubInstalled = TRUE;
  } else {
    FreePool (Buf);
  }
  Say ("TypeC STUB install", Status, (UINT64)(UINTN)Handle);
}

/* ---------------- 状态查询 ---------------- */
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
  UINT8       Head[16];
  UINTN       HeadSize;
  EFI_FILE_INFO  *Info;
  UINTN       InfoSize;
  UINT64      FileSize;

  Status = gBS->LocateHandleBuffer (ByProtocol, &mSfsGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status) || (Handles == NULL)) {
    Say ("payload: no volumes", Status, 0);
    return;
  }
  for (i = 0; (i < Count) && (i < 4); i++) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Sfs = NULL;
    EFI_FILE_PROTOCOL                *Root = NULL;
    EFI_FILE_PROTOCOL                *File = NULL;
    EFI_STATUS                        OpenStatus;

    Status = gBS->HandleProtocol (Handles[i], &mSfsGuid, (VOID **)&Sfs);
    if (EFI_ERROR (Status) || (Sfs == NULL)) {
      continue;
    }
    Status = Sfs->OpenVolume (Sfs, &Root);
    if (EFI_ERROR (Status) || (Root == NULL)) {
      continue;
    }
    OpenStatus = Root->Open (Root, &File, L"\\EFI\\hakios\\payload.bin", EFI_FILE_MODE_READ, 0);
    AsciiSPrint (mLine, sizeof (mLine), ">>> PAYLOAD vol%u open=%r\n", (UINT32)i, OpenStatus);
    OutLine (mLine);
    if (!EFI_ERROR (OpenStatus) && (File != NULL)) {
      Info = NULL;
      InfoSize = 0;
      FileSize = 0;
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
      AsciiSPrint (mLine, sizeof (mLine), ">>> PAYLOAD vol%u size=%lu (%r)\n", (UINT32)i, FileSize, Status);
      OutLine (mLine);
      HeadSize = sizeof (Head);
      ZeroMem (Head, sizeof (Head));
      Status = File->Read (File, &HeadSize, Head);
      if (!EFI_ERROR (Status) && (HeadSize > 0)) {
        AsciiSPrint (mLine, sizeof (mLine), ">>> PAYLOAD head(%u):", (UINT32)HeadSize);
        OutLine (mLine);
        {
          UINTN j;
          for (j = 0; j < HeadSize; j++) {
            AsciiSPrint (mLine, sizeof (mLine), ">>>   b[%u]=0x%02x\n", (UINT32)j, Head[j]);
            OutLine (mLine);
          }
        }
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

/* ---------------- 心跳 ---------------- */
STATIC
VOID
EFIAPI
HeartbeatCallback (
  IN EFI_EVENT Event,
  IN VOID      *Context
  )
{
  mBeatCount++;

  if (mGop == NULL) {
    EFI_STATUS  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&mGop);
    if (EFI_ERROR (Status)) {
      mGop = NULL;
    }
  }

  DrawStatus ();

  if (mBeatCount == 1) {
    Say ("=== PROBE v9 alive ===", EFI_SUCCESS, 0);
    PrintVolumes ();
  }

  if ((mBeatCount >= 5) && (mBeatCount <= 8) && !mStartTried) {
    TryStartController ((mBeatCount <= 6) ? 8 : 1);
    if (mBeatCount == 8) {
      mStartTried = TRUE;
    }
  }

  if ((mBeatCount == 10) || (mBeatCount == 20) || (mBeatCount == 40)) {
    Say ("STAT TypeC/USB_CONFIG/USBFN_IO", EFI_SUCCESS, 0);
    {
      VOID  *A = NULL;
      VOID  *B = NULL;
      VOID  *C = NULL;
      gBS->LocateProtocol (&mTypeCGuid, NULL, &A);
      gBS->LocateProtocol (&mUsbConfigGuid, NULL, &B);
      gBS->LocateProtocol (&mUsbfnIoGuid, NULL, &C);
      AsciiSPrint (mLine, sizeof (mLine), ">>> STAT TypeC=%a USB_CONFIG=%a USBFN_IO=%a\n",
                   (A != NULL) ? "OK" : "NO", (B != NULL) ? "OK" : "NO", (C != NULL) ? "OK" : "NO");
      OutLine (mLine);
    }
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

  OutLine (">>> PROBE: entry reached (UsbProbeDxe v9)\n");

  if (mGop == NULL) {
    gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&mGop);
  }
  DrawBar (8, 8, BAR_W, BAR_H, 0xFF, 0x00, 0x00);   /* 立即画红块: 证明 entry 跑了 */

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
  if (mStubInstalled && (mGop != NULL)) {
    DrawBar (8 + BAR_W + BAR_GAP, 8, BAR_W, BAR_H, 0x00, 0xFF, 0x00);
  }

  if (!EFI_ERROR (gBS->CreateEvent (EVT_TIMER, TPL_CALLBACK, HeartbeatCallback, NULL, &mHeartbeatEvent))) {
    gBS->SetTimer (mHeartbeatEvent, TimerPeriodic, 5000000ULL); /* 0.5s */
  }

  return EFI_SUCCESS;
}
