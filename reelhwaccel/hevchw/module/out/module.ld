ENTRY(module_base)
SECTIONS
{
  . = 0;
  .text : { *(.text.header) *(.text .text.*) *(.rodata .rodata.*) *(.data .data.*) *(.bss .bss.* COMMON) . = ALIGN(4); }
  /DISCARD/ : { *(.comment) *(.ARM.attributes) *(.note*) *(.ARM.exidx*) }
}
