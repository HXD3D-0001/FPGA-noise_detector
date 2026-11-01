/* Project-owned layout rules. Addresses come from validated candidate metadata.
 * This specifies the symbol contract consumed by the separately installed BSP
 * CPU0 startup; it contains no generated platform or initializer source. */
ENTRY(_vector_table)
MEMORY {
 LFN_APP (rwx) : ORIGIN = LFN_APP_BASE, LENGTH = LFN_APP_END - LFN_APP_BASE
 LFN_DATA (rw) : ORIGIN = LFN_DATA_BASE, LENGTH = LFN_DATA_END - LFN_DATA_BASE
}
SECTIONS {
 .text : { KEEP(*(.vectors)) KEEP(*(.boot)) *(.text .text.* .gnu.linkonce.t.* .plt .glue_7 .glue_7t .vfp11_veneer) } > LFN_APP
 .init : { KEEP(*(.init)) } > LFN_APP
 .fini : { KEEP(*(.fini)) } > LFN_APP
 .rodata : { *(.rodata .rodata.* .rodata1 .gnu.linkonce.r.*) } > LFN_APP
 .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } > LFN_APP
 .ARM.exidx : { __exidx_start = .; *(.ARM.exidx* .gnu.linkonce.armexidx.*) __exidx_end = .; } > LFN_APP
 .preinit_array : { __preinit_array_start = .; KEEP(*(SORT(.preinit_array.*))) KEEP(*(.preinit_array)) __preinit_array_end = .; } > LFN_APP
 .init_array : { __init_array_start = .; KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array)) __init_array_end = .; } > LFN_APP
 .fini_array : { __fini_array_start = .; KEEP(*(SORT(.fini_array.*))) KEEP(*(.fini_array)) __fini_array_end = .; } > LFN_APP
 .data : { *(.data .data.* .data1 .gnu.linkonce.d.* .got .got.* .jcr) } > LFN_APP
 .sdata : { __sdata_start = .; *(.sdata .sdata.*) } > LFN_APP
 .sdata2 : { __sdata2_start = .; *(.sdata2 .sdata2.*) } > LFN_APP
 .drvcfg_sec : ALIGN(8) { __drvcfgsecdata_start = .; KEEP(*(.drvcfg_sec)) __drvcfgsecdata_end = .; } > LFN_APP
 __drvcfgsecdata_size = __drvcfgsecdata_end - __drvcfgsecdata_start;
 .mmu_tbl : ALIGN(65536) { __mmu_tbl_start = .; KEEP(*(.mmu_tbl)) __mmu_tbl_end = .; } > LFN_APP
 .sbss (NOLOAD) : { __sbss_start = .; *(.sbss .sbss.*) __sbss_end = .; } > LFN_APP
 .sbss2 (NOLOAD) : { *(.sbss2 .sbss2.*) __sbss2_end = .; } > LFN_APP
 .bss (NOLOAD) : { . = ALIGN(16); __bss_start = .; *(.bss .bss.* .gnu.linkonce.b.*) *(COMMON) __bss_end = .; } > LFN_APP
 _SDA_BASE_ = __sdata_start + ((__sbss_end - __sdata_start) / 2);
 _SDA2_BASE_ = __sdata2_start + ((__sbss2_end - __sdata2_start) / 2);
 .heap (NOLOAD) : { . = ALIGN(16); _heap = .; HeapBase = .; _heap_start = .; . += 65536; _heap_end = .; HeapLimit = .; } > LFN_APP
 .stack (NOLOAD) : {
  . = ALIGN(16); _stack_end = .; . += 65536; _stack = .; __stack = .;
  _irq_stack_end = .; . += 1024; __irq_stack = .;
  _supervisor_stack_end = .; . += 2048; __supervisor_stack = .;
  _abort_stack_end = .; . += 1024; __abort_stack = .;
  _fiq_stack_end = .; . += 1024; __fiq_stack = .;
  _undef_stack_end = .; . += 1024; __undef_stack = .; end = .;
 } > LFN_APP
 .lfn_data (NOLOAD) : { . = ALIGN(64); __lfn_data_start = .; KEEP(*(.lfn_data.*)) . = ALIGN(64); __lfn_data_used_end = .; } > LFN_DATA
 .ARM.attributes 0 : { *(.ARM.attributes) }
 /DISCARD/ : { *(.comment .note.GNU-stack) }
}
ASSERT(_vector_table == LFN_APP_BASE, "Vector entry must begin application region")
ASSERT((MMUTable & 65535) == 0, "MMU table must be aligned to64KiB")
ASSERT(__undef_stack <= LFN_APP_END, "Application stacks exceed budget")
ASSERT(__lfn_data_used_end <= LFN_DATA_END, "DMA/pool exceeds32MiB budget")
ASSERT(LFN_APP_END <= LFN_DATA_BASE, "App andDMA regions overlap")
