#ifndef SOC_STATE_H
#define SOC_STATE_H

// estado del procesador (en el rtl son registros). todo static: el diseno es
// una sola unidad de traduccion, asi que las etapas lo ven directo sin params.
#include "soc_top.h"
#include "rv32i_defs.h"
#include "soc_config.h"

// ================= estado persistente (registros en RTL) =================

struct RatEntry {
    bool       has_tag;
    rob_tag_t tag;
};

// Entrada del ROB: resultado en vuelo, causa de trap y estado necesario para recuperar un branch bajo el predictor estatico NOT-TAKEN.
struct RobEntry {
    bool        valid, ready, is_store;
    bool        takes_trap;   // causa en `cause`: 2 ilegal, 3 ebreak, 11/8 ecall
    bool        is_mret;
    bool        is_branch, br_taken_real, br_mispred;
    ap_uint<32> br_target, pc;
    ap_uint<5>  cause, dest;
    ap_uint<32> value;        // resultado pendiente de commit
    ap_uint<32> addr, sdata;  // solo stores
    ap_uint<3>  mem_f3;
};

struct Operand {
    bool        ready;
    ap_uint<32> val;
    rob_tag_t  tag; // tag ROB esperado cuando !ready
};

struct AluRs {
    bool        busy;
    ap_uint<3>  f3;
    bool        alt;      // SUB/SRA(I)
    rob_tag_t   rob_tag;
    Operand     s1, s2;
};

// Slot de ejecucion de la UNICA ALU fisica.
// Una RS lista se copia aqui durante ISSUE y la RS queda libre.
struct AluExec {
    bool        busy;
    ap_uint<3>  f3;
    bool        alt;
    rob_tag_t   rob_tag;
    ap_uint<32> a, b;
};

struct MdRs {
    bool        busy, executing;
    ap_uint<4>  remaining;
    ap_uint<3>  f3;
    rob_tag_t  rob_tag;
    Operand     s1, s2;
};

struct LsuRs {
    bool        busy;
    bool        is_load;
    ap_uint<3>  f3;
    rob_tag_t  rob_tag;
    int32_t     imm;
    Operand     s1;  // base de direccion
    Operand     s2;  // dato del store (loads: ready=true)
};

struct BrRs {
    bool        busy, executing;
    ap_uint<1>  remaining;
    bool        is_jalr;
    ap_uint<3>  f3;
    rob_tag_t  rob_tag;
    ap_uint<32> br_pc;
    ap_uint<3>  size;   // 2 o 4 (extension C): link y fall-through = pc+size
    int32_t     imm;
    Operand     s1, s2;
};


// Unidad de sistema (bare-metal, items 2-5): una sola reservation
// station para las instrucciones CSR, que ejecuta SOLO en la cabeza del
// ROB (como un load) -- asi el acceso a CSR es en orden de programa y ve
// el estado arquitectonico correcto, sin necesitar el mecanismo de traps
// precisas (item 6, fuera de alcance). ECALL/EBREAK no pasan por aca:
// se marcan en el ROB (is_ecall) y detienen el programa al retirarse.
struct SysRs {
    bool        busy;
    ap_uint<3>  f3;        // CSRRW/S/C y variantes con inmediato
    ap_uint<12> csr_addr;  // instr[31:20]
    rob_tag_t  rob_tag;
    Operand     s1;        // valor de rs1 (CSRRW/S/C) o zimm (variantes I, ready=true)
};


static ap_uint<32> regfile[32];
static RatEntry    rat[32];

static RobEntry    rob[ROB_SZ];
static rob_tag_t   rob_head, rob_tail;
static ap_uint<3>  rob_count;

static AluRs       alu_rs[N_ALU_RS];
static AluExec     alu_exec;
static MdRs        md_rs;
static LsuRs       lsu_rs;
static BrRs        br_rs;
static SysRs       sys_rs;
// csrs de modo maquina (subset minimo bare-metal): solo almacenamiento, sin warl.
static ap_uint<32> csr_mstatus, csr_mie, csr_mtvec, csr_mscratch;
static ap_uint<32> csr_mepc, csr_mcause, csr_mtval, csr_mip;
static bool        ecall_halt;
// interrupciones (timer) y modos de privilegio
static ap_uint<32> csr_mtime;    // contador libre
static ap_uint<32> csr_mtimecmp; // mtime >= mtimecmp -> interrupcion
static ap_uint<2>  cur_priv;     // 3 M-mode, 0 U-mode
static ap_uint<32> fetch_pc;
static bool        fetch_stalled; // solo JALR detiene fetch; branches usan NOT-TAKEN
static ap_uint<32> stat_branches, stat_mispredicts;  // Contadores arquitectonicos de branches retirados y fallos del predictor estatico NOT-TAKEN.
#ifndef __SYNTHESIS__
static ap_uint<2> stat_max_alu_rs_busy; // solo observabilidad de csim
#endif
static bool        fetch_done;    // se encontro la palabra 0 (fin de programa)


#endif // SOC_STATE_H
