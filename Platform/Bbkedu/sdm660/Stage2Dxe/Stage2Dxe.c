/** @file
  阶段探针: 用于确定 DXE 调度卡在哪一段. 本模块会打印 STAGE 标记.
**/
#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>

EFI_STATUS
EFIAPI
Stage2Entry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  AsciiPrint ("\nSTAGE2: reached (after vendor DXE block / UsbConfig+UsbInit+UsbDevice+UsbfnDwc3+UsbMsd)\n");
  DEBUG ((DEBUG_ERROR, "STAGE2: reached (after vendor DXE block)\n"));
  return EFI_SUCCESS;
}
