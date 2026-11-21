/** @file
  UsbProbeApp (v1) - UEFI Shell application version of the h7000 USB probe.

  Run from startup.nsh inside the UEFI Shell: console is guaranteed ready and
  every line is also appended to \probe_log.txt on the first writable FAT volume
  (so the log can be pulled via adb/TWRP without scrolling the screen).

  What it does:
   1. GOP status bars at top-left: RED=app alive, GREEN=TypeC present/stub,
      BLUE=StartController succeeded, WHITE cells=heartbeat count;
   2. enumerate SimpleFileSystem volumes and try \EFI\hakios\payload.bin;
   3. call USB_CONFIG->StartController(core=0, connType=8/1) to force device mode;
   4. re-check USBFN_IO a few seconds later;
   5. exit after ~10 seconds (does not block the Shell).
**/
#include <Uefi.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL  *mGop = NULL;
STATIC EFI_FILE_PROTOCOL             *mLogFile = NULL;
STATIC EFI_STATUS                     mStartStatus = EFI_NOT_STARTED;
STATIC CHAR8                          mLogVol[16] = "?";

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
ProbeStubOk (
  VOID
  )
{
  return EFI_SUCCESS;
}

/* ---------------- output: console + log file ---------------- */
STATIC
VOID
Log (
  IN CONST CHAR16  *Format,
  ...
  )
{
  CHAR16   Buf[600];
  VA_LIST  Marker;
  UINTN    Size;

  VA_START (Marker, Format);
  UnicodeVSPrint (Buf, sizeof (Buf), Format, Marker);
  VA_END (Marker);

  if ((gST != NULL) && (gST->ConOut != NULL)) {
    gST->ConOut->OutputString (gST->ConOut, Buf);
  }
  if (mLogFile != NULL) {
    Size = StrLen (Buf) * sizeof (CHAR16);
    mLogFile->Write (mLogFile, &Size, Buf);
  }
}

STATIC
VOID
OpenLogFile (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles = NULL;
  UINTN       Count = 0;
  UINTN       i;

  Status = gBS->LocateHandleBuffer (ByProtocol, &mSfsGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status) || (Handles == NULL)) {
    return;
  }
  for (i = 0; i < Count; i++) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Sfs = NULL;
    EFI_FILE_PROTOCOL                *Root = NULL;
    EFI_FILE_PROTOCOL                *File = NULL;

    Status = gBS->HandleProtocol (Handles[i], &mSfsGuid, (VOID **)&Sfs);
    if (EFI_ERROR (Status) || (Sfs == NULL)) {
      continue;
    }
    Status = Sfs->OpenVolume (Sfs, &Root);
    if (EFI_ERROR (Status) || (Root == NULL)) {
      continue;
    }
    Status = Root->Open (
                     Root,
                     &File,
                     L"\\probe_log.txt",
                     EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                     0
                     );
    if (!EFI_ERROR (Status) && (File != NULL)) {
      File->SetPosition (File, 0xFFFFFFFFFFFFFFFFULL);
      mLogFile = File;
      AsciiSPrint (mLogVol, sizeof (mLogVol), "fs%d", (UINT32)i);
      break;
    }
    if (File != NULL) {
      File->Close (File);
    }
    Root->Close (Root);
  }
  if (Handles != NULL) {
    FreePool (Handles);
  }
}

/* ---------------- GOP bars ---------------- */
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
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Buf;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL   Pix;
  UINTN                           i;

  if ((mGop == NULL) || (mGop->Mode == NULL) || (mGop->Mode->Info == NULL)) {
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
  Pix.Red = R;
  Pix.Green = G;
  Pix.Blue = B;
  Pix.Reserved = 0;
  for (i = 0; i < Width * Height; i++) {
    Buf[i] = Pix;
  }
  mGop->Blt (mGop, Buf, EfiBltBufferToVideo, 0, 0, X, Y, Width, Height, 0);
  FreePool (Buf);
}

STATIC
VOID
DrawStatus (
  IN UINTN    Beat,
  IN BOOLEAN  TypeCPresent
  )
{
  UINTN  i;

  if (mGop == NULL) {
    return;
  }
  DrawBar (8, 8, 120, 10, 0xFF, 0x00, 0x00);              /* RED: app alive */
  if (TypeCPresent) {
    DrawBar (142, 8, 120, 10, 0x00, 0xFF, 0x00);          /* GREEN: TypeC */
  }
  if (!EFI_ERROR (mStartStatus)) {
    DrawBar (276, 8, 120, 10, 0x00, 0x00, 0xFF);          /* BLUE: StartController ok */
  }
  for (i = 0; (i < Beat) && (i < 10); i++) {
    DrawBar (410 + i * 12, 8, 8, 10, 0xFF, 0xFF, 0xFF);   /* WHITE cells: heartbeat */
  }
}

/* ---------------- payload.bin check ---------------- */
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

  Status = gBS->LocateHandleBuffer (ByProtocol, &mSfsGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status) || (Handles == NULL)) {
    Log (L">>> PAYLOAD: no volumes (%r)\r\n", Status);
    return;
  }
  Log (L">>> PAYLOAD: volumes=%u\r\n", (UINT32)Count);
  for (i = 0; (i < Count) && (i < 6); i++) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Sfs = NULL;
    EFI_FILE_PROTOCOL                *Root = NULL;
    EFI_FILE_PROTOCOL                *File = NULL;
    EFI_STATUS                        OpenStatus;

    Status = gBS->HandleProtocol (Handles[i], &mSfsGuid, (VOID **)&Sfs);
    if (EFI_ERROR (Status) || (Sfs == NULL)) {
      Log (L">>> PAYLOAD vol%u: no SFS\r\n", (UINT32)i);
      continue;
    }
    Status = Sfs->OpenVolume (Sfs, &Root);
    if (EFI_ERROR (Status) || (Root == NULL)) {
      Log (L">>> PAYLOAD vol%u: OpenVolume=%r\r\n", (UINT32)i, Status);
      continue;
    }
    OpenStatus = Root->Open (Root, &File, L"\\EFI\\hakios\\payload.bin", EFI_FILE_MODE_READ, 0);
    Log (L">>> PAYLOAD vol%u: open payload.bin = %r\r\n", (UINT32)i, OpenStatus);
    if (!EFI_ERROR (OpenStatus) && (File != NULL)) {
      EFI_FILE_INFO  *Info = NULL;
      UINTN           InfoSize = 0;
      UINT64          FileSize = 0;
      UINT8           Head[16];
      UINTN           HeadSize = sizeof (Head);
      UINTN           j;

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
      Log (L">>> PAYLOAD vol%u: size=%lu (%r)\r\n", (UINT32)i, FileSize, Status);
      ZeroMem (Head, sizeof (Head));
      Status = File->Read (File, &HeadSize, Head);
      if (!EFI_ERROR (Status) && (HeadSize > 0)) {
        Log (L">>> PAYLOAD vol%u head:", (UINT32)i);
        for (j = 0; j < HeadSize; j++) {
          Log (L" %02x", Head[j]);
        }
        Log (L"\r\n");
      }
      File->Close (File);
    }
    Root->Close (Root);
  }
  FreePool (Handles);
}

/* ---------------- StartController ---------------- */
STATIC
VOID
TryStartController (
  IN UINT32  ConnType
  )
{
  EFI_STATUS  Status;
  VOID        *Ifc = NULL;
  UINTN       Func;

  Status = gBS->LocateProtocol (&mUsbConfigGuid, NULL, &Ifc);
  if (EFI_ERROR (Status) || (Ifc == NULL)) {
    Log (L">>> StartController: no USB_CONFIG (%r)\r\n", Status);
    mStartStatus = Status;
    return;
  }
  Func = ((UINTN *)Ifc)[10];
  Log (L">>> StartController vtable[10]=0x%lx\r\n", (UINT64)Func);
  if (Func != 0) {
    Status = ((EFI_STATUS (EFIAPI *)(VOID *, UINT32, UINT32))Func)(Ifc, 0, ConnType);
    Log (L">>> StartController(0,connType=%u) = %r\r\n", ConnType, Status);
    mStartStatus = Status;
  }
}

STATIC
VOID
PrintStat (
  IN UINTN    Beat,
  IN BOOLEAN  TypeCPresent
  )
{
  VOID        *UsbCfg = NULL;
  VOID        *UsbFn = NULL;
  EFI_STATUS   St1;
  EFI_STATUS   St2;

  St1 = gBS->LocateProtocol (&mUsbConfigGuid, NULL, &UsbCfg);
  St2 = gBS->LocateProtocol (&mUsbfnIoGuid, NULL, &UsbFn);
  Log (L">>> ============ STAT (beat %u) ============\r\n", (UINT32)Beat);
  Log (L">>> STAT TypeC=%a USB_CONFIG=%a(%r) USBFN_IO=%a(%r)\r\n",
       TypeCPresent ? "OK" : "NO", (UsbCfg != NULL) ? "OK" : "NO", St1,
       (UsbFn != NULL) ? "OK" : "NO", St2);
  Log (L">>> STAT StartController=%r\r\n", mStartStatus);
  Log (L">>> ========================================\r\n");
}

EFI_STATUS
EFIAPI
UsbProbeAppEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  VOID        *TypeC = NULL;
  BOOLEAN      TypeCPresent;
  UINTN        Beat;
  VOID        *Stub = NULL;
  EFI_HANDLE   StubHandle = NULL;

  mLogFile = NULL;
  OpenLogFile ();

  Log (L"\r\n>>> ================================================\r\n");
  Log (L">>> PROBE-APP: entry reached (UsbProbeApp v1)\r\n");
  Log (L">>> log file: %a:\\probe_log.txt\r\n", mLogVol);
  Log (L">>> ================================================\r\n");

  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&mGop);
  if (EFI_ERROR (Status)) {
    mGop = NULL;
    Log (L">>> GOP not found (%r), text only\r\n", Status);
  }

  TypeC = NULL;
  Status = gBS->LocateProtocol (&mTypeCGuid, NULL, &TypeC);
  TypeCPresent = (!EFI_ERROR (Status)) && (TypeC != NULL);
  Log (L">>> TypeC protocol %a (%r), iface=0x%lx\r\n",
       TypeCPresent ? "EXISTS" : "MISSING", Status, (UINT64)(UINTN)TypeC);

  if (!TypeCPresent) {
    UINTN  k;

    Stub = AllocateZeroPool (0x100);
    if (Stub != NULL) {
      for (k = 0; k < 0x100 / sizeof (UINTN); k++) {
        ((UINTN *)Stub)[k] = (UINTN)(UINTN)&ProbeStubOk;
      }
      Status = gBS->InstallProtocolInterface (&StubHandle, &mTypeCGuid, EFI_NATIVE_INTERFACE, Stub);
      if (!EFI_ERROR (Status)) {
        TypeCPresent = TRUE;
        Log (L">>> TypeC temp stub installed (uninstall before exit)\r\n");
      } else {
        FreePool (Stub);
        Stub = NULL;
      }
    }
  }

  DrawStatus (0, TypeCPresent);

  for (Beat = 1; Beat <= 20; Beat++) {
    DrawStatus (Beat, TypeCPresent);
    Log (L">>> PROBE-APP: HB %u\r\n", (UINT32)Beat);

    if ((Beat == 3) || (Beat == 8) || (Beat == 15)) {
      PrintStat (Beat, TypeCPresent);
    }
    if (Beat == 4) {
      CheckPayload ();
    }
    if (Beat == 5) {
      TryStartController (8);
    }
    if ((Beat == 6) && EFI_ERROR (mStartStatus)) {
      TryStartController (1);
    }
    if (Beat == 12) {
      VOID        *UsbFn = NULL;
      EFI_STATUS   St = gBS->LocateProtocol (&mUsbfnIoGuid, NULL, &UsbFn);
      Log (L">>> 3s after StartController: USBFN_IO=%a (%r)\r\n",
           (UsbFn != NULL) ? "OK" : "NO", St);
    }
    gBS->Stall (500000);
  }

  PrintStat (20, TypeCPresent);
  DrawStatus (20, TypeCPresent);

  if ((Stub != NULL) && (StubHandle != NULL)) {
    gBS->UninstallProtocolInterface (StubHandle, &mTypeCGuid, Stub);
    FreePool (Stub);
    Log (L">>> TypeC temp stub uninstalled\r\n");
  }

  Log (L">>> PROBE-APP: done, exiting. log = %a:\\probe_log.txt\r\n", mLogVol);
  if (mLogFile != NULL) {
    mLogFile->Close (mLogFile);
    mLogFile = NULL;
  }
  return EFI_SUCCESS;
}
