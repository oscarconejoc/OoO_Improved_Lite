#ifndef RV32_OOO_H
#define RV32_OOO_H

#include <ap_int.h>

// RISC-V OoO Lite, sintetizable en Vitis HLS.
// Etapa actual: nucleo escalar entero con RAT + ROB + RS + CDB,
// ROB de 4 entradas, 3 ALU RS alimentando 1 ALU fisica, MUL/DIV,
// LSU, branches con predictor estatico NOT-TAKEN, CSR y extension C.
// FPU y coprocesador vectorial eliminados.
//
// Un tick = un ciclo microarquitectonico modelado.

#define OOO_IMEM_WORDS 16384
#define OOO_DMEM_WORDS 16384

// ROB Lite de 4 entradas -> tags de 2 bits (0..3).
typedef ap_uint<2> rob_tag_t;

void riscv_soc_tick(
    ap_uint<1>  reset,
    ap_uint<32> imem[OOO_IMEM_WORDS],
    ap_uint<32> dmem[OOO_DMEM_WORDS],

    // evento de dispatch
    ap_uint<1>&  disp_valid,
    rob_tag_t&   disp_tag,
    ap_uint<32>& disp_pc,

    // eventos de completion
    ap_uint<1>& alu_done,  rob_tag_t& alu_tag,
    ap_uint<1>& md_done,   rob_tag_t& md_tag,
    ap_uint<1>& lsu_done,  rob_tag_t& lsu_tag,
    ap_uint<1>& br_done,   rob_tag_t& br_tag,

    // stream de commit
    ap_uint<1>&  commit_valid,
    ap_uint<5>&  commit_rd,
    ap_uint<32>& commit_value,

    ap_uint<1>& halted
);

#ifndef __SYNTHESIS__
unsigned int soc_stat_branches();
unsigned int soc_stat_mispredicts();
unsigned int soc_stat_max_alu_rs_busy();
#endif

#endif // RV32_OOO_H