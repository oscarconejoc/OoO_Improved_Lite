.section .text
.global _start

_start:
    la   sp, _stack_top   /* configura el stack pointer */
    call main             /* salta a tu main() en C */

_hang:
    j _hang                /* si main() retorna, quedarse aquí */
