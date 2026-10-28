/** @file UfsWakeDxe.c
  Unconditional UFS device wake for the mars/m2381 UEFI chain (T9/M-c).

  The m2381 ABL leaves the UFS device in the PowerDown family
  (bCurrentPowerMode = 0x32, R23bg datum).  The OEM UFSDxe (even with the
  silime sleep->wakeup patch) only issues SSU-Active from inside its own
  sleep-transition path, which our boot order never reaches - so the first
  sustained BlockIo read retries sense 04/02 forever and BDS freezes in the
  DiskIo flood.  This driver runs at DXE dispatch, BEFORE UFSDxe ever starts
  a controller, and drives the R23-proven wake sequence unconditionally:

    probe READ_FLAG fDeviceInit (DME_HIBER_EXIT retry on timeout)
    -> SSU (power condition 1 = Active) to WLUN 0xD0, poll bCurrentPowerMode
       until 0x11
    -> NOP OUT
    -> SET fDeviceInit + poll clear

  It then restores the saved interrupt mask and exits; UFSDxe finds an
  Active device and enumerates normally.

  Every constant and routine is transcribed from the R23-proven
  UfsDeviceInitDxe (meizu20Pkg), which validated this exact sequence on this
  exact device (R23bg: SSU pwr 0x1 -> wlun 0xd0 Success, power_mode 0x32 ->
  0x11 immediately).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/IoLib.h>
#include <Library/BaseLib.h>
#include <Protocol/Cpu.h>

//
// UFSHCI register offsets (include/ufs/ufshci.h, enum ufshci_mmio_offset)
//
#define UFSHCI_CAP        0x00
#define UFSHCI_VER        0x08
#define UFSHCI_IS         0x20
#define UFSHCI_IE         0x24
#define UFSHCI_HCS        0x30
#define UFSHCI_CE         0x34
#define UFSHCI_UTRLBA_L   0x50
#define UFSHCI_UTRLBA_H   0x54
#define UFSHCI_UTRLDBR    0x58
#define UFSHCI_UTRL_CLEAR 0x5C
#define UFSHCI_UTRL_RSR   0x60
#define UFSHCI_UTMRLBA_L  0x70
#define UFSHCI_UTMRLBA_H  0x74
#define UFSHCI_UTMRDBR    0x78
#define UFSHCI_UTMRL_RSR  0x80
#define UFSHCI_UICC       0x90
#define UFSHCI_UICC_ARG1  0x94
#define UFSHCI_UICC_ARG2  0x98
#define UFSHCI_UICC_ARG3  0x9C

// HCS = REG_CONTROLLER_STATUS (ufshci.h: DEVICE_PRESENT 0x1, UTRL_RDY 0x2,
// UTMR_RDY 0x4, UIC_CMD_RDY 0x8)
#define HCS_DEVICE_PRESENT 0x1
#define HCS_LISTS_READY    0xE

//
// NOTE (R23bd verdict, on-device): the UFS reset line IS TLMM GPIO 210
// (confirmed against the *live* stock FDT: reset-gpios = <&tlmm 0xd2
// GPIO_ACTIVE_LOW> on ufshc@1d84000), but DRIVING TLMM REGISTERS FROM THIS
// POST-EBS DXE WORLD IS FATAL: the R23bd build read GPIO 210's page
// (ctl 0x3FF io 0xFFFF - already suspicious: not a plain GPIO output) and then
// wrote CTL; the SoC died on the spot and fell into EDL (900e).  Do NOT touch
// TLMM from here.  The pin is reset by the (properly clocked/pinctrl'd) Linux
// kernel when it probes ufs_qcom, or by XBL/ABL before us.


#define UFSHCI_BASE       0x01D84000UL
#define UfsReg32(off)     MmioRead32 (UFSHCI_BASE + (off))

//
// Bit/constant definitions (ufshci.h / ufs.h)
//
#define IS_UTP_TRANSFER_COMPL     0x1       // UTP_TRANSFER_REQ_COMPL
#define IS_UIC_COMMAND_COMPL      0x400     // bit10
#define UIC_CMD_DME_HIBER_EXIT    0x18      // enum uic_cmd_dme
#define UIC_CMD_DME_LINK_STARTUP  0x16      // enum uic_cmd_dme
#define MASK_UIC_RESULT           0xFF
#define OCS_SUCCESS               0x0
#define OCS_INVALID               0x0F

//
// HCS bits (ufshci.h): 0x1 DEVICE_PRESENT, 0x2 UTRL_RDY, 0x4 UTMR_RDY,
// 0x8 UIC_CMD_RDY.  Printed so the log always shows link state.
//

//
// UPIU (uffs.h / scsi_bsg_ufs.h)
//
#define UPIU_TRANSACTION_COMMAND  0x01
#define UPIU_TRANSACTION_QUERY_REQ 0x16
#define UPIU_CMD_FLAGS_READ       0x40      // UPIU_CMD_FLAGS_READ
#define UPIU_COMMAND_SET_TYPE_SCSI 0x00

#define UPIU_QUERY_FUNC_READ      0x01      // UPIU_QUERY_FUNC_STANDARD_READ_REQUEST
#define UPIU_QUERY_FUNC_WRITE     0x81      // UPIU_QUERY_FUNC_STANDARD_WRITE_REQUEST
#define UPIU_QUERY_OPCODE_READ_DESC  0x01
#define UPIU_QUERY_OPCODE_READ_FLAG  0x05
#define UPIU_QUERY_OPCODE_SET_FLAG   0x06
#define QUERY_DESC_IDN_DEVICE     0x00
#define QUERY_DESC_IDN_CONFIGURATION 0x01
#define QUERY_DESC_IDN_UNIT       0x02
#define QUERY_DESC_IDN_HEALTH     0x09
#define QUERY_ATTR_IDN_POWER_MODE 0x02   // ufs.h QUERY_ATTR_IDN_POWER_MODE
#define QUERY_ATTR_IDN_ACTIVE_ICC_LVL 0x03
#define QUERY_ATTR_IDN_REF_CLK_FREQ   0x0A
#define UPIU_QUERY_OPCODE_READ_ATTR   0x03
#define UPIU_QUERY_OPCODE_WRITE_ATTR  0x04
#define QUERY_FLAG_IDN_FDEVICEINIT 0x01

//
// R23bg: the two device-side state movers the kernel relies on when a device is
// not active (coordinates from ufs.h / ufshcd.c).
//
#define UPIU_TRANSACTION_NOP_OUT  0x00      // no-data liveness probe (NOP IN back)
#define WLUN_UFS_DEVICE           0xD0      // ufs.h:64 UFS_UPIU_WLUN_ID(0x80) | 0x50
#define SCSI_START_STOP_UNIT      0x1B      // SPC START STOP UNIT
#define SSU_PWR_COND_ACTIVE       0x01      // power condition 1 = Active (cdb[4] = 1<<4)

//
// R23bh: bare arm64 boot-protocol handoff (no EFI stub, no LoadImage).  Every
// constant below comes out of the kernel tree:
//   - INIT_SCTLR_EL1_MMU_OFF = 0x30600800 (arch/arm64/include/asm/sysreg.h:859,
//     LE variant) = M=0, C=0, EnIA/EnIB/APIA/APIB=0, ATA=0 => "no MMU, no PAC,
//     no MTE": exactly what the kernel expects when entered with the MMU off,
//     and it erases whatever PAC/MTE state Mu left behind.
//   - arm64 image header: offset 0 = harmless instruction whose low 16 bits
//     spell "MZ", offset 4 = "b primary_entry" (our Image: 0x1489b03b, target
//     +0x226c0f0 = primary_entry), offset 16 = image_size, offset 56 = "ARMd".
//   - Documentation/arch/arm64/booting.rst: x0 = DTB physical address,
//     x1..x3 = 0, MMU off, DAIF masked, image cleaned to the PoC.
//
#define INIT_SCTLR_EL1_MMU_OFF_VAL 0x30600800UL
#define KERNEL_IMAGE_ALIGN         0x200000UL   // 2 MB (booting.rst)
// KERNEL_BRANCH_WORD was the old *literal* B encoding for one specific build;
// it is not stable across rebuilds (see the check in DeliverKernel).
#define KERNEL_BRANCH_WORD_OLD     0x1489B03BUL
#define KERNEL_MAX_BYTES           0x4000000UL  // 64 MiB ceiling for the image field

//
// R23bi: hand the kernel a console.  On an EFI boot the stub fills
// sysfb_primary_display.screen, and everything downstream hangs off it:
// earlycon=efifb (drivers/firmware/efi/earlycon.c) and sysfb_init() ->
// simple/efi-framebuffer -> fbcon (CONFIG_SYSFB/FB_EFI/FRAMEBUFFER_CONSOLE are
// all =y in our kernel).  With the bare jump nobody fills it, so we do it here,
// mirroring the EFI stub's fill (drivers/firmware/efi/libstub/gop.c:
// setup_screen_info) with the framebuffer ABL leaves live for our own console:
// 1080x2400 32bpp BGRA at 0xB8000000, stride 1080*4.
//
// SYSFB_PRIMARY_DISPLAY_OFF = sysfb_primary_display - _text.  This is a
// *property of the kernel build*, not a board constant: the GCC 13.3 rebuild
// moved it by 0x9A0000 (R23bi/R23bj build 0x3570E08 -> R23bk build 0x2BD0E08;
// aarch64-linux-gnu-nm on out-ml1b-gcc13/vmlinux).  Baking it into Mu is what
// killed R23bk: the stale 0x3570E08 sat 7.2 MB past the end of the 46.7 MB
// image, so the zero-probe below read unmapped memory, took a translation fault
// (ESR EC 0x25, FAR 0x88570E08 = image base 0x85000000 + stale offset) and the
// kernel was never reached.  The offset now travels in the boot image itself
// (handoff stamp below, written by tools/handoff-stamp.py); this constant is
// only the fallback for an image that carries no stamp.  Valid while the kernel
// does not self-relocate - KASLR stays off because our DTB carries no
// kaslr-seed.
//
#define SYSFB_PRIMARY_DISPLAY_OFF  0x2BD0E08UL
#define SYSFB_SCREEN_INFO_LEN      0x40

//
// Handoff stamp (R23bl): 40 bytes at offset 0x800 of the boot image's first
// page, written at pack time by tools/handoff-stamp.py.  Anything the glue must
// know about a *specific kernel build* belongs in here, never in Mu, so that a
// kernel swap stays a pure repack.  Fields, little endian:
//   0x00 u32 magic "MZSF"      0x04 u32 version
//   0x08 u64 sysfb off         (sysfb_primary_display - _text)
//   0x10 u64 reserved          (unused: the DTB travels in the boot image's dtb
//                               field, its offset derives from the boot header)
//   0x18 u64 reserved
//   0x20 u32 check             (byte sum of 0x00..0x1F)   0x24 u32 reserved
//
#define STAMP_OFF                  0x800
#define STAMP_MAGIC                0x46535A4DUL   // "MZSF" as a little-endian u32
#define STAMP_VERSION              1
#define STAMP_F_SYSFB_OFF          0x08
#define STAMP_F_CHECK              0x20
#define STAMP_CHECK_LEN            0x20
#define FB_BASE_PHYS               0xB8000000UL
#define FB_WIDTH                   1080
#define FB_HEIGHT                  2400
#define FB_DEPTH                   32
#define FB_LINELENGTH              (FB_WIDTH * 4)
#define VIDEO_TYPE_EFI_VAL         0x70
#define RAMOOPS_BASE               0xA5100000UL    // stock memmap, already in our DTS

// Query response value is a BE32 field; we read it raw (little-endian CPU) so
// the flag byte the kernel extracts as (be32_to_cpu(v) & 0xFF) & 1 sits in the
// HIGH byte here.
#define FDEVICEINIT_BIT(v)        (((UINT32)(v) >> 24) & 0x1)

// Device/unit descriptor field offsets (ufs.h enum device_desc_param /
// unit_desc_param)
#define DEV_DESC_NUM_LU           0x06
#define DEV_DESC_ACTVE_ICC_LVL    0x0F
#define DEV_DESC_SPEC_VER         0x10
#define DEV_DESC_MANF_ID          0x18
#define UNIT_DESC_LU_ENABLE       0x03
#define UNIT_DESC_BLK_SIZE        0x0A
#define UNIT_DESC_BLK_COUNT       0x0B

//
// UCD layout (kernel struct utp_transfer_cmd_desc): command UPIU at 0,
// response UPIU at 512 (ALIGNED_UPIU_SIZE), PRDT right after it.
// UTRD DW6/DW7 fields are in double words; prd_table_length is the entry count.
//
#define RSP_UPIU_OFFSET           512
#define PRDT_OFFSET               1024
#define UCD_CLEAR_SIZE            0x800
#define RESP_LEN_DW               (RSP_UPIU_OFFSET / 4)
#define RESP_OFF_DW               (RSP_UPIU_OFFSET / 4)
#define PRDT_OFF_DW               (PRDT_OFFSET / 4)

#define UTRD_CT_UFS_STORAGE       0x1
#define UTP_DEVICE_TO_HOST        0x2       // utp_data_direction (read)
#define UTP_HOST_TO_DEVICE        0x1
#define UTP_NO_DATA_TRANSFER      0x0

#define SCRATCH_PAGES             16        // 64 KB: GPT entries / boot header
#define READ_CHUNK                0x20000   // 128 KB per transfer (< 256 KB PRDT max)
#define STAGE_BYTES_MAX           0x00DE6200UL  // vmlinuz.efi size (0xde6200)

//
// GPT (UEFI spec, LBA-relative: header at LBA 1, entries at LBA 2)
//
#define GPT_SIG                   "EFI PART"
#define GPT_ENTRIES_LBA_OFF       0x48      // u64
#define GPT_ENTRY_COUNT_OFF       0x50      // u32
#define GPT_ENTRY_SIZE_OFF        0x54      // u32
#define GPT_ENTRY_FIRST_LBA_OFF   0x20      // u64
#define GPT_ENTRY_LAST_LBA_OFF    0x28      // u64
#define GPT_ENTRY_NAME_OFF        0x38      // 36 x UTF-16LE

//
// Android boot image headers.  Arch A (2026-09-27): STANDARD field semantics
// - the kernel field holds the raw uncompressed arm64 Image (the pmOS
// mkbootimg layout), the ramdisk field holds the initramfs, the dtb field the
// DTB.  boot_b keeps carrying this firmware (kernel field = Mu); it is not a
// payload candidate any more.
//
// v0-v2 and v3/v4 differ where it matters.  Offsets below are derived from
// mkbootimg's own write order (tools/mkbootimg/mkbootimg.py: write_header and
// write_header_v3_and_above) and cross-checked against real images:
//   v0-v2: magic@0x00 kernel_size@0x08 ramdisk_size@0x10 second_size@0x18
#define WAIT_LOOPS                2000000UL
#define FLAG_CLEAR_POLLS          300       // 300 x ~3 ms ~ 1 s (kernel: 1500 ms)

typedef struct {
  UINT8   Opcode;
  UINT8   Idn;
  UINT8   Index;
  UINT8   Selector;
  UINT16  ReservedOsf;      // BE
  UINT16  Length;           // BE
  UINT32  Value;            // BE
  UINT32  Reserved[2];
} UFS_QUERY_REQ;

STATIC UINT8   *gUcd;          // 4 KB command descriptor (UPIUs + PRDT)
STATIC UINT64   gXblUtrlBase;
STATIC UINT32   gSavedIe;
STATIC UINT32   gSlot;         // UTR slot we send through

STATIC EFI_CPU_ARCH_PROTOCOL *mCpu;

STATIC
EFI_STATUS
MapRangeUncached (
  IN UINT64 Phys,
  IN UINT64 Length)
{
  EFI_PHYSICAL_ADDRESS Page = Phys & ~(EFI_PHYSICAL_ADDRESS)0xFFF;
  UINT64                End  = (Phys + Length + 0xFFF) & ~(UINT64)0xFFF;

  if (mCpu == NULL) {
    return EFI_NOT_READY;
  }

  return mCpu->SetMemoryAttributes (mCpu, Page, End - Page, EFI_MEMORY_UC);
}

STATIC
VOID
Dump8 (
  IN CONST CHAR8  *Tag,
  IN CONST UINT8  *Ptr)
{
  CONST UINT32 *W = (CONST UINT32 *)(CONST VOID *)Ptr;

  DEBUG ((DEBUG_ERROR, "UFS-DevInit: %a %08x %08x %08x %08x %08x %08x %08x %08x\n",
          Tag, W[0], W[1], W[2], W[3], W[4], W[5], W[6], W[7]));
}

// Attribute values are BE32 on the wire; print the semantic value.
STATIC
UINT32
Swap32 (
  IN UINT32 V)
{
  return ((V & 0xFF) << 24) | ((V & 0xFF00) << 8) |
         ((V >> 8) & 0xFF00) | ((V >> 24) & 0xFF);
}

//
// bCurrentPowerMode (QUERY_ATTR_IDN_POWER_MODE), JESD220C Table 7-10:
// 0x00 idle / 0x10 pre-active / 0x11 ACTIVE / 0x20 pre-sleep / 0x22 sleep /
// 0x30 pre-powerdown / 0x33 UFS-PowerDown.  Whatever the device's own
// bInitPowerMode (device descriptor 0x0A) asks for is where it must land after
// initialization; while it sits in a PowerDown/Sleep family mode it answers
// queries but refuses SET fDeviceInit (GENERAL_FAILURE 0xFF) and refuses every
// LU data command (NOT READY / INITIALIZING COMMAND REQUIRED, ASC/ASCQ 04/02).
//
STATIC
CONST CHAR8 *
PowerModeName (
  IN UINT8 Mode
  )
{
  switch (Mode) {
    case 0x00: return "idle";
    case 0x10: return "pre-active";
    case 0x11: return "ACTIVE";
    case 0x20: return "pre-sleep";
    case 0x22: return "sleep";
    case 0x30: return "pre-powerdown";
    case 0x33: return "POWERDOWN";
    default:   return "reserved";
  }
}

//
// All 18 sense bytes (R23bf printed only the first 8, which hid ASC/ASCQ).
// Layout as this device answers: response UPIU +32 = sense length (BE16),
// +34.. = sense data: [2] sense key, [7] additional length, [12]/[13] ASC/ASCQ.
//
STATIC
VOID
DumpSense (
  IN CONST CHAR8  *Tag,
  IN UINT8        *Rsp
  )
{
  UINT16  Len = (UINT16)((Rsp[32] << 8) | Rsp[33]);
  UINT8  *S   = Rsp + 34;

  DEBUG ((DEBUG_ERROR,
          "UFS-DevInit: %a sense len %u: %02x %02x %02x %02x %02x %02x %02x %02x %02x"
          " %02x %02x %02x %02x %02x %02x %02x %02x %02x key 0x%x asc/ascq 0x%x/0x%x\n",
          Tag, Len,
          S[0], S[1], S[2], S[3], S[4], S[5], S[6], S[7], S[8],
          S[9], S[10], S[11], S[12], S[13], S[14], S[15], S[16], S[17],
          S[2], S[12], S[13]));
}

STATIC
VOID
ProbeControllerState (
  OUT BOOLEAN *SafeToSend)
{
  UINT32  D, I, Slot, NonZero = 0;
  UINT32 *W;

  *SafeToSend = FALSE;

  D = UfsReg32 (UFSHCI_UTRLDBR);
  DEBUG ((DEBUG_ERROR, "UFS-DevInit: DBR 0x%08x\n", D));

  I = UfsReg32 (UFSHCI_IS);
  DEBUG ((DEBUG_ERROR, "UFS-DevInit: IS 0x%08x\n", I));

  W = (UINT32 *)(UINTN)gXblUtrlBase;
  Dump8 ("slot0", (CONST UINT8 *)W);

  // This list belongs to UFSDxe (we run after it) and lives in normal DXE
  // memory, so it is writable - unlike XBL's page (R23av hang / R23aw abort).
  for (Slot = 0; Slot < 32; Slot++) {
    W = (UINT32 *)(UINTN)(gXblUtrlBase + (UINT64)Slot * 32);
    if ((W[0] | W[1] | W[2] | W[3] | W[4] | W[5] | W[6] | W[7]) != 0) {
      NonZero++;
    }
  }
  DEBUG ((DEBUG_ERROR, "UFS-DevInit: nonzero slots: %u  DBR 0x%x\n",
          NonZero, D));

  for (Slot = 31; Slot > 0; Slot--) {
    if ((D & (1u << Slot)) == 0) {
      gSlot = Slot;
      break;
    }
  }
  if (Slot == 0) {
    DEBUG ((DEBUG_ERROR, "UFS-DevInit: no idle slot - abort\n"));
    return;
  }

  DEBUG ((DEBUG_ERROR, "UFS-DevInit: using slot %u\n", gSlot));
  *SafeToSend = TRUE;
}

STATIC
EFI_STATUS
WaitBit32 (
  IN UINT32  RegOffset,
  IN UINT32  Mask,
  IN BOOLEAN Set,
  IN UINTN   Loops)
{
  UINTN  i;
  UINT32 v;

  for (i = 0; i < Loops; i++) {
    v = UfsReg32 (RegOffset);
    if (Set ? ((v & Mask) != 0) : ((v & Mask) == 0)) {
      return EFI_SUCCESS;
    }
  }

  return EFI_TIMEOUT;
}

//
// Issue a UIC command (e.g. DME_HIBER_EXIT to wake a hibernated link).
// Completion = IS bit10; result code = ARG2 low byte (kernel semantics).
//
STATIC
EFI_STATUS
SendUicCommand (
  IN  UINT8   Cmd,
  OUT UINT32 *Result)
{
  EFI_STATUS Status;

  MmioWrite32 (UFSHCI_BASE + UFSHCI_IS, 0xFFFFFFFF);   // W1C all (direct)
  MmioWrite32 (UFSHCI_BASE + UFSHCI_UICC_ARG1, 0);
  MmioWrite32 (UFSHCI_BASE + UFSHCI_UICC_ARG2, 0);
  MmioWrite32 (UFSHCI_BASE + UFSHCI_UICC_ARG3, 0);
  MmioWrite32 (UFSHCI_BASE + UFSHCI_UICC, Cmd);

  Status = WaitBit32 (UFSHCI_IS, IS_UIC_COMMAND_COMPL, TRUE, WAIT_LOOPS);
  if (Result != NULL) {
    *Result = MmioRead32 (UFSHCI_BASE + UFSHCI_UICC_ARG2) & MASK_UIC_RESULT;
  }
  MmioWrite32 (UFSHCI_BASE + UFSHCI_IS, IS_UIC_COMMAND_COMPL); // W1C direct
  if (EFI_ERROR (Status)) {
    return EFI_TIMEOUT;
  }

  return EFI_SUCCESS;
}

//
// Fill the UTRD for the next transfer.  DW6/DW7 carry the response length and
// offset in DOUBLE WORDS; prd_table_length is the PRDT entry count (kernel
// ufshcd_sgl_to_prdt) and prd_table_offset is also in double words.
//
STATIC
VOID
FillUtrd (
  IN UINT8   *Utrd,
  IN UINT8    DataDirection,
  IN UINT32   PrdtEntries,
  IN UINT32   PrdtBytes,
  IN BOOLEAN  WithData,
  IN UINT64   DataPhys,
  IN UINT32   DataLen)
{
  SetMem (Utrd, 32, 0);
  Utrd[0]  = 0;                                            // cci
  Utrd[1]  = 0;                                            // ehs_length
  Utrd[2]  = 0;                                            // crypto off
  Utrd[3]  = (UINT8)((UTRD_CT_UFS_STORAGE << 4) | (DataDirection << 1) | 1);
  Utrd[8]  = OCS_INVALID;                                  // ocs prefill
  Utrd[9]  = 0;                                            // cds
  *(UINT16 *)(Utrd + 10) = 0;                              // ldbc (kernel: 0)
  *(UINT64 *)(Utrd + 16) = (UINT64)(UINTN)gUcd;            // DW4-5 UCD base
  *(UINT16 *)(Utrd + 24) = RESP_LEN_DW;                    // DW6 response len
  *(UINT16 *)(Utrd + 26) = RESP_OFF_DW;                    // DW6 response off
  *(UINT16 *)(Utrd + 28) = (UINT16)PrdtEntries;            // DW7 PRDT entries
  *(UINT16 *)(Utrd + 30) = WithData ? (UINT16)PRDT_OFF_DW : 0;

  if (WithData) {
    UINT8 *P = gUcd + PRDT_OFFSET;

    // struct ufshcd_sg_entry { __le64 addr; __le32 reserved; __le32 size; }
    // size is a 0-based byte count; bits 1:0 must be 11b (dword granularity).
    SetMem (P, PrdtBytes, 0);
    *(UINT64 *)(P + 0) = DataPhys;
    *(UINT32 *)(P + 8) = 0;
    *(UINT32 *)(P + 12) = DataLen - 1;
  }
}

//
// Submit the prepared UTRD: arm IS, ring the doorbell, wait for completion,
// then check OCS.  The controller (and, for reads, the device) is the only
// writer of the response area, so we clean our buffers out before and
// invalidate them after.
//
STATIC
EFI_STATUS
SubmitUtp (
  IN  BOOLEAN WithData,
  IN  UINT64  DataPhys,
  IN  UINT32  DataLen,
  OUT UINT8  *Ocs,
  OUT UINT32 *IsSnapshot)
{
  UINT8      *Utrd = (UINT8 *)(UINTN)(gXblUtrlBase + (UINT64)gSlot * 32);
  EFI_STATUS  Status;

  WriteBackInvalidateDataCacheRange (Utrd, 32);
  WriteBackInvalidateDataCacheRange (gUcd, UCD_CLEAR_SIZE);
  if (WithData) {
    InvalidateDataCacheRange ((VOID *)(UINTN)DataPhys, DataLen);
  }

  // W1C: only ever write the bits we own (R23ax: MmioOr32 wiped pending events)
  MmioWrite32 (UFSHCI_BASE + UFSHCI_IS, IS_UTP_TRANSFER_COMPL);
  MmioWrite32 (UFSHCI_BASE + UFSHCI_UTRLDBR, 1u << gSlot);

  Status = WaitBit32 (UFSHCI_IS, IS_UTP_TRANSFER_COMPL, TRUE, WAIT_LOOPS);
  *IsSnapshot = UfsReg32 (UFSHCI_IS);
  MmioWrite32 (UFSHCI_BASE + UFSHCI_IS, IS_UTP_TRANSFER_COMPL);

  InvalidateDataCacheRange (Utrd, 32);
  InvalidateDataCacheRange (gUcd, UCD_CLEAR_SIZE);
  if (WithData) {
    InvalidateDataCacheRange ((VOID *)(UINTN)DataPhys, DataLen);
  }

  *Ocs = Utrd[8];
  if (EFI_ERROR (Status)) {
    return EFI_TIMEOUT;
  }
  if (*Ocs != OCS_SUCCESS) {
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

//
// Generic query sender (READ_FLAG / SET_FLAG / READ_DESCRIPTOR / ...).
// Byte-field writes only - u32 shift construction put query_function and the
// transaction code in the wrong bytes in R23ap (see the header comment).
//
STATIC
EFI_STATUS
SendQuery (
  IN  UINT8    Opcode,
  IN  UINT8    Idn,
  IN  UINT8    Index,
  IN  UINT16   Length,
  IN  UINT32   ValueIn,
  IN  BOOLEAN  Verbose,
  OUT UINT32  *ValueRaw OPTIONAL,
  OUT UINT16  *DsLen OPTIONAL,
  OUT UINT8  **RspUpiu OPTIONAL
  )
{
  UINT8       *Utrd = (UINT8 *)(UINTN)(gXblUtrlBase + (UINT64)gSlot * 32);
  UINT8       *Req  = gUcd;
  UINT8       *Rsp  = gUcd + RSP_UPIU_OFFSET;
  UINT32       IsSnap = 0;
  UINT8        Ocs = 0;
  EFI_STATUS   Status;

  SetMem (gUcd, UCD_CLEAR_SIZE, 0);

  FillUtrd (Utrd, UTP_NO_DATA_TRANSFER, 0, 0, FALSE, 0, 0);

  Req[0]  = UPIU_TRANSACTION_QUERY_REQ;
  Req[1]  = 0;
  Req[2]  = 0;
  Req[3]  = (UINT8)gSlot;                       // task tag echoes the slot
  Req[4]  = 0;
  Req[5]  = (Opcode == UPIU_QUERY_OPCODE_SET_FLAG) ? UPIU_QUERY_FUNC_WRITE
                                                   : UPIU_QUERY_FUNC_READ;
  Req[6]  = 0;
  Req[7]  = 0;
  Req[8]  = 0;
  Req[9]  = 0;
  Req[10] = 0;
  Req[11] = 0;

  Req[12] = Opcode;
  Req[13] = Idn;
  Req[14] = Index;
  Req[15] = 0;
  Req[16] = 0;                                  // reserved_osf (BE16)
  Req[17] = 0;
  Req[18] = (UINT8)(Length >> 8);                // length (BE16)
  Req[19] = (UINT8)(Length & 0xFF);
  Req[20] = (UINT8)(ValueIn >> 24);              // value (BE32, WRITE_ATTR)
  Req[21] = (UINT8)(ValueIn >> 16);
  Req[22] = (UINT8)(ValueIn >> 8);
  Req[23] = (UINT8)(ValueIn);

  Status = SubmitUtp (FALSE, 0, 0, &Ocs, &IsSnap);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR,
            "UFS-DevInit: query 0x%x idn 0x%x failed: %r ocs 0x%x IS 0x%x HCS 0x%x\n",
            Opcode, Idn, Status, Ocs, IsSnap, UfsReg32 (UFSHCI_HCS)));
    return Status;
  }

  if (Verbose) {
    Dump8 ("resp upiu:", Rsp);
    Dump8 ("resp upiu+:", Rsp + 32);
  }

  if (Rsp[6] != 0) {
    DEBUG ((DEBUG_ERROR, "UFS-DevInit: query 0x%x idn 0x%x response 0x%x\n",
            Opcode, Idn, Rsp[6]));
    return EFI_DEVICE_ERROR;
  }

  if (ValueRaw != NULL) {
    // value field: query struct +8 = UPIU+20 (BE32, read raw)
    *ValueRaw = *(UINT32 *)(Rsp + 12 + 8);
  }
  if (DsLen != NULL) {
    // header data_segment_length: BE16 at UPIU+10
    *DsLen = (UINT16)((Rsp[10] << 8) | Rsp[11]);
  }
  if (RspUpiu != NULL) {
    *RspUpiu = Rsp;
  }

  return EFI_SUCCESS;
}

//
// Ask the device for a LUN's logical block size.  Unit descriptor offset 0x0A
// holds the power-of-two exponent (kernel: 1 << desc[BLK_SIZE], 0x0C = 4096).
//
STATIC
VOID
DumpDevState (
  IN CONST CHAR8  *When
  )
{
  EFI_STATUS  Status;
  UINT32      Raw = 0;
  UINT16      DsLen = 0;

  Status = SendQuery (UPIU_QUERY_OPCODE_READ_ATTR, QUERY_ATTR_IDN_POWER_MODE,
                      0, 0, 0, FALSE, &Raw, &DsLen, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "UFS-DevInit: [%a] power_mode %r\n", When, Status));
    return;
  }
  DEBUG ((DEBUG_ERROR, "UFS-DevInit: [%a] power_mode raw %08x = 0x%x (%a)\n",
          When, Raw, Swap32 (Raw) & 0xFF,
          PowerModeName ((UINT8)(Swap32 (Raw) & 0xFF))));

  Status = SendQuery (UPIU_QUERY_OPCODE_READ_FLAG, QUERY_FLAG_IDN_FDEVICEINIT,
                      0, 0, 0, FALSE, &Raw, &DsLen, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "UFS-DevInit: [%a] fDeviceInit %r\n", When, Status));
    return;
  }
  DEBUG ((DEBUG_ERROR, "UFS-DevInit: [%a] fDeviceInit raw 0x%08x flag %u\n",
          When, Raw, FDEVICEINIT_BIT (Raw)));
}

//
// Poll bCurrentPowerMode until the device reports Active (0x11) or ~200 ms pass
// (a power mode transition is not instantaneous).
//
STATIC
VOID
WaitDevActive (
  VOID
  )
{
  UINTN       i;
  UINT32      Raw = 0;
  EFI_STATUS  Status;

  for (i = 0; i < 20; i++) {
    Status = SendQuery (UPIU_QUERY_OPCODE_READ_ATTR, QUERY_ATTR_IDN_POWER_MODE,
                        0, 0, 0, FALSE, &Raw, NULL, NULL);
    if (EFI_ERROR (Status)) {
      return;
    }
    if ((Swap32 (Raw) & 0xFF) == 0x11) {
      DEBUG ((DEBUG_ERROR, "UFS-DevInit: power_mode reached ACTIVE after %u polls\n",
              (UINT32)i));
      return;
    }
    gBS->Stall (10000);
  }

  DEBUG ((DEBUG_ERROR, "UFS-DevInit: power_mode still 0x%x after 200 ms\n",
          Swap32 (Raw) & 0xFF));
}

//
// START STOP UNIT to the UFS Device WLUN (0xD0) - the kernel's
// ufshcd_set_dev_pwr_mode(): CDB[0] 0x1B, CDB[4] = power condition << 4, no
// data.  A device that came out of the boot chain in the Sleep/PowerDown family
// is moved to Active by this one command; until it is Active it refuses SET
// fDeviceInit (GENERAL_FAILURE 0xFF) and every LU read (04/02).
//
STATIC
EFI_STATUS
SendSsu (
  IN UINT8 PwrCond
  )
{
  UINT8      *Utrd = (UINT8 *)(UINTN)(gXblUtrlBase + (UINT64)gSlot * 32);
  UINT8      *Req  = gUcd;
  UINT8      *Rsp  = gUcd + RSP_UPIU_OFFSET;
  UINT32      IsSnap = 0;
  UINT8       Ocs = 0;
  EFI_STATUS  Status;

  SetMem (gUcd, UCD_CLEAR_SIZE, 0);
  FillUtrd (Utrd, UTP_NO_DATA_TRANSFER, 0, 0, FALSE, 0, 0);

  Req[0]  = UPIU_TRANSACTION_COMMAND;
  Req[1]  = 0;                              // no data stage
  Req[2]  = WLUN_UFS_DEVICE;
  Req[3]  = (UINT8)gSlot;                   // task tag echoes the UTRD slot
  Req[4]  = UPIU_COMMAND_SET_TYPE_SCSI;
  Req[16] = SCSI_START_STOP_UNIT;           // CDB[0]
  Req[20] = (UINT8)(PwrCond << 4);          // CDB[4] = power condition

  Status = SubmitUtp (FALSE, 0, 0, &Ocs, &IsSnap);
  DEBUG ((DEBUG_ERROR,
          "UFS-DevInit: SSU pwr 0x%x -> wlun 0xd0: %r ocs 0x%x type 0x%x rsp 0x%x"
          " status 0x%x\n",
          PwrCond, Status, Ocs, Rsp[0], Rsp[6], Rsp[7]));
  if (!EFI_ERROR (Status) && ((Rsp[7] & 0x3F) != 0)) {
    DumpSense ("   ssu", Rsp);
  }

  return Status;
}

//
// NOP OUT (UPIU 0x00, no data): the kernel's ufshcd_verify_dev_init() liveness
// probe - the device answers with a NOP IN (UPIU 0x20) when its UTP layer is up.
//
STATIC
EFI_STATUS
SendNopOut (
  VOID
  )
{
  UINT8      *Utrd = (UINT8 *)(UINTN)(gXblUtrlBase + (UINT64)gSlot * 32);
  UINT8      *Req  = gUcd;
  UINT8      *Rsp  = gUcd + RSP_UPIU_OFFSET;
  UINT32      IsSnap = 0;
  UINT8       Ocs = 0;
  EFI_STATUS  Status;

  SetMem (gUcd, UCD_CLEAR_SIZE, 0);
  FillUtrd (Utrd, UTP_NO_DATA_TRANSFER, 0, 0, FALSE, 0, 0);

  Req[0] = UPIU_TRANSACTION_NOP_OUT;
  Req[1] = 0;
  Req[2] = 0;
  Req[3] = (UINT8)gSlot;

  Status = SubmitUtp (FALSE, 0, 0, &Ocs, &IsSnap);
  DEBUG ((DEBUG_ERROR,
          "UFS-DevInit: NOP OUT %r ocs 0x%x is 0x%x -> type 0x%x (0x20 = NOP IN)\n",
          Status, Ocs, IsSnap, Rsp[0]));
  return Status;
}

//
// DME_LINKSTARTUP, twice - the kernel's device-state mover when the device is
// not active (see the block comment above).  UIC command only.
//
//
// Arch A (2026-09-27): the pmOS payload (standard boot image: kernel +
// initramfs + dtb) lives in boot_a - the INACTIVE slot, which ABL never reads
// or verifies while b is active.  boot_b stays this firmware's own home
// (kernel field = Mu, the ABL chainload trick).  No legacy candidate: if
// boot_a is missing or unreadable the pre-existing wake/retry stages run and
// the device waits, same as the no-partition case since R23bc.
//
// boot_a lives on LUN 4 in this layout (sda..sdf map to UFS LUN 0..5); fall
// back to LUN 0, with that LUN's own block size, if the boot LUN has no entry.
//
// fDeviceInit: SET_FLAG with retries (the kernel's ufshcd_query_flag_retry does
// 3), then poll READ_FLAG until it clears.  A refused SET is NOT fatal: the
// device reports fDeviceInit = 0 when it considers itself initialized and then
// rejects the redundant write (R23bb on-device verdict) - the flag read is the
// statement that counts.
//
STATIC
EFI_STATUS
SetDeviceInit (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT32      Value = 0;
  UINT16      DsLen = 0;
  UINT8      *Rsp = NULL;
  UINTN       Retry, Poll;

  for (Retry = 0; Retry < 3; Retry++) {
    Status = SendQuery (UPIU_QUERY_OPCODE_SET_FLAG, QUERY_FLAG_IDN_FDEVICEINIT,
                        0, 0, 0, (Retry == 0), NULL, &DsLen, &Rsp);
    if (!EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "UFS-DevInit: SET_FLAG fDeviceInit ok (try %u)\n",
              (UINT32)(Retry + 1)));
      break;
    }
    DEBUG ((DEBUG_ERROR, "UFS-DevInit: SET_FLAG try %u refused (%r)\n",
            (UINT32)(Retry + 1), Status));
    gBS->Stall (1000);
  }
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "UFS-DevInit: SET_FLAG refused 3x - flag is the judge\n"));
  }

  for (Poll = 0; Poll < FLAG_CLEAR_POLLS; Poll++) {
    gBS->Stall (1000);
    Status = SendQuery (UPIU_QUERY_OPCODE_READ_FLAG, QUERY_FLAG_IDN_FDEVICEINIT,
                        0, 0, 0, FALSE, &Value, &DsLen, &Rsp);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "UFS-DevInit: READ_FLAG failed at poll %u: %r\n",
              (UINT32)Poll, Status));
      return Status;
    }
    if ((Poll % 64) == 0) {
      DEBUG ((DEBUG_ERROR, "UFS-DevInit: poll %u fDeviceInit=%u raw 0x%08x\n",
              (UINT32)Poll, FDEVICEINIT_BIT (Value), Value));
    }
    if (FDEVICEINIT_BIT (Value) == 0) {
      DEBUG ((DEBUG_ERROR, "UFS-DevInit: fDeviceInit 0 after %u polls\n",
              (UINT32)Poll));
      return EFI_SUCCESS;
    }
  }

  DEBUG ((DEBUG_ERROR, "UFS-DevInit: fDeviceInit never cleared\n"));
  return EFI_TIMEOUT;
}



//
// m2381 wake entry: device Active before UFSDxe ever starts a controller.
//
EFI_STATUS
EFIAPI
UfsWakeDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINT32      Cap, Ver, Value = 0;
  UINT16      DsLen = 0;
  UINT8      *Rsp = NULL;

  // ---- 0. CPU arch (for the uncached mapping) + MMIO before first read ----
  Status = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&mCpu);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "UFS-Wake: no CPU arch protocol, abort\n"));
    return EFI_SUCCESS;
  }

  Status = MapRangeUncached (UFSHCI_BASE, 0x20000);
  DEBUG ((DEBUG_ERROR, "UFS-Wake: map UFSHCI @0x%lx UC: %r\n",
          (UINT64)UFSHCI_BASE, Status));
  if (EFI_ERROR (Status)) {
    return EFI_SUCCESS;
  }

  Cap = UfsReg32 (UFSHCI_CAP);
  Ver = UfsReg32 (UFSHCI_VER);
  if ((Cap == 0xFFFFFFFF) || (Cap == 0)) {
    DEBUG ((DEBUG_ERROR, "UFS-Wake: CAP dead (0x%x) - abort\n", Cap));
    return EFI_SUCCESS;
  }
  DEBUG ((DEBUG_ERROR, "UFS-Wake: CAP 0x%08x VER 0x%x HCS 0x%x\n",
          Cap, Ver, UfsReg32 (UFSHCI_HCS)));

  gSavedIe = UfsReg32 (UFSHCI_IE);

  // ---- 1. ride the running transfer list; never touch RSR/UTRLBA ----
  gXblUtrlBase = (UINT64)UfsReg32 (UFSHCI_UTRLBA_L) |
                 ((UINT64)UfsReg32 (UFSHCI_UTRLBA_H) << 32);
  DEBUG ((DEBUG_ERROR, "UFS-Wake: UTRLBA 0x%016lx - riding it\n", gXblUtrlBase));
  if (gXblUtrlBase == 0) {
    DEBUG ((DEBUG_ERROR, "UFS-Wake: UTRLBA zero - cannot ride, abort\n"));
    return EFI_SUCCESS;
  }

  {
    BOOLEAN  Safe = FALSE;
    ProbeControllerState (&Safe);
    if (!Safe) {
      MmioWrite32 (UFSHCI_BASE + UFSHCI_IE, gSavedIe);
      return EFI_SUCCESS;
    }
  }

  // ---- 2. one UCD page, kept away from UFSDxe's transfer list page ----
  gUcd = AllocatePages (1);
  if (gUcd == NULL) {
    DEBUG ((DEBUG_ERROR, "UFS-Wake: UCD alloc failed\n"));
    return EFI_SUCCESS;
  }

  if ((((UINT64)(UINTN)gUcd + 0x1000) > gXblUtrlBase) &&
      ((UINT64)(UINTN)gUcd < gXblUtrlBase + 32 * 32)) {
    DEBUG ((DEBUG_ERROR, "UFS-Wake: UCD 0x%lx overlaps UTRL 0x%lx - abort\n",
            (UINT64)(UINTN)gUcd, gXblUtrlBase));
    return EFI_SUCCESS;
  }
  DEBUG ((DEBUG_ERROR, "UFS-Wake: UCD 0x%lx\n", (UINT64)(UINTN)gUcd));

  // ---- 3. probe: proves the request/response path (HIBER_EXIT retry) ----
  Status = SendQuery (UPIU_QUERY_OPCODE_READ_FLAG, QUERY_FLAG_IDN_FDEVICEINIT,
                      0, 0, 0, TRUE, &Value, &DsLen, &Rsp);
  DEBUG ((DEBUG_ERROR, "UFS-Wake: probe READ_FLAG %r fDeviceInit raw 0x%08x\n",
          Status, Value));

  if (Status == EFI_TIMEOUT) {
    UINT32  UicResult = 0;

    DEBUG ((DEBUG_ERROR, "UFS-Wake: timeout; IS 0x%x - trying hiber exit\n",
            UfsReg32 (UFSHCI_IS)));
    Status = SendUicCommand (UIC_CMD_DME_HIBER_EXIT, &UicResult);
    DEBUG ((DEBUG_ERROR, "UFS-Wake: HIBER_EXIT %r result 0x%x IS 0x%x\n",
            Status, UicResult, UfsReg32 (UFSHCI_IS)));
    if (!EFI_ERROR (Status)) {
      Status = SendQuery (UPIU_QUERY_OPCODE_READ_FLAG, QUERY_FLAG_IDN_FDEVICEINIT,
                          0, 0, 0, TRUE, &Value, &DsLen, &Rsp);
      DEBUG ((DEBUG_ERROR, "UFS-Wake: probe retry %r raw 0x%08x\n", Status, Value));
    }
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "UFS-Wake: probe failed (%r) - restoring IE, abort\n", Status));
    MmioWrite32 (UFSHCI_BASE + UFSHCI_IE, gSavedIe);
    return EFI_SUCCESS;
  }

  // ---- 4. the R23-proven wake: SSU Active -> poll 0x11 -> NOP -> fDeviceInit ----
  DEBUG ((DEBUG_ERROR, "UFS-Wake: sending SSU pwr cond 1 (Active)\n"));
  SendSsu (SSU_PWR_COND_ACTIVE);
  WaitDevActive ();
  DumpDevState ("UFS-Wake post-ssu");
  SendNopOut ();
  Status = SetDeviceInit ();
  DEBUG ((DEBUG_ERROR, "UFS-Wake: fDeviceInit %r - device ready for UFSDxe\n", Status));

  // ---- 5. restore the XBL interrupt mask; UFSDxe programs its own ----
  MmioWrite32 (UFSHCI_BASE + UFSHCI_IE, gSavedIe);
  DEBUG ((DEBUG_ERROR, "UFS-Wake: done, device Active - handing to UFSDxe\n"));
  return EFI_SUCCESS;
}
