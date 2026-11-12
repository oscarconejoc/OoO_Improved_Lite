#ifndef SOC_CSR_H
#define SOC_CSR_H

// Banco de CSRs de modo maquina de la version Lite.
// Las instrucciones CSR ejecutan en la cabeza del ROB.
#include "soc_top.h"
#include "rv32i_defs.h"
#include "soc_state.h"

static ap_uint<32> read_csr(ap_uint<12> addr) {
    switch (addr.to_uint()) {
        case rv32i::CSR::MSTATUS:  return csr_mstatus;
        case rv32i::CSR::MISA:     return 0x40001104; // RV32IMC
        case rv32i::CSR::MIE:      return csr_mie;
        case rv32i::CSR::MTVEC:    return csr_mtvec;
        case rv32i::CSR::MSCRATCH: return csr_mscratch;
        case rv32i::CSR::MEPC:     return csr_mepc;
        case rv32i::CSR::MCAUSE:   return csr_mcause;
        case rv32i::CSR::MTVAL:    return csr_mtval;
        case rv32i::CSR::MIP:      return csr_mip;
        case rv32i::CSR::MHARTID:  return 0;
        case rv32i::CSR::MTIME:    return csr_mtime;
        case rv32i::CSR::MTIMECMP: return csr_mtimecmp;
        default:                   return 0;
    }
}

static void write_csr(ap_uint<12> addr, ap_uint<32> v) {
    switch (addr.to_uint()) {
        case rv32i::CSR::MSTATUS:  csr_mstatus = v; break;
        case rv32i::CSR::MIE:      csr_mie = v; break;
        case rv32i::CSR::MTVEC:    csr_mtvec = v; break;
        case rv32i::CSR::MSCRATCH: csr_mscratch = v; break;
        case rv32i::CSR::MEPC:     csr_mepc = v; break;
        case rv32i::CSR::MCAUSE:   csr_mcause = v; break;
        case rv32i::CSR::MTVAL:    csr_mtval = v; break;
        case rv32i::CSR::MIP:      csr_mip = v; break;
        case rv32i::CSR::MTIMECMP: csr_mtimecmp = v; break;
        default:                   break;
    }
}

#endif // SOC_CSR_H