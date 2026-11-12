OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY
{
    RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 1M
}
SECTIONS
{
    .text : {
        *(.text .text.*)
    } > RAM
    .data : {
        *(.data .data.*)
    } > RAM
    .bss : {
        *(.bss .bss.*)
    } > RAM
    .metrics : {
        *(.metrics)
    } > RAM
    .results : {
        PROVIDE(__results_start = .);
        *(.results)
        PROVIDE(__results_end = .);
    } > RAM
}

/* Stack al tope de la RAM, creciendo hacia abajo */
_stack_top = ORIGIN(RAM) + LENGTH(RAM);