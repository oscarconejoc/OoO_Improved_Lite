#ifndef FRONTEND_DISPATCH_H
#define FRONTEND_DISPATCH_H

// decode + dispatch (una instruccion por ciclo, en orden). decide la rs, lee
// operandos renombrados y reserva la entrada del rob. devuelve false si hay
// stall estructural (la unidad esta ocupada) y el fetch reintenta. aca vive
// tabla de decodificacion de la version Lite RV32IMC.
// FPU y RVV no estan implementados y generan Illegal Instruction.
#include "soc_top.h"
#include "rv32i_defs.h"
#include "soc_state.h"
#include "soc_csr.h"
#include "backend.h"
#include "immediates_hls.h"

using namespace rv32_hls;

// `next_fetch` es de entrada/salida: JAL y los saltos resueltos en el
// dispatch lo reescriben para redirigir el fetch.
static bool dispatch_decode(ap_uint<32> instr, ap_uint<3> isize,
                            ap_uint<32> this_pc, rob_tag_t new_tag,
                            ap_uint<32>& next_fetch) {
#pragma HLS INLINE
    uint32_t iw  = instr.to_uint();
    uint32_t opc = rv32i::opcode(iw);
    ap_uint<5> rd  = rv32i::rd(iw);
    ap_uint<3> f3  = rv32i::funct3(iw);
    ap_uint<5> rs1 = rv32i::rs1(iw);
    ap_uint<5> rs2 = rv32i::rs2(iw);
    uint32_t   f7  = rv32i::funct7(iw);
    bool can_dispatch = false;

        if (opc == rv32i::Opcode::LUI || opc == rv32i::Opcode::AUIPC ||
            opc == rv32i::Opcode::JAL) {
            // resuelven completos en el dispatch: valor conocido sin
            // leer registros (JAL ademas redirige el fetch aca mismo)
            can_dispatch = true;
            RobEntry& e = rob[new_tag];
            e.valid = true; e.is_store = false; e.dest = rd; e.ready = true;
            if (opc == rv32i::Opcode::LUI)
                e.value = ap_uint<32>(static_cast<uint32_t>(get_imm_U(instr)));
            else if (opc == rv32i::Opcode::AUIPC)
                e.value = this_pc + ap_uint<32>(static_cast<uint32_t>(get_imm_U(instr)));
            else { // JAL (un C.JAL expandido enlaza a pc+2 via isize)
                e.value = this_pc + isize;
                next_fetch = this_pc + ap_uint<32>(static_cast<uint32_t>(get_imm_J(instr)));
            }
        } else if (opc == rv32i::Opcode::OP && f7 == rv32i::Funct7::MULDIV) {
            if (!md_rs.busy) {
                can_dispatch = true;
                md_rs.busy = true; md_rs.executing = false;
                md_rs.f3 = f3; md_rs.rob_tag = new_tag;
                md_rs.s1 = read_operand(rs1);
                md_rs.s2 = read_operand(rs2);
                RobEntry& e = rob[new_tag];
                e.valid = true; e.is_store = false; e.dest = rd; e.ready = false;
            }
                } else if (opc == rv32i::Opcode::OP || opc == rv32i::Opcode::OP_IMM) {
                    int free_alu_rs = -1;

                    for (int i = N_ALU_RS - 1; i >= 0; i--) {
                        #pragma HLS UNROLL
                        if (!alu_rs[i].busy) free_alu_rs = i;
                    }

                    if (free_alu_rs >= 0) {
                        can_dispatch = true;

                        AluRs& rs = alu_rs[free_alu_rs];

                        rs.busy = true;
                        rs.f3 = f3;
                        rs.rob_tag = new_tag;
                        rs.s1 = read_operand(rs1);
                if (opc == rv32i::Opcode::OP) {
                    rs.alt = (f7 == rv32i::Funct7::ALT);
                    rs.s2 = read_operand(rs2);
                } else {
                    // OP_IMM: el inmediato entra como operando ya listo;
                    // SRAI se distingue por el bit 30 (extension de funct7
                    // dentro del campo de inmediato, igual que en el ISA)
                    rs.alt = (f3 == rv32i::Funct3_ALU::SRL_SRA) && ((iw >> 30) & 1);
                    rs.s2.ready = true;
                    rs.s2.tag = 0;
                    if (f3 == rv32i::Funct3_ALU::SLL || f3 == rv32i::Funct3_ALU::SRL_SRA)
                        rs.s2.val = get_shamt(instr);
                    else
                        rs.s2.val = ap_uint<32>(static_cast<uint32_t>(get_imm_I(instr)));
                }
                RobEntry& e = rob[new_tag];
                e.valid = true; e.is_store = false; e.dest = rd; e.ready = false;
            }
                } else if (opc == rv32i::Opcode::LOAD || opc == rv32i::Opcode::STORE) {

            // Lite: solo loads/stores ENTEROS.
            // FLW/FSW y las operaciones de memoria RVV ya no existen.
            if (!lsu_rs.busy) {
                bool is_load = (opc == rv32i::Opcode::LOAD);

                can_dispatch = true;

                lsu_rs.busy = true;
                lsu_rs.is_load = is_load;
                lsu_rs.f3 = f3;
                lsu_rs.rob_tag = new_tag;

                // La direccion siempre usa un registro entero.
                lsu_rs.s1 = read_operand(rs1);

                if (is_load) {
                    lsu_rs.imm = get_imm_I(instr);

                    // Un load no utiliza s2.
                    lsu_rs.s2.ready = true;
                    lsu_rs.s2.val = 0;
                    lsu_rs.s2.tag = 0;
                } else {
                    lsu_rs.imm = get_imm_S(instr);

                    // Store entero: el dato viene del banco x.
                    lsu_rs.s2 = read_operand(rs2);
                }

                RobEntry& e = rob[new_tag];
                e.valid = true;
                e.ready = false;
                e.is_store = !is_load;
                e.dest = is_load ? rd : ap_uint<5>(0);
            }

        } else if (opc == 0x57u ||  // OP-V: instrucciones vectoriales
                   opc == rv32i::Opcode::LOAD_FP ||
                   opc == rv32i::Opcode::STORE_FP ||
                   opc == rv32i::Opcode::FMADD ||
                   opc == rv32i::Opcode::FMSUB ||
                   opc == rv32i::Opcode::FNMSUB ||
                   opc == rv32i::Opcode::FNMADD ||
                   opc == rv32i::Opcode::OP_FP) {

            // Lite no implementa extensiones F ni V.
            // Se conserva el mecanismo arquitectonico de excepcion
            // precisa: causa 2 = Illegal Instruction.
            can_dispatch = true;

            RobEntry& e = rob[new_tag];
            e.valid = true;
            e.ready = true;
            e.is_store = false;
            e.takes_trap = true;
            e.pc = this_pc;
            e.cause = 2;
            e.dest = 0;
            e.value = 0;
        } else if (opc == rv32i::Opcode::BRANCH || opc == rv32i::Opcode::JALR) {
            if (!br_rs.busy) {
                can_dispatch = true;
                br_rs.busy = true; br_rs.executing = false;
                br_rs.is_jalr = (opc == rv32i::Opcode::JALR);
                br_rs.f3 = f3; br_rs.rob_tag = new_tag;
                br_rs.br_pc = this_pc;
                br_rs.size = isize; // link/fall-through correctos con C
                br_rs.s1 = read_operand(rs1);
                RobEntry& e = rob[new_tag];
                e.valid = true; e.is_store = false; e.ready = false;
                e.dest = br_rs.is_jalr ? rd : ap_uint<5>(0);
                if (br_rs.is_jalr) {
                    br_rs.imm = get_imm_I(instr);
                    br_rs.s2.ready = true; br_rs.s2.val = 0; br_rs.s2.tag = 0;
                    // JALR: el destino sale de un REGISTRO, y sin BTB/RAS no
                    // hay de donde predecirlo -> este si detiene el fetch
                    // hasta resolverse (decision documentada en el README).
                    fetch_stalled = true;
                } else {
                    br_rs.imm = get_imm_B(instr);
                    br_rs.s2 = read_operand(rs2);
                    // ---- LITE: predictor estatico NOT-TAKEN ----
                    // Los branches condicionales nunca detienen el fetch.
                    // Siempre se predicen NO tomados, por lo que next_fetch permanece en this_pc + isize (fall-through).
                    // Si el branch resulta tomado, la unidad BR lo marca como misprediction y el COMMIT redirige el PC y hace flush de todas las instrucciones jovenes.
                    e.is_branch = true;
                }
            }
        } else if (opc == rv32i::Opcode::SYSTEM) {
            // bare-metal (items 2-5): ECALL/EBREAK (funct3=0) e
            // instrucciones CSR (funct3=1..7).
            if (f3 == rv32i::Funct3_SYSTEM::PRIV) {
                uint32_t imm12 = (iw >> 20) & 0xFFF;
                if (imm12 == 0x000 || imm12 == 0x001) {
                    // ECALL (causa 11 = "environment call from M-mode")
                    // / EBREAK (causa 3). Se marca en el ROB y el trap
                    // se toma al retirarse -> excepcion precisa.
                    can_dispatch = true;
                    RobEntry& e = rob[new_tag];
                    e.valid = true; e.is_store = false; e.takes_trap = true;
                    e.pc = this_pc;
                    e.cause = (imm12 == 0x000) ? ap_uint<5>(11) : ap_uint<5>(3);
                    e.dest = 0; e.value = 0; e.ready = true;
                } else if (imm12 == 0x302) {
                    // MRET: retorno desde el handler (pc <- mepc)
                    can_dispatch = true;
                    RobEntry& e = rob[new_tag];
                    e.valid = true; e.is_store = false; e.is_mret = true;
                    e.pc = this_pc; e.dest = 0; e.value = 0; e.ready = true;
                } else {
                    // WFI y otros privilegiados: no-op
                    can_dispatch = true;
                    RobEntry& e = rob[new_tag];
                    e.valid = true; e.is_store = false; e.dest = 0; e.value = 0; e.ready = true;
                }
            } else {
                // CSRRW/CSRRS/CSRRC (y variantes con inmediato de 5 bits)
                if (!sys_rs.busy) {
                    can_dispatch = true;
                    bool is_imm = (f3 == rv32i::Funct3_SYSTEM::CSRRWI ||
                                   f3 == rv32i::Funct3_SYSTEM::CSRRSI ||
                                   f3 == rv32i::Funct3_SYSTEM::CSRRCI);
                    sys_rs.busy = true;
                    sys_rs.f3 = f3;
                    sys_rs.csr_addr = (iw >> 20) & 0xFFF;
                    sys_rs.rob_tag = new_tag;
                    if (is_imm) {
                        // el "fuente" es el campo rs1 usado como zimm[4:0]
                        sys_rs.s1.ready = true; sys_rs.s1.val = rs1; sys_rs.s1.tag = 0;
                    } else {
                        sys_rs.s1 = read_operand(rs1);
                    }
                    RobEntry& e = rob[new_tag];
                    e.valid = true;
                    e.is_store = false;
                    e.dest = rd;
                    e.ready = false;
                }
                // si sys_rs.busy: can_dispatch false -> stall (serializa CSR)
            }
        } else if (opc == rv32i::Opcode::MISC_MEM) {
            // FENCE / FENCE.I: sin caches ni reordenamiento de memoria
            // real observable, se reconoce como no-op (el crt0 la emite).
            can_dispatch = true;
            RobEntry& e = rob[new_tag];
            e.valid = true; e.is_store = false; e.dest = 0; e.value = 0; e.ready = true;
        } else {
            // Opcode no soportado -> EXCEPCION DE INSTRUCCION ILEGAL
            // (causa 2). Antes se retiraba como no-op silencioso, lo
            // que hacia que un binario incompatible diera resultados
            // incorrectos SIN AVISAR. Ahora falla de forma visible y,
            // si hay handler, este puede emular la instruccion.
            can_dispatch = true;
            RobEntry& e = rob[new_tag];
            e.valid = true; e.is_store = false; e.takes_trap = true;
            e.pc = this_pc; e.cause = 2;
            e.dest = 0; e.value = 0; e.ready = true;
        }
    return can_dispatch;
}

#endif // FRONTEND_DISPATCH_H
