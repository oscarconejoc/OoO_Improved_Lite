#ifndef BACKEND_H
#define BACKEND_H

// unidades de ejecucion escalares + bypass: CDB, lectura de operandos
// renombrados, ALU, mul/div y condicion de salto. funciones puras salvo
// cdb_broadcast, que despierta las estaciones de reserva.
#include "soc_top.h"
#include "rv32i_defs.h"
#include "soc_state.h"
#include "immediates_hls.h"

using namespace rv32_hls;

// ================= helpers combinacionales =================

// despierta a toda rs que esperaba `tag` (el rol del cdb). se llama una vez por
// unidad que completa en el tick (bus de resultado por unidad).
static void cdb_broadcast(rob_tag_t tag, ap_uint<32> value) {
    #pragma HLS INLINE
    for (int i = 0; i < N_ALU_RS; i++) {
#pragma HLS UNROLL
        if (alu_rs[i].busy && !alu_rs[i].s1.ready && alu_rs[i].s1.tag == tag) {
            alu_rs[i].s1.ready = true; alu_rs[i].s1.val = value;
        }
        if (alu_rs[i].busy && !alu_rs[i].s2.ready && alu_rs[i].s2.tag == tag) {
            alu_rs[i].s2.ready = true; alu_rs[i].s2.val = value;
        }
    }
    if (md_rs.busy && !md_rs.s1.ready && md_rs.s1.tag == tag) {
        md_rs.s1.ready = true; md_rs.s1.val = value;
    }
    if (md_rs.busy && !md_rs.s2.ready && md_rs.s2.tag == tag) {
        md_rs.s2.ready = true; md_rs.s2.val = value;
    }
    if (lsu_rs.busy && !lsu_rs.s1.ready && lsu_rs.s1.tag == tag) {
        lsu_rs.s1.ready = true; lsu_rs.s1.val = value;
    }
    if (lsu_rs.busy && !lsu_rs.s2.ready && lsu_rs.s2.tag == tag) {
        lsu_rs.s2.ready = true; lsu_rs.s2.val = value;
    }
    if (br_rs.busy && !br_rs.s1.ready && br_rs.s1.tag == tag) {
        br_rs.s1.ready = true; br_rs.s1.val = value;
    }
    if (br_rs.busy && !br_rs.s2.ready && br_rs.s2.tag == tag) {
        br_rs.s2.ready = true; br_rs.s2.val = value;
    }
    if (sys_rs.busy && !sys_rs.s1.ready && sys_rs.s1.tag == tag) {
        sys_rs.s1.ready = true; sys_rs.s1.val = value;
    }
}

// Lectura/escritura del banco de CSRs (subset minimo de modo maquina).
// misa/mhartid son de solo lectura (valores fijos); el resto es
static Operand read_operand(ap_uint<5> reg) {
    #pragma HLS INLINE
    Operand op;
    if (reg == 0) {
        op.ready = true; op.val = 0; op.tag = 0;
        return op;
    }
    if (!rat[reg].has_tag) {
        op.ready = true; op.val = regfile[reg]; op.tag = 0;
        return op;
    }
    rob_tag_t t = rat[reg].tag;
    if (rob[t].ready) {
        op.ready = true; op.val = rob[t].value; op.tag = 0;
    } else {
        op.ready = false; op.val = 0; op.tag = t;
    }
    return op;
}


// ALU entera RV32I -- identica semantica al bloque OP/OP_IMM de
// rv32_core.cpp, factorizada como funcion pura (fase 1 del plan:
// separar decode de execute).
static ap_uint<32> alu_compute(ap_uint<3> f3, bool alt, ap_uint<32> a_u, ap_uint<32> b_u) {
    int32_t a = static_cast<int32_t>(a_u.to_uint());
    int32_t b = static_cast<int32_t>(b_u.to_uint());
    ap_uint<5> sh = b_u.range(4, 0);
    switch (f3.to_uint()) {
        case rv32i::Funct3_ALU::ADD_SUB: return alt ? ap_uint<32>(a - b) : ap_uint<32>(a + b);
        case rv32i::Funct3_ALU::SLL:     return a_u << sh;
        case rv32i::Funct3_ALU::SLT:     return (a < b) ? 1 : 0;
        case rv32i::Funct3_ALU::SLTU:    return (a_u < b_u) ? 1 : 0;
        case rv32i::Funct3_ALU::XOR:     return a_u ^ b_u;
        case rv32i::Funct3_ALU::SRL_SRA: return alt ? ap_uint<32>(a >> sh.to_uint()) : ap_uint<32>(a_u >> sh);
        case rv32i::Funct3_ALU::OR:      return a_u | b_u;
        case rv32i::Funct3_ALU::AND:     return a_u & b_u;
        default:                         return 0;
    }
}

// Extension M completa, con los mismos casos borde del ISA que
// rv32_core.cpp (division por cero, overflow INT32_MIN/-1).
static ap_uint<32> md_compute(ap_uint<3> f3, ap_uint<32> a_u, ap_uint<32> b_u) {
    uint32_t ua = a_u.to_uint(), ub = b_u.to_uint();
    int32_t  sa = static_cast<int32_t>(ua), sb = static_cast<int32_t>(ub);
    int64_t  a64 = sa, b64 = sb;
    uint64_t ua64 = ua, ub64 = ub;
    switch (f3.to_uint()) {
        case rv32i::Funct3_MULDIV::MUL:
            return ua * ub;
        case rv32i::Funct3_MULDIV::MULH:
            return static_cast<uint32_t>(static_cast<uint64_t>(a64 * b64) >> 32);
        case rv32i::Funct3_MULDIV::MULHSU:
            return static_cast<uint32_t>(static_cast<uint64_t>(a64 * static_cast<int64_t>(ub64)) >> 32);
        case rv32i::Funct3_MULDIV::MULHU:
            return static_cast<uint32_t>((ua64 * ub64) >> 32);
        case rv32i::Funct3_MULDIV::DIV:
            if (sb == 0)                          return 0xFFFFFFFF;
            if (ua == 0x80000000u && sb == -1)    return 0x80000000;
            return static_cast<uint32_t>(sa / sb);
        case rv32i::Funct3_MULDIV::DIVU:
            return (ub == 0) ? 0xFFFFFFFF : (ua / ub);
        case rv32i::Funct3_MULDIV::REM:
            if (sb == 0)                          return ua;
            if (ua == 0x80000000u && sb == -1)    return 0;
            return static_cast<uint32_t>(sa % sb);
        case rv32i::Funct3_MULDIV::REMU:
            return (ub == 0) ? ua : (ua % ub);
        default:
            return 0;
    }
}


static bool branch_taken(ap_uint<3> f3, ap_uint<32> a_u, ap_uint<32> b_u) {
    int32_t a = static_cast<int32_t>(a_u.to_uint());
    int32_t b = static_cast<int32_t>(b_u.to_uint());
    switch (f3.to_uint()) {
        case rv32i::Funct3_BRANCH::BEQ:  return a_u == b_u;
        case rv32i::Funct3_BRANCH::BNE:  return a_u != b_u;
        case rv32i::Funct3_BRANCH::BLT:  return a < b;
        case rv32i::Funct3_BRANCH::BGE:  return a >= b;
        case rv32i::Funct3_BRANCH::BLTU: return a_u < b_u;
        case rv32i::Funct3_BRANCH::BGEU: return a_u >= b_u;
        default:                         return false;
    }
}

// Acceso a memoria de datos direccionada por palabra -- misma tecnica
// (y misma limitacion de no cruzar palabra) que mem_load/mem_store de
// rv32_core.cpp, sobre el arreglo dmem local de esta pista.

#endif // BACKEND_H
