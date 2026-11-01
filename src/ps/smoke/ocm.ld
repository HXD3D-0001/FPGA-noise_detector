OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)
MEMORY {
    PROGRAM (rwx) : ORIGIN = 0x00010000, LENGTH = 0x0001e000
    STACK (rw) : ORIGIN = 0x0002e000, LENGTH = 0x00001000
    MAILBOX (rw) : ORIGIN = 0x0002f000, LENGTH = 0x00000040
}
PHDRS {
    code PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}
SECTIONS {
    .vectors : { KEEP(*(.vectors)) } > PROGRAM :code
    .text : { *(.text.startup) *(.text .text.*) } > PROGRAM :code
    .rodata : { *(.rodata .rodata.*) } > PROGRAM :code
    .data : { . = ALIGN(8); *(.data .data.*) } > PROGRAM :data
    .bss (NOLOAD) : {
        . = ALIGN(8);
        __bss_start = .;
        *(.bss .bss.*) *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    } > PROGRAM :data
    __program_end = .;
    __stack_bottom = ORIGIN(STACK);
    __stack_top = ORIGIN(STACK) + LENGTH(STACK);
    __mailbox_start = ORIGIN(MAILBOX);
    __mailbox_end = ORIGIN(MAILBOX) + LENGTH(MAILBOX);
    /DISCARD/ : {
        *(.comment) *(.note*)
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    }
    ASSERT(ADDR(.vectors)==0x00010000,"Vectors must begin at OCM entry")
    ASSERT(SIZEOF(.vectors)==32,"ARM vectors must contain exactly eight branches")
    ASSERT(__program_end<=0x0002e000,"Program exceeds assigned OCM region")
    ASSERT(__stack_bottom==0x0002e000 && __stack_top==0x0002f000,"Stack bounds changed")
    ASSERT(__mailbox_start==0x0002f000 && __mailbox_end==0x0002f040,"Mailbox bounds changed")
}
