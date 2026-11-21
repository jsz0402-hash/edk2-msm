/** @file
  UsbProbeApp v3 - Shell app probe for h7000 USB device mode.

  v2 additions on top of v1:
   * DUMP section (loaded at heartbeat 10/15):
       - USB_CONFIG protocol interface pointer, driver private struct base (iface-0xE8)
         and a hex dump of it (fields like core base 0xA800000 / PHY enable value);
       - DWC3 / QUSB2PHY / QMP register reads at known Qualcomm physical addresses;
   * everything (incl. dumps) is also appended to \probe_log.txt on a FAT volume.

  Run from startup.nsh inside UEFI Shell. Exits after ~10-12 s, does not block Shell.
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

typedef struct {
  EFI_GUID     Guid;
  CONST CHAR8 *Name;
} PROBE_CAND;

STATIC CONST PROBE_CAND mCand[] = {
  { { 0x6adb9972, 0x083d, 0x4be4, { 0xbb, 0x74, 0xc7, 0x25, 0xa7, 0x30, 0x61, 0x56 } }, "TypeC" },
  { { 0xe722b03f, 0xb250, 0x42ce, { 0x8e, 0xbd, 0x5b, 0xd5, 0x18, 0x12, 0xd0, 0x37 } }, "USB_CONFIG" },
  { { 0x32d2963a, 0xfe5d, 0x4f30, { 0xb6, 0x33, 0x6e, 0x5d, 0xc5, 0x58, 0x03, 0xcc } }, "USBFN_IO" },
  { { 0x241afae6, 0x885f, 0x4f6c, { 0xa7, 0xea, 0xc2, 0x8e, 0xab, 0x79, 0xc3, 0xe5 } }, "Clock" },
  { { 0x22d38d3d, 0xe8b6, 0x4f8f, { 0x9c, 0x26, 0xbc, 0xeb, 0x07, 0xd6, 0xcb, 0x68 } }, "PmicVreg" },
  { { 0xb0760469, 0x970c, 0x487a, { 0xa4, 0xb5, 0x28, 0xdb, 0x7b, 0x45, 0xce, 0xf1 } }, "ChipInfo?" },
  { { 0xf4e5c7d0, 0xd239, 0x47cb, { 0xaa, 0xcd, 0x7f, 0x66, 0xef, 0x76, 0x32, 0x38 } }, "Smem" },
  { { 0x157a5c45, 0x21b2, 0x43c5, { 0xba, 0x7c, 0x82, 0x2f, 0xee, 0x5f, 0xe5, 0x99 } }, "PlatformInfo" },
  { { 0xeb97088e, 0xcfdf, 0x49c6, { 0xbe, 0x4b, 0xd9, 0x06, 0xa5, 0xb2, 0x0e, 0x86 } }, "AcpiSdt" },
  { { 0xae37b942, 0x457f, 0x4c91, { 0xa1, 0x96, 0xd9, 0x66, 0x9f, 0xd3, 0x47, 0xa3 } }, "Spm i?(ChipInfo/Clock/DALTLMM)" },
  { { 0x13a3f0f6, 0x264a, 0x3ef0, { 0xf2, 0xe0, 0xde, 0xc5, 0x12, 0x34, 0x2f, 0x34 } }, "PcdProtocol" },
  { { 0xef9fc172, 0xa1b2, 0x4693, { 0xb3, 0x27, 0x6d, 0x32, 0xfc, 0x41, 0x60, 0x42 } }, "depex:BdsDxe,DisplayEngine,DriverHealthManagerDxe" },
  { { 0x0fd96974, 0x23aa, 0x4cdc, { 0xb9, 0xcb, 0x98, 0xd1, 0x77, 0x50, 0x32, 0x2a } }, "depex:BdsDxe,DisplayEngine,DriverHealthManagerDxe" },
  { { 0x587e72d7, 0xcc50, 0x4f79, { 0x82, 0x09, 0xca, 0x29, 0x1f, 0xc1, 0xa1, 0x0f } }, "depex:BdsDxe,DisplayEngine,DriverHealthManagerDxe" },
  { { 0x26baccb3, 0x6f42, 0x11d4, { 0xbc, 0xe7, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } }, "depex:I2C,SdccDxe,UsbfnDwc3Dxe" },
  { { 0x6441f818, 0x6362, 0x4e44, { 0xb5, 0x70, 0x7d, 0xba, 0x31, 0xdd, 0x24, 0x53 } }, "depex:CapsuleRuntimeDxe,DtPlatformDxe,UsbfnDwc3Dxe" },
  { { 0x1e5668e2, 0x8481, 0x11d4, { 0xbc, 0xf1, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } }, "depex:DtPlatformDxe,RealTimeClock,UsbfnDwc3Dxe" },
  { { 0x2890b3ea, 0x053d, 0x1643, { 0xad, 0x0c, 0xd6, 0x48, 0x08, 0xda, 0x3f, 0xf1 } }, "depex:ArmCpuDxe,ArmTimerDxe" },
  { { 0x26baccb1, 0x6f42, 0x11d4, { 0xbc, 0xe7, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } }, "depex:SimpleFbDxe,UsbfnDwc3Dxe" },
  { { 0x32898322, 0x2da1, 0x474a, { 0xba, 0xaa, 0xf3, 0xf7, 0xcf, 0x56, 0x94, 0x70 } }, "depex:ArmCpuDxe" },
  { { 0x86212936, 0x0e76, 0x41c8, { 0xa0, 0x3a, 0x2a, 0xf2, 0xfc, 0x1c, 0x39, 0xe2 } }, "depex:StatusCodeHandlerRuntimeDxe" },
  { { 0x9e61de7c, 0x9927, 0x4f28, { 0x9f, 0x3d, 0x32, 0x07, 0x05, 0xd8, 0x18, 0x01 } }, "depex:ChipInfo" },
  { { 0xad9aec18, 0x7bf0, 0x4809, { 0x9e, 0x96, 0x30, 0x12, 0x30, 0x9f, 0x3d, 0xf7 } }, "depex:I2C" },
  { { 0x18a031ab, 0xb443, 0x4d1a, { 0xa5, 0xc0, 0x0c, 0x09, 0x26, 0x1e, 0x9f, 0x71 } }, "depex:UsbfnDwc3Dxe" },
  { { 0x76b95dad, 0x1e66, 0x4d98, { 0x9c, 0xe7, 0xb2, 0x94, 0xd9, 0x59, 0x68, 0x95 } }, "depex:UsbfnDwc3Dxe" },
  { { 0x665e3ff6, 0x46cc, 0x11d4, { 0x9a, 0x38, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }, "depex:UsbfnDwc3Dxe" },
  { { 0x26baccb2, 0x6f42, 0x11d4, { 0xbc, 0xe7, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } }, "depex:UsbfnDwc3Dxe" },
  { { 0x1da97072, 0xbddc, 0x4b30, { 0x99, 0xf1, 0x72, 0xa0, 0xb5, 0x6f, 0xff, 0x2a } }, "depex:UsbfnDwc3Dxe" },
  { { 0x27cfac87, 0x46cc, 0x11d4, { 0x9a, 0x38, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }, "depex:UsbfnDwc3Dxe" },
  { { 0x27cfac88, 0x46cc, 0x11d4, { 0x9a, 0x38, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }, "depex:UsbfnDwc3Dxe" },
  { { 0xb7dfb4e1, 0x052f, 0x449f, { 0x87, 0xbe, 0x98, 0x18, 0xfc, 0x91, 0xb7, 0x33 } }, "depex:UsbfnDwc3Dxe" },
  { { 0xa46423e3, 0x4617, 0x49f1, { 0xb9, 0xff, 0xd1, 0xbf, 0xa9, 0x11, 0x58, 0x39 } }, "depex:UsbfnDwc3Dxe" },
  { { 0x665e3ff5, 0x46cc, 0x11d4, { 0x9a, 0x38, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }, "depex:UsbfnDwc3Dxe" },
  { { 0xffe06bdd, 0x6107, 0x46a6, { 0x7b, 0xb2, 0x5a, 0x9c, 0x7e, 0xc5, 0x27, 0x5c } }, "depex:AcpiPlatform" },
  { { 0x03583ff6, 0xcb36, 0x4940, { 0x94, 0x7e, 0xb9, 0xb3, 0x9f, 0x4a, 0xfa, 0xf7 } }, "depex:PlatformSmbiosDxe" },
  { { 0x8557a993, 0xea5d, 0x40fd, { 0x91, 0xb1, 0xf9, 0xba, 0x45, 0x67, 0xba, 0x8d } }, "depex:PlatformSmbiosDxe" },
  { { 0xa770c357, 0xb693, 0x4e6d, { 0xa6, 0xcf, 0xd2, 0x1c, 0x72, 0x8e, 0x55, 0x0b } }, "depex:DisplayEngine" },
  { { 0xb9d4c360, 0xbcfb, 0x4f9b, { 0x92, 0x98, 0x53, 0xc1, 0x36, 0x98, 0x22, 0x58 } }, "depex:DriverHealthManagerDxe" },
  { { 0x1a1241e6, 0x8f19, 0x41a9, { 0xbc, 0x0e, 0xe8, 0xef, 0x39, 0xe0, 0x65, 0x46 } }, "depex:LogoDxe" },
};

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

/* Qualcomm sdm660 (h7000) physical addresses from DT */
#define SSUSB_BASE      0x0A800000ULL   /* ssusb@a800000 (DWC3 wrapper) */
#define QUSB2PHY_BASE   0x0C012000ULL   /* qusb@c012000 (HS PHY) */
#define QMP_BASE        0x0C010000ULL   /* ssphy@c010000 (SS QMP PHY) */

STATIC
EFI_STATUS
EFIAPI
ProbeStubOk (
  VOID
  )
{
  return EFI_SUCCESS;
}

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
    Status = Root->Open (Root, &File, L"\\probe_log.txt",
                         EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
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
  DrawBar (8, 8, 120, 10, 0xFF, 0x00, 0x00);
  if (TypeCPresent) {
    DrawBar (142, 8, 120, 10, 0x00, 0xFF, 0x00);
  }
  if (!EFI_ERROR (mStartStatus)) {
    DrawBar (276, 8, 120, 10, 0x00, 0x00, 0xFF);
  }
  for (i = 0; (i < Beat) && (i < 10); i++) {
    DrawBar (410 + i * 12, 8, 8, 10, 0xFF, 0xFF, 0xFF);
  }
}

/* ---------------- dump helpers ---------------- */
STATIC
VOID
DumpBytes (
  IN CONST CHAR16  *Tag,
  IN CONST UINT8   *Ptr,
  IN UINTN          Len
  )
{
  CHAR16  Line[160];
  UINTN   i;
  UINTN   pos;

  Log (L">>> DUMP %s @0x%lx len=%u\r\n", Tag, (UINT64)(UINTN)Ptr, (UINT32)Len);
  for (i = 0; i < Len; i += 16) {
    pos = 0;
    pos += UnicodeSPrint (Line + pos, sizeof (Line) - pos * sizeof (CHAR16), L"  %04x:", (UINT32)i);
    {
      UINTN j;
      for (j = 0; (j < 16) && (i + j < Len); j++) {
        pos += UnicodeSPrint (Line + pos, sizeof (Line) - pos * sizeof (CHAR16), L" %02x", Ptr[i + j]);
      }
    }
    UnicodeSPrint (Line + pos, sizeof (Line) - pos * sizeof (CHAR16), L"\r\n");
    Log (L"%s", Line);
  }
}

STATIC
UINT32
Rd32 (
  IN UINT64  Addr
  )
{
  return *(volatile UINT32 *)(UINTN)Addr;
}

STATIC
VOID
DumpRegs (
  VOID
  )
{
  VOID    *Ifc = NULL;
  UINT8   *Base;
  EFI_STATUS  Status;

  Log (L">>> ============ DUMP (v2) ============\r\n");

  Status = gBS->LocateProtocol (&mUsbConfigGuid, NULL, &Ifc);
  Log (L">>> USB_CONFIG iface=0x%lx (%r)\r\n", (UINT64)(UINTN)Ifc, Status);
  if (!EFI_ERROR (Status) && (Ifc != NULL)) {
    DumpBytes (L"usbcfg-vtable", (CONST UINT8 *)Ifc, 0x40);
    Base = (UINT8 *)Ifc - 0xE8;
    if ((UINTN)Base > 0x1000) {
      UINT64 CoreBase = *(volatile UINT64 *)(UINTN)(Base + 0x68);
      Log (L">>> drvpriv base=0x%lx  [base+0x68](core base?)=0x%lx\r\n",
           (UINT64)(UINTN)Base, CoreBase);
      DumpBytes (L"drvpriv-0x100", (CONST UINT8 *)Base, 0x100);
    }
  }

  Log (L">>> DWC3 regs @0x%lx:\r\n", (UINT64)SSUSB_BASE);
  Log (L">>>   +0x000=0x%08x +0x004=0x%08x +0x008=0x%08x +0x00c=0x%08x\r\n",
       Rd32 (SSUSB_BASE + 0x000), Rd32 (SSUSB_BASE + 0x004),
       Rd32 (SSUSB_BASE + 0x008), Rd32 (SSUSB_BASE + 0x00c));
  Log (L">>>   +0x4c8=0x%08x +0x4cc=0x%08x +0x504=0x%08x +0x50c=0x%08x\r\n",
       Rd32 (SSUSB_BASE + 0x4C8), Rd32 (SSUSB_BASE + 0x4CC),
       Rd32 (SSUSB_BASE + 0x504), Rd32 (SSUSB_BASE + 0x50C));
  Log (L">>>   +0x40c=0x%08x +0xc100=0x%08x +0xc104=0x%08x +0xc110=0x%08x\r\n",
       Rd32 (SSUSB_BASE + 0x40C), Rd32 (SSUSB_BASE + 0xC100),
       Rd32 (SSUSB_BASE + 0xC104), Rd32 (SSUSB_BASE + 0xC110));
  Log (L">>>   +0xc120=0x%08x +0xc124=0x%08x +0xc140=0x%08x +0xc144=0x%08x\r\n",
       Rd32 (SSUSB_BASE + 0xC120), Rd32 (SSUSB_BASE + 0xC124),
       Rd32 (SSUSB_BASE + 0xC140), Rd32 (SSUSB_BASE + 0xC144));

  Log (L">>> QUSB2PHY regs @0x%lx:\r\n", (UINT64)QUSB2PHY_BASE);
  Log (L">>>   +0x00=0x%08x +0x04=0x%08x +0x08=0x%08x +0x0c=0x%08x\r\n",
       Rd32 (QUSB2PHY_BASE + 0x00), Rd32 (QUSB2PHY_BASE + 0x04),
       Rd32 (QUSB2PHY_BASE + 0x08), Rd32 (QUSB2PHY_BASE + 0x0C));
  Log (L">>>   +0x30=0x%08x +0x34=0x%08x +0x40=0x%08x +0x44=0x%08x\r\n",
       Rd32 (QUSB2PHY_BASE + 0x30), Rd32 (QUSB2PHY_BASE + 0x34),
       Rd32 (QUSB2PHY_BASE + 0x40), Rd32 (QUSB2PHY_BASE + 0x44));

  Log (L">>> QMP regs @0x%lx:\r\n", (UINT64)QMP_BASE);
  Log (L">>>   +0x00=0x%08x +0x04=0x%08x +0x08=0x%08x +0x20=0x%08x\r\n",
       Rd32 (QMP_BASE + 0x00), Rd32 (QMP_BASE + 0x04),
       Rd32 (QMP_BASE + 0x08), Rd32 (QMP_BASE + 0x20));
  Log (L">>>   +0x24=0x%08x +0x30=0x%08x +0x34=0x%08x +0x38=0x%08x\r\n",
       Rd32 (QMP_BASE + 0x24), Rd32 (QMP_BASE + 0x30),
       Rd32 (QMP_BASE + 0x34), Rd32 (QMP_BASE + 0x38));
  Log (L">>> =================================\r\n");
}

/* ---------------- payload check ---------------- */
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
      continue;
    }
    Status = Sfs->OpenVolume (Sfs, &Root);
    if (EFI_ERROR (Status) || (Root == NULL)) {
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
LocateAll (
  VOID
  )
{
  UINTN       i;
  VOID        *Ifc;
  EFI_STATUS  Status;

  Log (L">>> ============ LOCATE TABLE (%u) ============\r\n", (UINT32)ARRAY_SIZE (mCand));
  for (i = 0; i < ARRAY_SIZE (mCand); i++) {
    Ifc = NULL;
    Status = gBS->LocateProtocol (&mCand[i].Guid, NULL, &Ifc);
    Log (L">>> LOC %-36a = %a (%r) ptr=0x%lx\r\n",
         mCand[i].Name, EFI_ERROR (Status) ? "MISSING" : "OK     ", Status,
         (UINT64)(UINTN)Ifc);
  }
  Log (L">>> ==========================================\r\n");
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
  Log (L">>> PROBE-APP v3: entry reached\r\n");
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

  for (Beat = 1; Beat <= 24; Beat++) {
    DrawStatus (Beat, TypeCPresent);
    Log (L">>> PROBE-APP: HB %u\r\n", (UINT32)Beat);

    if ((Beat == 2) || (Beat == 13)) {
      LocateAll ();
    }
    if ((Beat == 3) || (Beat == 15)) {
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
    if (Beat == 10) {
      DumpRegs ();
    }
    if (Beat == 14) {
      VOID        *UsbFn = NULL;
      EFI_STATUS   St = gBS->LocateProtocol (&mUsbfnIoGuid, NULL, &UsbFn);
      Log (L">>> after StartController: USBFN_IO=%a (%r)\r\n",
           (UsbFn != NULL) ? "OK" : "NO", St);
      PrintStat (Beat, TypeCPresent);
    }
    gBS->Stall (500000);
  }

  PrintStat (24, TypeCPresent);
  DrawStatus (24, TypeCPresent);

  if ((Stub != NULL) && (StubHandle != NULL)) {
    gBS->UninstallProtocolInterface (StubHandle, &mTypeCGuid, Stub);
    FreePool (Stub);
    Log (L">>> TypeC temp stub uninstalled\r\n");
  }

  Log (L">>> PROBE-APP v3: done, exiting. log = %a:\\probe_log.txt\r\n", mLogVol);
  if (mLogFile != NULL) {
    mLogFile->Close (mLogFile);
    mLogFile = NULL;
  }
  return EFI_SUCCESS;
}
