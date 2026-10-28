_Head:
  adr   x1, _Payload    /* Set _Entry address */
  b     _Start          /* Jump to the real code */

#if REQUIRES_KERNEL_HEADER == 1
_StackBase:
  .quad FD_BASE         /* Text Offset */

_StackSize:
  .quad FD_SIZE         /* Image Size */
  .quad 0               /* Flags */
  .quad 0               /* Reserved */
  .quad 0               /* Reserved */
  .quad 0               /* Reserved */
  .ascii "ARM\x64"      /* ARM64 Magic */
  .long 0               /* Reserved */
#endif

_Start:
  mov   x4, x1

#if DISABLE_GUNYAH_WATCHDOG == 1
  /*
   * Some Gunyah-based boot chains leave the VM watchdog running when ABL
   * exits boot services. Disable it before copying the firmware volume so a
   * slow UEFI initialization cannot expire the inherited timeout.
   *
   * GUNYAH_WDT_CONTROL = ARM_SMCCC fast SMC32, vendor-hyp owner, function 5
   * WDT_CTRL_DISABLE    = BIT(1)
   */
  movz  x0, #0x0005
  movk  x0, #0x8600, lsl #16
  mov   x1, #2
  mov   x2, #0
  smc   #0
#endif

  ldr   x5, =FD_BASE
  cmp   x4, x5
  beq   _Entry
  ldr   x6,  =FD_SIZE

_CopyLoop:
  ldp   x2, x3, [x4], #16
  stp   x2, x3, [x5], #16
  subs  x6, x6, #16
  b.ne  _CopyLoop
  ldr   x5, =FD_BASE

_Entry:
  br    x5

_Dead:
  b     _Dead

.text
.align 4

_Payload:
