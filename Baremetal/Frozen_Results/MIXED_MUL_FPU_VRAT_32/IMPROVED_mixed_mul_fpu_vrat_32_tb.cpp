#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include "soc_top.h"
#include "rv32i_defs.h"
#include "rv32c_defs.h" // encoders r_type/i_type/s_type/b_type/u_type/j_type

// Binarios ELF reales, compilados con gcc (ver baremetal/ y build.sh)
#include "trap_elf.h"    // handler de excepciones + MRET (reanudable)
#include "full_elf.h"    // UART + interrupcion de timer + modos M/U
#include "printf_elf.h"  // printf de la biblioteca C (newlib)

// =====================================================================
// TESTBENCH UNICO del core RV32IMFC + RVV + OOO.
//
// Corre cuatro suites en secuencia, reseteando el core entre cada una:
//
//   A. ISA + RVV  -- programa ensamblado a mano (I+M+F+C y RVV Fases 1-3:
//      mascara, EEW 8/16/32, comparaciones, merge, logica de mascaras,
//      vid/viota/vcpop/vfirst), con evidencia de ejecucion fuera de orden
//      y de coprocesamiento vectorial.
//   B. Excepciones -- ELF real: handler en mtvec, ECALL, MRET y el
//      programa CONTINUA (excepcion precisa y reanudable).
//   C. Sistema     -- ELF real: UART, interrupcion de timer y U-mode.
//   D. printf      -- ELF real enlazado contra newlib (~54 KB).
//
// Todo se verifica sin backdoor: el estado se reconstruye desde el stream
// de commit o desde lo que el propio programa deja en memoria.
// =====================================================================

using namespace rv32c;
namespace F3A = rv32i::Funct3_ALU;
namespace F3M = rv32i::Funct3_MULDIV;
namespace F7F = rv32i::Funct7_FP;

// Codificacion RVV -- mismos campos de bits que rv32_vector.cpp/rv32_ooo.cpp,
// verificados contra la especificacion oficial RVV v1.0.
static ap_uint<32> enc_vec_mem(uint32_t opcode, uint32_t vd_or_vs3, uint32_t rs1) {
    return (0u << 29) | (0u << 28) | (0u << 26) | (1u << 25) | (0u << 20) |
           (rs1 << 15) | (0b110u << 12) | (vd_or_vs3 << 7) | opcode;
}
static ap_uint<32> enc_op_v(uint32_t funct6, uint32_t vs2, uint32_t vs1, uint32_t funct3, uint32_t vd) {
    const uint32_t vm = 1;
    return (funct6 << 26) | (vm << 25) | (vs2 << 20) | (vs1 << 15) | (funct3 << 12) | (vd << 7) | 0b1010111u;
}
// vsetvli rd, rs1, vtypei -- instr[31]=0, zimm[10:0]=instr[30:20],
// rs1=AVL, funct3=111 (OPCFG). Formato de la seccion 5 de la spec.
static ap_uint<32> enc_vsetvli(uint32_t rd, uint32_t rs1, uint32_t vtypei) {
    return ((vtypei & 0x7FF) << 20) | (rs1 << 15) | (0b111u << 12) | (rd << 7) | 0b1010111u;
}
// vtype para SEW=32 (vsew=010 en bits[5:3]) y LMUL=1 (vlmul=000 en bits[2:0])
static const uint32_t VTYPE_E32_M1 = (0b010u << 3) | 0b000u;
// version con control de mascara: vm=0 -> predicada por v0
static ap_uint<32> enc_op_v_m(uint32_t funct6, uint32_t vs2, uint32_t vs1,
                              uint32_t funct3, uint32_t vd, uint32_t vm) {
    return (funct6 << 26) | (vm << 25) | (vs2 << 20) | (vs1 << 15) | (funct3 << 12) | (vd << 7) | 0b1010111u;
}
static const uint32_t F3_OPIVI = 0b011, F3_OPIVX = 0b100;
static const uint32_t F6_VAND = 0b001001, F6_VOR = 0b001010;
static const uint32_t F6_VMAX = 0b000111, F6_VSLL = 0b100101;
static const uint32_t F6_VDIV = 0b100001, F6_VRSUB = 0b000011;
// EEW variable: vtype con SEW=8 / SEW=16, y width de vle8/vle16
static const uint32_t VTYPE_E8_M1  = (0b000u << 3) | 0b000u; // VLMAX=16
static const uint32_t VTYPE_E16_M1 = (0b001u << 3) | 0b000u; // VLMAX=8
static const uint32_t WIDTH_8  = 0b000, WIDTH_16 = 0b101;
// Fase 3: comparaciones, merge, logica de mascaras y grupos unary
static const uint32_t F6_VMSEQ = 0b011000, F6_VMSLT = 0b011011, F6_VMSGT = 0b011111;
static const uint32_t F6_VMERGE = 0b010111;
static const uint32_t F6_VMAND = 0b011001, F6_VMOR = 0b011010;
static const uint32_t F6_VWXUNARY0 = 0b010000, F6_VMUNARY0 = 0b010100;
static const uint32_t VS1_VCPOP = 0b10000, VS1_VFIRST = 0b10001;
static const uint32_t VS1_VIOTA = 0b10000, VS1_VID = 0b10001;
static ap_uint<32> enc_vec_mem_w(uint32_t opcode, uint32_t vd_or_vs3, uint32_t rs1, uint32_t width) {
    return (1u << 25) | (rs1 << 15) | (width << 12) | (vd_or_vs3 << 7) | opcode;
}


// funct3 de las familias vectoriales (los usa la suite de Fase 4)
static const uint32_t rvv_f3_opivv = 0b000, rvv_f3_opmvv = 0b010;
// funct6 de la Fase 4 (tabla de la seccion 19 de la spec)
static const uint32_t F6_VREDSUM = 0b000000, F6_VREDAND = 0b000001, F6_VREDMAX = 0b000111;
static const uint32_t F6_VRGATHER = 0b001100, F6_VSLIDEUP = 0b001110, F6_VSLIDEDOWN = 0b001111;
static const uint32_t F6_VCOMPRESS = 0b010111;
static const uint32_t F6_VSADDU = 0b100000, F6_VSSUBU = 0b100010, F6_VAADD = 0b001001;

// ---------------------------------------------------------------------
// Suite A2: RVV Fase 4 (reducciones, permutaciones, punto fijo).
// Programa aparte para tener los 32 registros vectoriales libres.
// ---------------------------------------------------------------------
static int suite_fase4() {
    ap_uint<32> imem[OOO_IMEM_WORDS] = {0};
    ap_uint<32> dmem[OOO_DMEM_WORDS] = {0};
    dmem[0] = 10; dmem[1] = 20; dmem[2] = 30; dmem[3] = 40; // v1
    dmem[4] = 1;  dmem[5] = 2;  dmem[6] = 3;  dmem[7] = 4;  // v2

    const uint32_t OP_IMM = rv32i::Opcode::OP_IMM;
    const uint32_t STORE  = rv32i::Opcode::STORE;
    std::vector<uint16_t> prog;
    auto push32 = [&](uint32_t w) { prog.push_back(w & 0xFFFF); prog.push_back(w >> 16); };
    auto push16 = [&](uint16_t h) { prog.push_back(h); };

    push32(enc_vsetvli(1, 0, VTYPE_E32_M1));                       // vl=4
    push32(enc_vec_mem(rv32i::Opcode::LOAD_FP, 1, 0));             // vle32.v v1,(x0) -> {10,20,30,40}
    push32(i_type(OP_IMM, 3, F3A::ADD_SUB, 0, 16));                // x3 = 16
    push32(enc_vec_mem(rv32i::Opcode::LOAD_FP, 2, 3));             // vle32.v v2,(x3) -> {1,2,3,4}
    // 4a: reducciones
    push32(enc_op_v(F6_VREDSUM, 1, 2, rvv_f3_opmvv, 6));           // v6[0]=1+100=101
    push32(enc_op_v(F6_VREDMAX, 1, 2, rvv_f3_opmvv, 7));           // v7[0]=40
    push32(enc_op_v(F6_VREDAND, 1, 1, rvv_f3_opmvv, 8));           // v8[0]=0
    // 4b: permutaciones
    push32(enc_op_v(F6_VMERGE, 0, 2, rvv_f3_opivv, 9));            // vmv.v.v v9,v2
    push32(enc_op_v(F6_VSLIDEUP, 1, 1, F3_OPIVI, 9));              // v9={1,10,20,30}
    push32(enc_op_v(F6_VSLIDEDOWN, 1, 1, F3_OPIVI, 10));           // v10={20,30,40,0}
    push32(enc_op_v(F6_VRGATHER, 1, 2, F3_OPIVI, 11));             // v11={30,30,30,30}
    push32(i_type(OP_IMM, 4, F3A::ADD_SUB, 0, 0b0101));            // x4=5
    push32(i_type(OP_IMM, 5, F3A::ADD_SUB, 0, 48));                // x5=48 (dmem[12])
    push32(s_type(STORE, rv32i::Funct3_STORE::SW, 5, 4, 0));       // sw x4,0(x5)
    push32(enc_vec_mem(rv32i::Opcode::LOAD_FP, 0, 5));             // vle32.v v0,(x5) -> mascara
    push32(enc_op_v(F6_VCOMPRESS, 1, 0, rvv_f3_opmvv, 12));        // v12={10,30}
    // 4c: punto fijo
    push32(i_type(OP_IMM, 6, F3A::ADD_SUB, 0, -1));                // x6=0xFFFFFFFF
    push32(enc_op_v(F6_VSADDU, 1, 6, F3_OPIVX, 13));               // satura al maximo
    push32(i_type(OP_IMM, 7, F3A::ADD_SUB, 0, 10));                // x7=10
    push32(enc_op_v(F6_VSSUBU, 2, 7, F3_OPIVX, 14));               // satura a 0
    push32(enc_op_v(F6_VAADD, 1, 2, rvv_f3_opmvv, 15));            // promedio redondeado
    push16(0x0000);

    for (size_t i = 0; i < prog.size(); i++) {
        uint32_t w = imem[i / 2].to_uint();
        if (i % 2 == 0) w = (w & 0xFFFF0000u) | prog[i];
        else            w = (w & 0x0000FFFFu) | (uint32_t(prog[i]) << 16);
        imem[i / 2] = w;
    }

    ap_uint<1> dv; ap_uint<3> dt; ap_uint<32> dp;
    ap_uint<1> a0d,a1d,mdd,fpd,lsd,brd,vcd; ap_uint<3> a0t,a1t,mdt,fpt,lst,brt,vct;
    ap_uint<1> cv, cfp; ap_uint<5> crd; ap_uint<32> cval;
    static ap_uint<32> vo[OOO_VEC_REGFILE_LEN];
    ap_uint<1> halted;
    riscv_soc_tick(1, imem, dmem, dv,dt,dp, a0d,a0t,a1d,a1t,mdd,mdt, fpd,fpt,
                  lsd,lst, brd,brt, vcd,vct, cv,cfp,crd,cval, vo, halted);
    int cycle = 0;
    while (!halted && cycle < 2000) {
        cycle++;
        riscv_soc_tick(0, imem, dmem, dv,dt,dp, a0d,a0t,a1d,a1t,mdd,mdt, fpd,fpt,
                      lsd,lst, brd,brt, vcd,vct, cv,cfp,crd,cval, vo, halted);
    }
    printf("      (termino en %d ciclos)\n", cycle);

    struct { int vreg, lane; uint32_t expect; const char* what; } cks[] = {
        {6,0,101, "vredsum.vs: v2[0]=1 + sum(v1)=100"},
        {7,0,40,  "vredmax.vs: max(1, 10,20,30,40)"},
        {8,0,0,   "vredand.vs: 10 & 10&20&30&40"},
        {9,0,1,  "vslideup.vi: el elemento 0 queda SIN TOCAR"},
        {9,1,10, "vslideup.vi: v9[1] = v1[0]"},
        {9,2,20, "vslideup.vi: v9[2] = v1[1]"},
        {9,3,30, "vslideup.vi: v9[3] = v1[2]"},
        {10,0,20,"vslidedown.vi: v10[0] = v1[1]"},
        {10,1,30,"vslidedown.vi"},
        {10,2,40,"vslidedown.vi"},
        {10,3,0, "vslidedown.vi: mas alla del vector -> 0"},
        {11,0,30,"vrgather.vi: todos toman v1[2]"},
        {11,3,30,"vrgather.vi"},
        {12,0,10,"vcompress.vm: empaqueta v1[0] (mascara 0b0101)"},
        {12,1,30,"vcompress.vm: y v1[2], CONSECUTIVOS"},
        {13,0,0xFFFFFFFF,"vsaddu.vx: 10+0xFFFFFFFF SATURA al maximo"},
        {13,3,0xFFFFFFFF,"vsaddu.vx satura"},
        {14,0,0,  "vssubu.vx: 1-10 SATURA a 0 (no envuelve)"},
        {14,3,0,  "vssubu.vx satura"},
        {15,0,6,  "vaadd.vv: (10+1)/2 con redondeo = 6"},
        {15,1,11, "vaadd.vv: (20+2)/2 = 11"},
        {15,2,17, "vaadd.vv: (30+3)/2 con redondeo = 17"},
        {15,3,22, "vaadd.vv: (40+4)/2 = 22"},
    };
    int fails = 0;
    for (auto& c : cks) {
        uint32_t got = vo[c.vreg * OOO_VEC_LANES + c.lane].to_uint();
        if (got != c.expect) {
            printf("FAIL  v%d[%d] = 0x%08x, esperado 0x%08x (%s)\n",
                   c.vreg, c.lane, got, c.expect, c.what);
            fails++;
        } else {
            printf("OK    v%d[%d] = 0x%08x (%s)\n", c.vreg, c.lane, got, c.what);
        }
    }
    return fails;
}

// ---------------------------------------------------------------------
// Suite A3: RVV Fase 5 -- los modos de direccionamiento de memoria.
//
// Codificacion de un load/store vectorial (seccion 7.1 de la spec):
//   nf[31:29] mew[28] mop[27:26] vm[25] lumop/rs2/vs2[24:20]
//   rs1[19:15] width[14:12] vd|vs3[11:7] opcode[6:0]
// ---------------------------------------------------------------------
static ap_uint<32> enc_vmem(uint32_t opcode, uint32_t vd_or_vs3, uint32_t rs1,
                            uint32_t width, uint32_t mop, uint32_t f24_20,
                            uint32_t nf = 0, uint32_t vm = 1) {
    return (nf << 29) | (0u << 28) | (mop << 26) | (vm << 25) | (f24_20 << 20) |
           (rs1 << 15) | (width << 12) | (vd_or_vs3 << 7) | opcode;
}
static const uint32_t MOP_UNIT = 0b00, MOP_IDX_U = 0b01, MOP_STRIDED = 0b10;
static const uint32_t LUMOP_UNIT = 0b00000, LUMOP_WHOLE = 0b01000;
static const uint32_t LUMOP_MASK = 0b01011, LUMOP_FOF = 0b10000;
static const uint32_t WIDTH_32t = 0b110, WIDTH_64t = 0b111;

static int suite_fase5() {
    ap_uint<32> imem[OOO_IMEM_WORDS] = {0};
    ap_uint<32> dmem[OOO_DMEM_WORDS] = {0};
    // Datos fuente: 8 palabras consecutivas desde el byte 0.
    for (int i = 0; i < 8; i++) dmem[i] = 10 * (i + 1);   // 10..80
    // Offsets en BYTES para el modo indexado, en el byte 64 (dmem[16]):
    // {12,8,4,0} -> lee el vector AL REVES.
    dmem[16] = 12; dmem[17] = 8; dmem[18] = 4; dmem[19] = 0;
    // Pares intercalados para el load segmentado, en el byte 128 (dmem[32]):
    // (1,100) (2,200) (3,300) (4,400) -- como un arreglo de structs.
    for (int i = 0; i < 4; i++) { dmem[32 + 2*i] = i + 1; dmem[33 + 2*i] = 100 * (i + 1); }
    // Al FINAL de la dmem, para el fault-only-first: solo dos palabras
    // validas; el tercer elemento ya se sale del rango fisico.
    dmem[OOO_DMEM_WORDS - 2] = 0xAAAA; dmem[OOO_DMEM_WORDS - 1] = 0xBBBB;

    const uint32_t OP_IMM = rv32i::Opcode::OP_IMM;
    const uint32_t LUI    = rv32i::Opcode::LUI;
    const uint32_t LDFP   = rv32i::Opcode::LOAD_FP;
    const uint32_t STFP   = rv32i::Opcode::STORE_FP;
    std::vector<uint16_t> prog;
    auto push32 = [&](uint32_t w) { prog.push_back(w & 0xFFFF); prog.push_back(w >> 16); };
    auto push16 = [&](uint16_t h) { prog.push_back(h); };

    push32(enc_vsetvli(1, 0, VTYPE_E32_M1));                     // vl=VLMAX=4

    // ---- 1. STRIDED: paso de 8 bytes -> toma una palabra SI y una NO ----
    push32(i_type(OP_IMM, 2, F3A::ADD_SUB, 0, 8));               // x2 = paso 8B
    push32(enc_vmem(LDFP, 1, 0, WIDTH_32t, MOP_STRIDED, 2));     // vlse32.v v1,(x0),x2
    // guardar con el mismo paso en el byte 256 y releer contiguo: si el
    // store strided funciona, la relectura contigua NO da lo mismo.
    push32(i_type(OP_IMM, 3, F3A::ADD_SUB, 0, 256));             // x3 = 256
    push32(enc_vmem(STFP, 1, 3, WIDTH_32t, MOP_STRIDED, 2));     // vsse32.v v1,(x3),x2
    push32(enc_vmem(LDFP, 2, 3, WIDTH_32t, MOP_UNIT, LUMOP_UNIT));// vle32.v v2,(x3)

    // ---- 2. INDEXADO: los offsets salen de un registro VECTORIAL ----
    push32(i_type(OP_IMM, 4, F3A::ADD_SUB, 0, 64));              // x4 = 64
    push32(enc_vmem(LDFP, 4, 4, WIDTH_32t, MOP_UNIT, LUMOP_UNIT));// vle32.v v4,(x4) = {12,8,4,0}
    push32(enc_vmem(LDFP, 3, 0, WIDTH_32t, MOP_IDX_U, 4));       // vluxei32.v v3,(x0),v4
    // dispersion (scatter) al byte 320 y relectura contigua
    push32(i_type(OP_IMM, 5, F3A::ADD_SUB, 0, 320));             // x5 = 320
    push32(enc_vmem(STFP, 1, 5, WIDTH_32t, MOP_IDX_U, 4));       // vsuxei32.v v1,(x5),v4
    push32(enc_vmem(LDFP, 5, 5, WIDTH_32t, MOP_UNIT, LUMOP_UNIT));// vle32.v v5,(x5)

    // ---- 3. SEGMENTADO: separa un arreglo de pares en dos vectores ----
    push32(i_type(OP_IMM, 6, F3A::ADD_SUB, 0, 128));             // x6 = 128
    push32(enc_vmem(LDFP, 6, 6, WIDTH_32t, MOP_UNIT, LUMOP_UNIT, /*nf=*/1)); // vlseg2e32.v v6,(x6)

    // ---- 4. REGISTRO COMPLETO: no mira vl (se prueba con vl=0) ----
    push32(i_type(OP_IMM, 7, F3A::ADD_SUB, 0, 384));             // x7 = 384
    push32(enc_vmem(STFP, 1, 7, WIDTH_32t, MOP_UNIT, LUMOP_WHOLE)); // vs1r.v v1,(x7)
    push32(i_type(OP_IMM, 8, F3A::ADD_SUB, 0, 0));               // x8 = 0  -> AVL=0
    push32(enc_vsetvli(9, 8, VTYPE_E32_M1));                     // vsetvli x9,x8 -> vl=0
    push32(enc_vmem(LDFP, 8, 7, WIDTH_32t, MOP_UNIT, LUMOP_WHOLE)); // vl1r.v v8,(x7) CON vl=0
    push32(enc_vsetvli(9, 0, VTYPE_E32_M1));                     // restaura vl=4

    // ---- 5. MASCARA: vsm.v / vlm.v mueven bits, no elementos ----
    push32(i_type(OP_IMM, 10, F3A::ADD_SUB, 0, 0b1010));         // x10 = 0b1010
    push32(i_type(OP_IMM, 11, F3A::ADD_SUB, 0, 448));            // x11 = 448
    push32(s_type(rv32i::Opcode::STORE, rv32i::Funct3_STORE::SW, 11, 10, 0));
    push32(enc_vmem(LDFP, 12, 11, 0b000, MOP_UNIT, LUMOP_MASK)); // vlm.v v12,(x11)
    push32(i_type(OP_IMM, 12, F3A::ADD_SUB, 0, 512));            // x12 = 512
    push32(enc_vmem(STFP, 12, 12, 0b000, MOP_UNIT, LUMOP_MASK)); // vsm.v v12,(x12)
    push32(enc_vmem(LDFP, 13, 12, WIDTH_32t, MOP_UNIT, LUMOP_UNIT)); // relee como palabra

    // ---- 6. FAULT-ONLY-FIRST: recorta vl en vez de atrapar ----
    // base = ultimas 2 palabras de la dmem; los elementos 2 y 3 se salen.
    push32(u_type(LUI, 14, (OOO_DMEM_WORDS * 4) >> 12));         // x14 = 64KB
    push32(i_type(OP_IMM, 14, F3A::ADD_SUB, 14, -8));            // x14 = 64KB-8
    push32(enc_vmem(LDFP, 14, 14, WIDTH_32t, MOP_UNIT, LUMOP_FOF)); // vle32ff.v v14,(x14)
    push32(i_type(rv32i::Opcode::SYSTEM, 15, rv32i::Funct3_SYSTEM::CSRRS,
                  0, rv32i::CSR::VL));                           // csrr x15, vl
    push32(enc_vsetvli(16, 0, VTYPE_E32_M1));                    // restaura vl=4
    push32(enc_op_v(F6_VMERGE, 0, 15, F3_OPIVX, 16));            // vmv.v.x v16,x15

    // ---- 7. EEW=64 esta FUERA de Zve32x -> instruccion ilegal ----
    // Sin mtvec instalado el core se detiene: la prueba es que la
    // instruccion que sigue NO llegue a escribir su registro.
    push32(enc_vmem(LDFP, 20, 0, WIDTH_64t, MOP_UNIT, LUMOP_UNIT)); // vle64.v -> ILEGAL
    push32(enc_op_v(F6_VMERGE, 0, 1, F3_OPIVX, 20));             // no deberia ejecutarse
    push16(0x0000);

    for (size_t i = 0; i < prog.size(); i++) {
        uint32_t w = imem[i / 2].to_uint();
        if (i % 2 == 0) w = (w & 0xFFFF0000u) | prog[i];
        else            w = (w & 0x0000FFFFu) | (uint32_t(prog[i]) << 16);
        imem[i / 2] = w;
    }

    ap_uint<1> dv; ap_uint<3> dt; ap_uint<32> dp;
    ap_uint<1> a0d,a1d,mdd,fpd,lsd,brd,vcd; ap_uint<3> a0t,a1t,mdt,fpt,lst,brt,vct;
    ap_uint<1> cv, cfp; ap_uint<5> crd; ap_uint<32> cval;
    static ap_uint<32> vo[OOO_VEC_REGFILE_LEN];
    ap_uint<1> halted;
    riscv_soc_tick(1, imem, dmem, dv,dt,dp, a0d,a0t,a1d,a1t,mdd,mdt, fpd,fpt,
                  lsd,lst, brd,brt, vcd,vct, cv,cfp,crd,cval, vo, halted);
    int cycle = 0;
    while (!halted && cycle < 3000) {
        cycle++;
        riscv_soc_tick(0, imem, dmem, dv,dt,dp, a0d,a0t,a1d,a1t,mdd,mdt, fpd,fpt,
                      lsd,lst, brd,brt, vcd,vct, cv,cfp,crd,cval, vo, halted);
    }
    printf("      (termino en %d ciclos)\n", cycle);

    struct { int vreg, lane; uint32_t expect; const char* what; } cks[] = {
        {1,0,10, "vlse32.v paso=8: v1[0]=mem[0]"},
        {1,1,30, "vlse32.v paso=8: v1[1]=mem[8], SALTEA una palabra"},
        {1,2,50, "vlse32.v paso=8"},
        {1,3,70, "vlse32.v paso=8"},
        {2,0,10, "vsse32.v: releido contiguo, v2[0]"},
        {2,1,0,  "vsse32.v: el hueco del paso quedo SIN escribir"},
        {2,2,30, "vsse32.v: v2[2] es el segundo elemento guardado"},
        {2,3,0,  "vsse32.v: segundo hueco intacto"},
        {3,0,40, "vluxei32.v offsets {12,8,4,0}: invierte el vector"},
        {3,1,30, "vluxei32.v"},
        {3,2,20, "vluxei32.v"},
        {3,3,10, "vluxei32.v"},
        {5,0,70, "vsuxei32.v: scatter, v1[3]=70 fue al offset 0"},
        {5,1,50, "vsuxei32.v scatter"},
        {5,2,30, "vsuxei32.v scatter"},
        {5,3,10, "vsuxei32.v scatter"},
        {6,0,1,  "vlseg2e32.v: campo 0 -> v6 (los 'x' de los pares)"},
        {6,1,2,  "vlseg2e32.v campo 0"},
        {6,3,4,  "vlseg2e32.v campo 0"},
        {7,0,100,"vlseg2e32.v: campo 1 -> v7 (los 'y'), DESINTERCALADO"},
        {7,1,200,"vlseg2e32.v campo 1"},
        {7,3,400,"vlseg2e32.v campo 1"},
        {8,0,10, "vl1r.v con vl=0: copia el registro COMPLETO igual"},
        {8,3,70, "vl1r.v ignora vl (el elemento 3 tambien se copio)"},
        {12,0,0b1010, "vlm.v: carga la mascara como bytes"},
        {13,0,0b1010, "vsm.v: y la vuelve a guardar identica"},
        {14,0,0xAAAA, "vle32ff.v: el elemento 0 SI se cargo"},
        {14,1,0xBBBB, "vle32ff.v: el elemento 1 tambien"},
        {16,0,2, "vle32ff.v RECORTO vl a 2 en vez de atrapar"},
        {20,0,0, "vle64.v (EEW=64, fuera de Zve32x) atrapo: v20 sin escribir"},
    };
    int fails = 0;
    for (auto& c : cks) {
        uint32_t got = vo[c.vreg * OOO_VEC_LANES + c.lane].to_uint();
        if (got != c.expect) {
            printf("FAIL  v%d[%d] = 0x%08x, esperado 0x%08x (%s)\n",
                   c.vreg, c.lane, got, c.expect, c.what);
            fails++;
        } else {
            printf("OK    v%d[%d] = 0x%08x (%s)\n", c.vreg, c.lane, got, c.what);
        }
    }
    return fails;
}

// ---------------------------------------------------------------------
// Suite A4: RVV Fase 4d (widening/narrowing) + Fase 6 (vstart).
// Se corre con SEW=16 porque ensanchar exige 2*SEW <= ELEN = 32.
// ---------------------------------------------------------------------
static const uint32_t F6_VWADDU = 0b110000, F6_VWADD = 0b110001;
static const uint32_t F6_VWSUB  = 0b110011, F6_VWADD_W = 0b110101;
static const uint32_t F6_VWMULU = 0b111000, F6_VWMUL = 0b111011;
static const uint32_t F6_VWMACC = 0b111101;
static const uint32_t F6_VNSRL  = 0b101100, F6_VNSRA = 0b101101;
static const uint32_t F6_VNCLIPU= 0b101110, F6_VNCLIP = 0b101111;
static const uint32_t F3_OPMVX_t = 0b110;
static const uint32_t F6_VADD_t = 0b000000;

static int suite_fase4d_vstart() {
    ap_uint<32> imem[OOO_IMEM_WORDS] = {0};
    ap_uint<32> dmem[OOO_DMEM_WORDS] = {0};
    // 8 elementos de 16 bits desde el byte 0: 1,2,3,4,5,6,7,8
    for (int i = 0; i < 4; i++) dmem[i] = (uint32_t(2*i+2) << 16) | uint32_t(2*i+1);
    // 8 elementos de 16 bits desde el byte 32 (dmem[8]): 100..800
    for (int i = 0; i < 4; i++) dmem[8+i] = (uint32_t(200*(i+1)) << 16) | uint32_t(100*(2*i+1));
    // fuente ANCHA (32b) para narrowing, byte 64 (dmem[16]): valores grandes
    dmem[16] = 0x00012345; dmem[17] = 0x0007FFFF;
    dmem[18] = 0xFFFF0000; dmem[19] = 0x00000FF0;

    const uint32_t OP_IMM = rv32i::Opcode::OP_IMM;
    std::vector<uint16_t> prog;
    auto push32 = [&](uint32_t w) { prog.push_back(w & 0xFFFF); prog.push_back(w >> 16); };
    auto push16 = [&](uint16_t h) { prog.push_back(h); };

    // SEW=16 -> VLMAX = 128/16 = 8 elementos
    push32(enc_vsetvli(1, 0, VTYPE_E16_M1));
    push32(enc_vec_mem_w(rv32i::Opcode::LOAD_FP, 2, 0, WIDTH_16));   // v2 = {1..8}
    push32(i_type(OP_IMM, 3, F3A::ADD_SUB, 0, 32));
    push32(enc_vec_mem_w(rv32i::Opcode::LOAD_FP, 4, 3, WIDTH_16));   // v4 = {100,200,...}

    // ---- 4d widening: el destino ocupa el PAR (v6,v7) ----
    push32(enc_op_v(F6_VWADDU, 2, 4, 0b010, 6));                     // vwaddu.vv v6,v2,v4
    push32(enc_op_v(F6_VWMULU, 2, 4, 0b010, 8));                     // vwmulu.vv v8,v2,v4
    push32(i_type(OP_IMM, 5, F3A::ADD_SUB, 0, -1));                  // x5 = 0xFFFFFFFF
    push32(enc_op_v(F6_VWADDU, 2, 5, F3_OPMVX_t, 10));               // vwaddu.vx: zero-extiende
    push32(enc_op_v(F6_VWADD,  2, 5, F3_OPMVX_t, 12));               // vwadd.vx : SIGNO -> -1
    // multiply-accumulate: acumula sobre el par (v8,v9) ya cargado
    push32(enc_op_v(F6_VWMACC, 2, 4, 0b010, 8));                     // v8 += v2*v4

    // ---- 4d narrowing: la fuente v16 es ANCHA (2*SEW=32) ----
    push32(i_type(OP_IMM, 6, F3A::ADD_SUB, 0, 64));
    push32(enc_vsetvli(7, 0, VTYPE_E32_M1));                         // vl=4 para cargar 32b
    push32(enc_vec_mem(rv32i::Opcode::LOAD_FP, 16, 6));              // v16 = 4 palabras
    push32(enc_vsetvli(7, 0, VTYPE_E16_M1));                         // vuelve a SEW=16, vl=8
    push32(enc_op_v(F6_VNSRL, 16, 4, F3_OPIVI, 18));                 // vnsrl.wi v18,v16,4
    push32(enc_op_v(F6_VNCLIPU, 16, 0, F3_OPIVI, 19));               // vnclipu.wi v19,v16,0 -> SATURA
    push32(enc_op_v(F6_VNSRA, 16, 4, F3_OPIVI, 20));                 // vnsra.wi (con signo)

    // ---- Fase 6: vstart ----
    // se escribe vstart=3 y se hace un vadd: los elementos 0..2 quedan
    // SIN TOCAR y solo se calculan del 3 en adelante.
    push32(enc_op_v(F6_VMERGE, 0, 2, rvv_f3_opivv, 21));             // vmv.v.v v21,v2 (centinela)
    push32(i_type(OP_IMM, 8, F3A::ADD_SUB, 0, 3));                   // x8 = 3
    push32(i_type(rv32i::Opcode::SYSTEM, 0, rv32i::Funct3_SYSTEM::CSRRW,
                  8, rv32i::CSR::VSTART));                           // csrw vstart, x8
    push32(enc_op_v(F6_VADD_t, 2, 4, rvv_f3_opivv, 21));             // vadd.vv v21,v2,v4
    // tras completar, vstart DEBE haber vuelto a 0: el siguiente vadd
    // escribe TODOS los elementos.
    push32(enc_op_v(F6_VADD_t, 2, 4, rvv_f3_opivv, 22));             // vadd.vv v22,v2,v4
    // y se lee vstart para comprobar que quedo en 0
    push32(i_type(rv32i::Opcode::SYSTEM, 9, rv32i::Funct3_SYSTEM::CSRRS,
                  0, rv32i::CSR::VSTART));                           // csrr x9, vstart
    push32(enc_op_v(F6_VMERGE, 0, 9, F3_OPIVX, 23));                 // vmv.v.x v23,x9

    push16(0x0000);

    for (size_t i = 0; i < prog.size(); i++) {
        uint32_t w = imem[i / 2].to_uint();
        if (i % 2 == 0) w = (w & 0xFFFF0000u) | prog[i];
        else            w = (w & 0x0000FFFFu) | (uint32_t(prog[i]) << 16);
        imem[i / 2] = w;
    }

    ap_uint<1> dv; ap_uint<3> dt; ap_uint<32> dp;
    ap_uint<1> a0d,a1d,mdd,fpd,lsd,brd,vcd; ap_uint<3> a0t,a1t,mdt,fpt,lst,brt,vct;
    ap_uint<1> cv, cfp; ap_uint<5> crd; ap_uint<32> cval;
    static ap_uint<32> vo[OOO_VEC_REGFILE_LEN];
    ap_uint<1> halted;
    riscv_soc_tick(1, imem, dmem, dv,dt,dp, a0d,a0t,a1d,a1t,mdd,mdt, fpd,fpt,
                  lsd,lst, brd,brt, vcd,vct, cv,cfp,crd,cval, vo, halted);
    int cycle = 0;
    while (!halted && cycle < 3000) {
        cycle++;
        riscv_soc_tick(0, imem, dmem, dv,dt,dp, a0d,a0t,a1d,a1t,mdd,mdt, fpd,fpt,
                      lsd,lst, brd,brt, vcd,vct, cv,cfp,crd,cval, vo, halted);
    }
    printf("      (termino en %d ciclos)\n", cycle);

    // helpers para leer elementos de 16 y de 32 bits del banco vectorial
    auto e16 = [&](int vreg, int idx) -> uint32_t {
        uint32_t w = vo[vreg * OOO_VEC_LANES + idx/2].to_uint();
        return (idx % 2) ? (w >> 16) : (w & 0xFFFF);
    };
    // elemento ANCHO (32b) del par que empieza en `vreg`
    auto e32p = [&](int vreg, int idx) -> uint32_t {
        return vo[(vreg + idx/4) * OOO_VEC_LANES + (idx % 4)].to_uint();
    };

    struct { uint32_t got, expect; const char* what; } cks[] = {
        // widening: 2*SEW = SEW op SEW, destino en el PAR (v6,v7)
        {e32p(6,0), 101u,   "vwaddu.vv: 1+100 en 32b, elemento 0 del par"},
        {e32p(6,3), 404u,   "vwaddu.vv: 4+400"},
        {e32p(6,4), 505u,   "vwaddu.vv: elemento 4 YA ESTA EN v7 (EMUL=2)"},
        {e32p(6,7), 808u,   "vwaddu.vv: ultimo elemento, en v7"},
        // producto ensanchado: 8*800=6400 NO cabria en 16b sin ensanchar
        {e32p(8,7), 6400u*2u, "vwmacc: v8 = 8*800 (vwmulu) + 8*800 (vwmacc)"},
        // .vx con x5=0xFFFFFFFF: zero-extiende vs UNO con signo
        {e32p(10,0), 1u + 0xFFFFu, "vwaddu.vx: el escalar se toma de 16b SIN signo"},
        {e32p(12,0), 0u,           "vwadd.vx: el MISMO escalar, CON signo, es -1 -> 1-1=0"},
        // narrowing: fuente de 32b -> destino de 16b
        {e16(18,0), 0x1234u, "vnsrl.wi 4: 0x00012345 >> 4, truncado a 16b"},
        {e16(18,1), 0x7FFFu, "vnsrl.wi 4: 0x0007FFFF >> 4 = 0x7FFF"},
        {e16(19,0), 0xFFFFu, "vnclipu.wi 0: 0x12345 NO cabe en 16b -> SATURA"},
        {e16(20,2), 0xF000u, "vnsra.wi 4: 0xFFFF0000 con signo"},
        // vstart: los elementos anteriores quedan intactos
        {e16(21,0), 1u,   "vstart=3: el elemento 0 quedo SIN TOCAR (centinela v2)"},
        {e16(21,2), 3u,   "vstart=3: el elemento 2 tampoco se toco"},
        {e16(21,3), 4u+400u, "vstart=3: el elemento 3 SI se calculo"},
        {e16(21,7), 8u+800u, "vstart=3: y del 3 en adelante tambien"},
        {e16(22,0), 1u+100u, "vstart volvio a 0: el siguiente vadd escribe TODO"},
        {e16(23,0), 0u,   "csrr vstart = 0 tras completar la vectorial"},
    };
    int fails = 0;
    for (auto& c : cks) {
        if (c.got != c.expect) {
            printf("FAIL  0x%08x, esperado 0x%08x (%s)\n", c.got, c.expect, c.what);
            fails++;
        } else {
            printf("OK    0x%08x (%s)\n", c.got, c.what);
        }
    }
    return fails;
}


// ---------------------------------------------------------------------
// Suite E: el predictor TAGE del frontend.
//
// Dos lazos:
//  1) un lazo simple de 32 iteraciones (backward branch tomado 31 veces)
//     -- cualquier predictor decente lo aprende en un par de intentos;
//  2) un branch ALTERNANTE (tomado, no-tomado, tomado, ...) dentro del
//     lazo: un bimodal puro falla ~50% de las veces PARA SIEMPRE, pero
//     TAGE lo aprende con un solo bit de historia global. Esa es la
//     diferencia que justifica el predictor del diagrama.
// ---------------------------------------------------------------------
static int suite_tage() {
    ap_uint<32> imem[OOO_IMEM_WORDS] = {0};
    ap_uint<32> dmem[OOO_DMEM_WORDS] = {0};
    const uint32_t OP_IMM = rv32i::Opcode::OP_IMM;
    const uint32_t OP     = rv32i::Opcode::OP;
    std::vector<uint16_t> prog;
    auto push32 = [&](uint32_t w) { prog.push_back(w & 0xFFFF); prog.push_back(w >> 16); };
    auto push16 = [&](uint16_t h) { prog.push_back(h); };

    //  0: addi x1,x0,0      contador del lazo
    //  4: addi x2,x0,32     limite
    //  8: addi x4,x0,0      acumulador del camino "par"
    // 12: (loop) addi x1,x1,1
    // 16: andi x3,x1,1
    // 20: bne  x3,x0,+8     ALTERNA cada iteracion: T,N,T,N,...
    // 24: addi x4,x4,1      (solo iteraciones pares)
    // 28: blt  x1,x2,-16    backward: tomado 31 veces, no-tomado 1
    // 32: sw   x1,256(x0)
    // 36: sw   x4,260(x0)
    push32(i_type(OP_IMM, 1, F3A::ADD_SUB, 0, 0));
    push32(i_type(OP_IMM, 2, F3A::ADD_SUB, 0, 32));
    push32(i_type(OP_IMM, 4, F3A::ADD_SUB, 0, 0));
    push32(i_type(OP_IMM, 1, F3A::ADD_SUB, 1, 1));
    push32(i_type(OP_IMM, 3, F3A::AND, 1, 1));
    push32(b_type(rv32i::Funct3_BRANCH::BNE, 3, 0, 8));
    push32(i_type(OP_IMM, 4, F3A::ADD_SUB, 4, 1));
    push32(b_type(rv32i::Funct3_BRANCH::BLT, 1, 2, -16));
    push32(s_type(rv32i::Opcode::STORE, rv32i::Funct3_STORE::SW, 0, 1, 256));
    push32(s_type(rv32i::Opcode::STORE, rv32i::Funct3_STORE::SW, 0, 4, 260));
    push16(0x0000);

    for (size_t i = 0; i < prog.size(); i++) {
        uint32_t w = imem[i / 2].to_uint();
        if (i % 2 == 0) w = (w & 0xFFFF0000u) | prog[i];
        else            w = (w & 0x0000FFFFu) | (uint32_t(prog[i]) << 16);
        imem[i / 2] = w;
    }

    ap_uint<1> dv; ap_uint<3> dt; ap_uint<32> dp;
    ap_uint<1> a0d,a1d,mdd,fpd,lsd,brd,vcd; ap_uint<3> a0t,a1t,mdt,fpt,lst,brt,vct;
    ap_uint<1> cv, cfp; ap_uint<5> crd; ap_uint<32> cval;
    static ap_uint<32> vo[OOO_VEC_REGFILE_LEN];
    ap_uint<1> halted;
    riscv_soc_tick(1, imem, dmem, dv,dt,dp, a0d,a0t,a1d,a1t,mdd,mdt, fpd,fpt,
                  lsd,lst, brd,brt, vcd,vct, cv,cfp,crd,cval, vo, halted);
    int cycle = 0;
    while (!halted && cycle < 3000) {
        cycle++;
        riscv_soc_tick(0, imem, dmem, dv,dt,dp, a0d,a0t,a1d,a1t,mdd,mdt, fpd,fpt,
                      lsd,lst, brd,brt, vcd,vct, cv,cfp,crd,cval, vo, halted);
    }
    uint32_t br = soc_stat_branches(), mp = soc_stat_mispredicts();
    printf("      (%d ciclos; %u branches, %u mispredicts -> %.1f%% acierto)\n",
           cycle, br, mp, br ? 100.0 * (br - mp) / br : 0.0);

    int fails = 0;
    struct { bool cond; const char* what; } cks[] = {
        {dmem[64].to_uint() == 32, "el lazo ejecuto sus 32 iteraciones BAJO especulacion"},
        {dmem[65].to_uint() == 16, "el camino alternante tomo exactamente los 16 pares"},
        {br == 64,                 "64 branches condicionales retirados (32+32)"},
        // El branch alternante mata a un bimodal (~32 fallos de 64). Si
        // TAGE aprendio el patron con su historia, el total queda muy por
        // debajo. El umbral 16 deja margen al arranque en frio.
        {mp <= 16,                 "TAGE aprendio: mispredicts <= 16 de 64 (bimodal: ~32)"},
    };
    for (auto& c : cks) {
        if (c.cond) { printf("OK    %s\n", c.what); }
        else        { printf("FAIL  %s\n", c.what); fails++; }
    }
    return fails;
}

// ---------------------------------------------------------------------
// Carga de binarios ELF32 (comun a las suites B, C y D)
// ---------------------------------------------------------------------
struct Elf32_Ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};
struct Elf32_Phdr {
    uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align;
};

// Codifica "jal x0, offset". El inmediato J-type va troceado y
// reordenado, por eso se arma por partes.
static uint32_t enc_jal_x0(int32_t off) {
    uint32_t i = static_cast<uint32_t>(off);
    return (((i >> 20) & 1) << 31) | (((i >> 1) & 0x3FF) << 21) |
           (((i >> 11) & 1) << 20) | (((i >> 12) & 0xFF) << 12) | 0x6Fu;
}

static bool load_elf(const unsigned char* img, unsigned len,
                     ap_uint<32> imem[OOO_IMEM_WORDS],
                     ap_uint<32> dmem[OOO_DMEM_WORDS]) {
    if (len < sizeof(Elf32_Ehdr)) return false;
    Elf32_Ehdr eh; std::memcpy(&eh, img, sizeof(eh));
    if (!(eh.e_ident[0] == 0x7f && eh.e_ident[1] == 'E')) return false;
    for (int i = 0; i < eh.e_phnum; i++) {
        Elf32_Phdr ph;
        std::memcpy(&ph, img + eh.e_phoff + i * eh.e_phentsize, sizeof(ph));
        if (ph.p_type != 1) continue; // PT_LOAD
        // El binario usa un espacio de direcciones UNIFICADO pero el core
        // es Harvard: cada segmento va a AMBAS memorias (el codigo se
        // busca en imem; las constantes/.data que el codigo lee quedan
        // accesibles en dmem con la misma direccion).
        for (uint32_t b = 0; b < ph.p_filesz; b++) {
            uint32_t va = ph.p_vaddr + b, wi = va >> 2, sh = (va & 3) * 8;
            uint8_t by = img[ph.p_offset + b];
            if (wi < OOO_IMEM_WORDS)
                imem[wi] = (imem[wi].to_uint() & ~(0xFFu << sh)) | (uint32_t(by) << sh);
            if (wi < OOO_DMEM_WORDS)
                dmem[wi] = (dmem[wi].to_uint() & ~(0xFFu << sh)) | (uint32_t(by) << sh);
        }
    }
    // El core arranca siempre en pc=0, pero el entry point del ELF puede
    // estar en otra direccion. Se instala un STUB DE ARRANQUE en 0 que
    // salta al entry -- lo mismo que hace el vector de reset de un core
    // real antes de entregar el control al programa.
    if (eh.e_entry != 0) imem[0] = enc_jal_x0(static_cast<int32_t>(eh.e_entry));
    return true;
}

// Corre un binario ELF hasta que se detiene solo. Devuelve el numero de
// ciclos, o -1 si no termino dentro del limite.
static long run_elf(const unsigned char* img, unsigned len,
                    ap_uint<32> dmem_out[OOO_DMEM_WORDS], long max_cycles) {
    static ap_uint<32> imem[OOO_IMEM_WORDS];
    static ap_uint<32> dmem[OOO_DMEM_WORDS];
    for (int i = 0; i < OOO_IMEM_WORDS; i++) imem[i] = 0;
    for (int i = 0; i < OOO_DMEM_WORDS; i++) dmem[i] = 0;
    if (!load_elf(img, len, imem, dmem)) return -1;

    ap_uint<1> dv; ap_uint<3> dt; ap_uint<32> dp;
    ap_uint<1> a0d, a1d, mdd, fpd, lsd, brd, vcd;
    ap_uint<3> a0t, a1t, mdt, fpt, lst, brt, vct;
    ap_uint<1> cv, cfp; ap_uint<5> crd; ap_uint<32> cval;
    static ap_uint<32> vregs_out[OOO_VEC_REGFILE_LEN];
    ap_uint<1> halted;

    riscv_soc_tick(1, imem, dmem, dv, dt, dp, a0d,a0t, a1d,a1t, mdd,mdt,
                  fpd,fpt, lsd,lst, brd,brt, vcd,vct, cv, cfp, crd, cval,
                  vregs_out, halted);
    long cycle = 0;
    while (!halted && cycle < max_cycles) {
        cycle++;
        riscv_soc_tick(0, imem, dmem, dv, dt, dp, a0d,a0t, a1d,a1t, mdd,mdt,
                      fpd,fpt, lsd,lst, brd,brt, vcd,vct, cv, cfp, crd, cval,
                      vregs_out, halted);
    }
    for (int i = 0; i < OOO_DMEM_WORDS; i++) dmem_out[i] = dmem[i];
    return halted ? cycle : -1;
}

// Verifica una lista de posiciones de memoria dejadas por el programa.
struct MemCheck { uint32_t addr; uint32_t expect; const char* what; };
static int check_mem(ap_uint<32> dmem[OOO_DMEM_WORDS], const MemCheck* cks, int n) {
    int fails = 0;
    for (int i = 0; i < n; i++) {
        uint32_t got = dmem[(cks[i].addr >> 2) & (OOO_DMEM_WORDS - 1)].to_uint();
        if (got != cks[i].expect) {
            printf("FAIL  mem[0x%x] = 0x%x, esperado 0x%x (%s)\n",
                   cks[i].addr, got, cks[i].expect, cks[i].what);
            fails++;
        } else {
            printf("OK    mem[0x%x] = 0x%x (%s)\n", cks[i].addr, got, cks[i].what);
        }
    }
    return fails;
}

// ---------------------------------------------------------------------
// Suite A: ISA (I+M+F+C) + RVV Fases 1-3, programa ensamblado a mano
// ---------------------------------------------------------------------
static int suite_isa_rvv() {
    ap_uint<32> imem[OOO_IMEM_WORDS] = {0};
    ap_uint<32> dmem[OOO_DMEM_WORDS] = {0};

    // datos de prueba para la seccion RVV -- direcciones 0/16, lejos de
    // 64/68 que usa la seccion escalar (dmem[16]/dmem[17]), sin colision
    dmem[0] = 10; dmem[1] = 20; dmem[2] = 30; dmem[3] = 40; // v1 tras vle32.v
    dmem[4] = 1;  dmem[5] = 2;  dmem[6] = 3;  dmem[7] = 4;  // v2 tras vle32.v

    const uint32_t OP     = rv32i::Opcode::OP;
    const uint32_t OP_IMM = rv32i::Opcode::OP_IMM;
    const uint32_t LOAD   = rv32i::Opcode::LOAD;
    const uint32_t STORE  = rv32i::Opcode::STORE;
    const uint32_t MULDIV = rv32i::Funct7::MULDIV;
    const uint32_t OP_FP  = rv32i::Opcode::OP_FP;
    const uint32_t rvv_funct6_add = 0b000000;
    const uint32_t rvv_funct6_mul = 0b100101;

    std::vector<uint16_t> prog;
    auto push32 = [&](uint32_t w) { prog.push_back(w & 0xFFFF); prog.push_back(w >> 16); };
    auto push16 = [&](uint16_t h) { prog.push_back(h); };

    // ---- parte I+M (pcs 0..56) ----
    push32(i_type(OP_IMM, 1, F3A::ADD_SUB, 0, 5));         //   0: addi x1,x0,5      d0
    push32(i_type(OP_IMM, 2, F3A::ADD_SUB, 0, 10));        //   4: addi x2,x0,10     d1
    push32(r_type(OP, 3, F3M::MUL, 1, 2, MULDIV));         //   8: mul  x3,x1,x2     d2  (lat 3)
    push32(i_type(OP_IMM, 4, F3A::ADD_SUB, 0, 7));         //  12: addi x4,x0,7      d3  (OOO vs d2)
    push32(r_type(OP, 5, F3A::ADD_SUB, 3, 4, 0));          //  16: add  x5,x3,x4     d4  = 57
    push32(s_type(STORE, rv32i::Funct3_STORE::SW, 0, 5, 64)); // 20: sw x5,64(x0)    d5
    push32(i_type(LOAD, 6, rv32i::Funct3_LOAD::LW, 0, 64));   // 24: lw x6,64(x0)    d6
    push32(b_type(rv32i::Funct3_BRANCH::BEQ, 1, 1, 8));    //  28: beq  x1,x1,+8     d7  (tomado)
    push32(i_type(OP_IMM, 7, F3A::ADD_SUB, 0, 99));        //  32: addi x7,x0,99     (saltada)
    push32(i_type(OP_IMM, 7, F3A::ADD_SUB, 0, 1));         //  36: addi x7,x0,1      d8
    push32(j_type(8, 8));                                  //  40: jal  x8,+8        d9  (link 44)
    push32(i_type(OP_IMM, 9, F3A::ADD_SUB, 0, 88));        //  44: addi x9,x0,88     (saltada)
    push32(u_type(rv32i::Opcode::LUI, 10, 0x12345));       //  48: lui  x10,0x12345  d10
    push32(r_type(OP, 11, F3M::DIV, 3, 1, MULDIV));        //  52: div  x11,x3,x1    d11 (lat 8)
    push32(r_type(OP, 12, F3A::ADD_SUB, 2, 1, rv32i::Funct7::ALT)); // 56: sub x12,x2,x1 d12 (OOO vs d11)

    // ---- parte F (pcs 60..104) ----
    push32(i_type(OP_IMM, 20, F3A::ADD_SUB, 0, 3));        //  60: addi x20,x0,3     d13
    push32(r_type(OP_FP, 1, 0, 20, rv32i::Rs2_FCVT::W, F7F::FCVT_S_W)); // 64: fcvt.s.w f1,x20  d14 (3.0)
    push32(i_type(OP_IMM, 21, F3A::ADD_SUB, 0, 4));        //  68: addi x21,x0,4     d15
    push32(r_type(OP_FP, 2, 0, 21, rv32i::Rs2_FCVT::W, F7F::FCVT_S_W)); // 72: fcvt.s.w f2,x21  d16 (4.0)
    push32(r_type(OP_FP, 3, 0, 1, 2, F7F::FMUL_S));        //  76: fmul.s f3,f1,f2   d17 (12.0)
    push32(r_type(rv32i::Opcode::FMADD, 5, 0, 1, 2, (3u << 2))); // 80: fmadd.s f5,f1,f2,f3 d18 (3*4+12=24.0)
    push32(s_type(rv32i::Opcode::STORE_FP, rv32i::Funct3_FP_MEM::W, 0, 5, 68)); // 84: fsw f5,68(x0) d19
    push32(i_type(rv32i::Opcode::LOAD_FP, 6, rv32i::Funct3_FP_MEM::W, 0, 68));  // 88: flw f6,68(x0) d20
    push32(r_type(OP_FP, 22, rv32i::Funct3_FCMP::FEQ, 5, 6, F7F::FCMP_S)); // 92: feq.s x22,f5,f6 d21 (=1)
    push32(r_type(OP_FP, 23, 0, 3, 0, F7F::FMV_X_W_FCLASS_S)); // 96: fmv.x.w x23,f3 d22 (0x41400000)
    push32(r_type(OP_FP, 7, 0, 3, 1, F7F::FDIV_S));        // 100: fdiv.s f7,f3,f1   d23 (4.0, lat 8)
    push32(i_type(OP_IMM, 24, F3A::ADD_SUB, 0, 2));        // 104: addi x24,x0,2     d24 (OOO vs d23)

    // ---- parte C (pcs 108..114) ----
    push16(0x47A5);                                        // 108: c.li x15,9        d25 (16 bits)
    push32(i_type(OP_IMM, 16, F3A::ADD_SUB, 0, 21));       // 110: addi x16,x0,21    d26 (32 bits, pc%4==2: straddle)
    push16(0x078D);                                        // 114: c.addi x15,3      d27 -> x15=12

    // ---- parte RVV: coprocesamiento (pcs 116..164) ----
    // Todo programa RVV real arranca configurando el largo vectorial con
    // vsetvli: al reset vtype tiene vill y vl=0, asi que SIN este vsetvli
    // las vectoriales de abajo no procesarian ningun elemento.
    push32(enc_vsetvli(/*rd=*/19, /*rs1=*/0, VTYPE_E32_M1));            // 116: vsetvli x19,x0,e32,m1 d28 -> vl=VLMAX=4
    push32(enc_vec_mem(rv32i::Opcode::LOAD_FP, /*vd=*/1, /*rs1=*/0));    // 120: vle32.v v1,(x0)   d29 -> {10,20,30,40}
    push32(i_type(OP_IMM, 13, F3A::ADD_SUB, 0, 16));                    // 124: addi x13,x0,16    d30
    push32(enc_vec_mem(rv32i::Opcode::LOAD_FP, /*vd=*/2, /*rs1=*/13));  // 128: vle32.v v2,(x13)  d31 -> {1,2,3,4}
    push32(enc_op_v(rvv_funct6_add, 2, 1, rvv_f3_opivv, /*vd=*/3));     // 132: vadd.vv v3,v2,v1  d32 (lat 2) -> {11,22,33,44}
    push32(i_type(OP_IMM, 14, F3A::ADD_SUB, 0, 99));                    // 136: addi x14,x0,99    d33 (OOO vs d32)
    push32(enc_op_v(rvv_funct6_mul, 1, 1, rvv_f3_opmvv, /*vd=*/4));     // 140: vmul.vv v4,v1,v1  d34 (lat 2) -> {100,400,900,1600}
    push32(i_type(OP_IMM, 17, F3A::ADD_SUB, 0, 77));                    // 144: addi x17,x0,77    d35 (OOO vs d34)
    push32(enc_vec_mem(rv32i::Opcode::LOAD_FP, /*vd=*/5, /*rs1=*/0));   // 148: vle32.v v5,(x0)   d36 -> centinela {10,20,30,40}
    push32(i_type(OP_IMM, 18, F3A::ADD_SUB, 0, 32));                    // 152: addi x18,x0,32    d37
    push32(enc_vec_mem(rv32i::Opcode::STORE_FP, /*vs3=*/3, /*rs1=*/18)); // 156: vse32.v v3,(x18) d38 -> dmem[32..47]

    // ---- largo vectorial DINAMICO: vl=2 (AVL=2 < VLMAX=4) ----
    // Con vl=2, el vadd.vv solo debe tocar los 2 primeros elementos de v5
    // y dejar los otros 2 SIN TOCAR (tail-undisturbed): v5 quedara
    // {11,22, 30,40} -- los dos primeros calculados, los dos ultimos
    // sobrevivientes del centinela cargado arriba con vl=4.
    push32(i_type(OP_IMM, 26, F3A::ADD_SUB, 0, 2));                     // 160: addi x26,x0,2     d39 (AVL=2)
    push32(enc_vsetvli(/*rd=*/27, /*rs1=*/26, VTYPE_E32_M1));           // 164: vsetvli x27,x26,e32,m1 d40 -> vl=2
    push32(enc_op_v(rvv_funct6_add, 2, 1, rvv_f3_opivv, /*vd=*/5));     // 168: vadd.vv v5,v2,v1  d41 -> solo lanes 0..1

    // ---- Fase 1: familias de ops, formas escalares y MASCARA ----
    // OJO: rd != x0. Con rd=x0 Y rs1=x0 la spec manda CONSERVAR el vl.
    push32(enc_vsetvli(/*rd=*/19, /*rs1=*/0, VTYPE_E32_M1));            // 172: vsetvli x19,x0 -> vl=4
    push32(enc_op_v(F6_VAND, 2, 1, rvv_f3_opivv, /*vd=*/6));           // 176: vand.vv v6,v2,v1
    push32(enc_op_v(F6_VOR,  2, 1, rvv_f3_opivv, /*vd=*/7));           // 180: vor.vv  v7,v2,v1
    push32(enc_op_v(F6_VMAX, 2, 1, rvv_f3_opivv, /*vd=*/8));           // 184: vmax.vv v8,v2,v1
    push32(enc_op_v(F6_VSLL, 2, 1, rvv_f3_opivv, /*vd=*/9));           // 188: vsll.vv v9,v2,v1
    push32(enc_op_v(F6_VDIV, 1, 2, rvv_f3_opmvv, /*vd=*/10));          // 192: vdiv.vv v10,v1,v2
    push32(enc_op_v(rvv_funct6_add, 1, /*imm=*/3, F3_OPIVI, /*vd=*/11)); // 196: vadd.vi v11,v1,3
    push32(i_type(OP_IMM, 29, F3A::ADD_SUB, 0, 100));                  // 200: addi x29,x0,100
    push32(enc_op_v(rvv_funct6_add, 1, /*rs1=*/29, F3_OPIVX, /*vd=*/12)); // 204: vadd.vx v12,v1,x29
    push32(enc_op_v(F6_VRSUB, 1, /*imm=*/0, F3_OPIVI, /*vd=*/13));     // 208: vrsub.vi v13,v1,0

    // MASCARA: v0={1,0,1,0} -> solo lanes 0 y 2 se escriben; v14 se
    // precarga con el centinela {10,20,30,40} para ver los inactivos.
    push32(i_type(OP_IMM, 30, F3A::ADD_SUB, 0, 0b0101));               // 212: addi x30,x0,0b0101
    push32(i_type(OP_IMM, 31, F3A::ADD_SUB, 0, 48));                   // 216: addi x31,x0,48 (dmem[12])
    push32(s_type(STORE, rv32i::Funct3_STORE::SW, 31, 30, 0));         // 220: sw x30,0(x31)
    push32(enc_vec_mem(rv32i::Opcode::LOAD_FP, /*vd=*/0, /*rs1=*/31)); // 224: vle32.v v0,(x31) -> mascara
    push32(enc_vec_mem(rv32i::Opcode::LOAD_FP, /*vd=*/14, /*rs1=*/0)); // 228: vle32.v v14,(x0) -> centinela
    push32(enc_op_v_m(rvv_funct6_add, 2, 1, rvv_f3_opivv, /*vd=*/14, /*vm=*/0)); // 232: vadd.vv v14,v2,v1,v0.t

    // ---- Fase 2: ANCHO DE ELEMENTO VARIABLE (EEW 8 y 16) ----
    // dmem[0..3] = {10,20,30,40} -> como BYTES (LE): 0A,00,00,00, 14,...
    push32(enc_vsetvli(/*rd=*/28, /*rs1=*/0, VTYPE_E8_M1));            // vsetvli x28,x0,e8,m1 -> vl=16
    push32(enc_vec_mem_w(rv32i::Opcode::LOAD_FP, /*vd=*/15, /*rs1=*/0, WIDTH_8));  // vle8.v v15,(x0)
    push32(enc_op_v(rvv_funct6_add, 15, /*imm=*/1, F3_OPIVI, /*vd=*/16)); // vadd.vi v16,v15,1 (16 elems de 8b)
    push32(enc_vsetvli(/*rd=*/28, /*rs1=*/0, VTYPE_E16_M1));           // vsetvli x28,x0,e16,m1 -> vl=8
    push32(enc_vec_mem_w(rv32i::Opcode::LOAD_FP, /*vd=*/17, /*rs1=*/0, WIDTH_16)); // vle16.v v17,(x0)
    push32(enc_op_v(rvv_funct6_add, 17, /*imm=*/2, F3_OPIVI, /*vd=*/18)); // vadd.vi v18,v17,2 (8 elems de 16b)

    // ---- Fase 3: comparaciones, merge, logica de mascaras, unary ----
    push32(enc_vsetvli(/*rd=*/28, /*rs1=*/0, VTYPE_E32_M1));           // vsetvli x28,x0,e32 -> vl=4
    push32(enc_op_v(F6_VMSEQ, 1, 1, rvv_f3_opivv, /*vd=*/19));         // vmseq.vv v19,v1,v1 -> 0b1111
    push32(enc_op_v(F6_VMSLT, 1, 2, rvv_f3_opivv, /*vd=*/20));         // vmslt.vv v20,v1,v2 -> 0b0000
    push32(enc_op_v(F6_VMSGT, 1, /*imm=*/15, F3_OPIVI, /*vd=*/21));    // vmsgt.vi v21,v1,15 -> 0b1110
    push32(enc_op_v(F6_VMAND, 19, 21, rvv_f3_opmvv, /*vd=*/22));       // vmand.mm v22,v19,v21 -> 0b1110
    push32(enc_op_v(F6_VMOR,  20, 21, rvv_f3_opmvv, /*vd=*/23));       // vmor.mm  v23,v20,v21 -> 0b1110
    push32(enc_op_v_m(F6_VMERGE, 2, 1, rvv_f3_opivv, /*vd=*/24, 0));   // vmerge.vvm v24,v2,v1,v0 (v0=0b0101)
    push32(enc_op_v(F6_VMERGE, 0, 1, rvv_f3_opivv, /*vd=*/25));        // vmv.v.v v25,v1
    push32(enc_op_v(F6_VMUNARY0, 0, VS1_VID, rvv_f3_opmvv, /*vd=*/26)); // vid.v v26 -> {0,1,2,3}
    push32(enc_op_v(F6_VMUNARY0, 21, VS1_VIOTA, rvv_f3_opmvv, /*vd=*/27)); // viota.m v27,v21 -> {0,0,1,2}
    push32(enc_op_v(F6_VWXUNARY0, 21, VS1_VCPOP,  rvv_f3_opmvv, /*vd=*/29)); // vcpop.m x29,v21 -> 3
    push32(enc_op_v(F6_VWXUNARY0, 21, VS1_VFIRST, rvv_f3_opmvv, /*vd=*/30)); // vfirst.m x30,v21 -> 1

    push16(0x0000);                                        // fin de programa

    // guarda: si el programa no cabe en imem, el packing de abajo
    // desbordaria sobre dmem y los resultados no tendrian sentido.
    if (prog.size() > 2 * OOO_IMEM_WORDS) {
        printf("FAIL  el programa (%zu halfwords) no cabe en imem (%d palabras)\n",
               prog.size(), OOO_IMEM_WORDS);
        return 1;
    }
    for (size_t i = 0; i < prog.size(); i++) {
        uint32_t w = imem[i / 2].to_uint();
        if (i % 2 == 0) w = (w & 0xFFFF0000u) | prog[i];
        else            w = (w & 0x0000FFFFu) | (uint32_t(prog[i]) << 16);
        imem[i / 2] = w;
    }

    ap_uint<1> disp_valid; ap_uint<3> disp_tag; ap_uint<32> disp_pc;
    ap_uint<1> alu0_done;  ap_uint<3> alu0_tag;
    ap_uint<1> alu1_done;  ap_uint<3> alu1_tag;
    ap_uint<1> md_done;    ap_uint<3> md_tag;
    ap_uint<1> fpu_done;   ap_uint<3> fpu_tag;
    ap_uint<1> lsu_done;   ap_uint<3> lsu_tag;
    ap_uint<1> br_done;    ap_uint<3> br_tag;
    ap_uint<1> vec_done;   ap_uint<3> vec_tag;
    ap_uint<1> commit_valid; ap_uint<1> commit_is_fp;
    ap_uint<5> commit_rd; ap_uint<32> commit_value;
    ap_uint<32> vregs_out[OOO_VEC_REGFILE_LEN];
    ap_uint<1> halted;

    riscv_soc_tick(1, imem, dmem,
                  disp_valid, disp_tag, disp_pc,
                  alu0_done, alu0_tag, alu1_done, alu1_tag,
                  md_done, md_tag, fpu_done, fpu_tag,
                  lsu_done, lsu_tag, br_done, br_tag, vec_done, vec_tag,
                  commit_valid, commit_is_fp, commit_rd, commit_value,
                  vregs_out, halted);

    uint32_t regs[32] = {0};   // ambos reconstruidos SOLO desde commits
    uint32_t fregs[32] = {0};
    int tag2disp[8];
    for (int i = 0; i < 8; i++) tag2disp[i] = -1;
    int complete_cycle[64];
    uint32_t pc_of_disp[64];
    for (int i = 0; i < 64; i++) { complete_cycle[i] = -1; pc_of_disp[i] = 0xFFFFFFFF; }
    int n_disp = 0, n_commit = 0;

    printf("ciclo | evento\n");
    printf("------+------------------------------------------------\n");

    int cycle = 0;
    const int MAX_CYCLES = 300;
    while (!halted && cycle < MAX_CYCLES) {
        cycle++;
        riscv_soc_tick(0, imem, dmem,
                      disp_valid, disp_tag, disp_pc,
                      alu0_done, alu0_tag, alu1_done, alu1_tag,
                      md_done, md_tag, fpu_done, fpu_tag,
                      lsu_done, lsu_tag, br_done, br_tag, vec_done, vec_tag,
                      commit_valid, commit_is_fp, commit_rd, commit_value,
                      vregs_out, halted);

        if (disp_valid) {
            tag2disp[disp_tag.to_uint()] = n_disp;
            if (n_disp < 64) pc_of_disp[n_disp] = disp_pc.to_uint();
            printf("%5d | DISPATCH d%-2d (pc=%3u, tag %d)\n",
                   cycle, n_disp, disp_pc.to_uint(), disp_tag.to_uint());
            n_disp++;
        }
        struct { ap_uint<1>* v; ap_uint<3>* t; const char* name; } evs[] = {
            {&alu0_done, &alu0_tag, "ALU0"}, {&alu1_done, &alu1_tag, "ALU1"},
            {&md_done, &md_tag, "MULDIV"}, {&fpu_done, &fpu_tag, "FPU"},
            {&lsu_done, &lsu_tag, "LSU"}, {&br_done, &br_tag, "BR"},
            {&vec_done, &vec_tag, "VEC"},
        };
        for (auto& e : evs) {
            if (*e.v) {
                int d = tag2disp[e.t->to_uint()];
                if (d >= 0 && d < 64) complete_cycle[d] = cycle;
                printf("%5d | %-6s completa d%-2d (tag %d)\n",
                       cycle, e.name, d, e.t->to_uint());
            }
        }
        if (commit_valid) {
            n_commit++;
            if (commit_is_fp) {
                fregs[commit_rd.to_uint()] = commit_value.to_uint();
                printf("%5d | COMMIT  f%-2u = 0x%08x\n",
                       cycle, commit_rd.to_uint(), commit_value.to_uint());
            } else {
                if (commit_rd != 0) regs[commit_rd.to_uint()] = commit_value.to_uint();
                printf("%5d | COMMIT  x%-2u = 0x%08x\n",
                       cycle, commit_rd.to_uint(), commit_value.to_uint());
            }
        }
    }
    printf("------+------------------------------------------------\n");
    printf("Halted en el ciclo %d (%d dispatches, %d commits)\n\n", cycle, n_disp, n_commit);

    int fails = 0;
    struct { int reg; uint32_t expect; const char* what; } xchecks[] = {
        {1, 5,           "addi"},
        {2, 10,          "addi"},
        {3, 50,          "mul"},
        {4, 7,           "addi independiente del mul"},
        {5, 57,          "add dependiente del mul (RAW via CDB)"},
        {6, 57,          "lw round-trip del sw"},
        {7, 1,           "beq tomado (99 nunca ejecuto)"},
        {8, 44,          "link del jal"},
        {9, 0,           "instruccion saltada por jal"},
        {10, 0x12345000, "lui"},
        {11, 10,         "div"},
        {12, 5,          "sub independiente del div"},
        {15, 12,         "C.LI + C.ADDI comprimidas"},
        {16, 21,         "addi de 32 bits en pc%4==2 (straddle)"},
        {20, 3,          "addi (fuente de fcvt)"},
        {21, 4,          "addi (fuente de fcvt)"},
        {22, 1,          "feq.s f5==f6 (FP escribe banco entero)"},
        {23, 0x41400000, "fmv.x.w bits de 12.0f"},
        {24, 2,          "addi independiente del fdiv"},
        {13, 16,         "addi (base del segundo vle32.v)"},
        {14, 99,         "addi independiente de vadd.vv"},
        {17, 77,         "addi independiente de vmul.vv"},
        {19, 4,          "vsetvli x19,x0 -> vl=VLMAX=4 (rs1=x0 pide el maximo)"},
        {26, 2,          "addi (AVL para el segundo vsetvli)"},
        {27, 2,          "vsetvli x27,x26 -> vl=min(AVL=2,VLMAX=4)=2 (largo dinamico)"},
        {28, 4,          "vsetvli e32 al inicio de Fase 3 -> vl=VLMAX=4"},
        {29, 3,          "vcpop.m  sobre mascara 0b1110 -> 3 bits en 1"},
        {30, 1,          "vfirst.m sobre mascara 0b1110 -> primer bit en indice 1"},
        {18, 32,         "addi (direccion del vse32.v)"},
    };
    for (auto& c : xchecks) {
        if (regs[c.reg] != c.expect) {
            printf("FAIL  x%-2d = 0x%08x, esperado 0x%08x (%s)\n",
                   c.reg, regs[c.reg], c.expect, c.what);
            fails++;
        } else {
            printf("OK    x%-2d = 0x%08x (%s)\n", c.reg, regs[c.reg], c.what);
        }
    }
    struct { int reg; uint32_t expect; const char* what; } fchecks[] = {
        {1, 0x40400000, "fcvt.s.w 3 -> 3.0"},
        {2, 0x40800000, "fcvt.s.w 4 -> 4.0"},
        {3, 0x41400000, "fmul.s 3*4 = 12.0"},
        {5, 0x41C00000, "fmadd.s 3*4+12 = 24.0 (rs3)"},
        {6, 0x41C00000, "flw round-trip del fsw"},
        {7, 0x40800000, "fdiv.s 12/3 = 4.0"},
    };
    for (auto& c : fchecks) {
        if (fregs[c.reg] != c.expect) {
            printf("FAIL  f%-2d = 0x%08x, esperado 0x%08x (%s)\n",
                   c.reg, fregs[c.reg], c.expect, c.what);
            fails++;
        } else {
            printf("OK    f%-2d = 0x%08x (%s)\n", c.reg, fregs[c.reg], c.what);
        }
    }

    // Los checks de orden se hacen POR PC, no por numero de dispatch: con
    // el frontend especulativo (TAGE) puede haber dispatches descartados
    // por mispredict, asi que los indices dN ya no son estables. El ULTIMO
    // dispatch de un pc dado es el arquitectonico (los descartados nunca
    // completan o son sobrescritos por el re-dispatch tras el flush).
    auto cyc_of_pc = [&](uint32_t pc) -> int {
        int r = -1;
        for (int d = 0; d < 64 && d < n_disp; d++)
            if (pc_of_disp[d] == pc) r = complete_cycle[d];
        return r;
    };
    struct { uint32_t later, earlier; const char* what; } ooo[] = {
        {12,  8,   "addi(pc12) antes que mul(pc8)"},
        {56,  52,  "sub(pc56) antes que div(pc52)"},
        {104, 100, "addi(pc104) antes que fdiv(pc100) -- OOO cruzando bancos"},
        {136, 132, "addi(pc136) antes que vadd.vv(pc132) -- coprocesamiento"},
        {144, 140, "addi(pc144) antes que vmul.vv(pc140) -- coprocesamiento"},
    };
    for (auto& c : ooo) {
        int cl = cyc_of_pc(c.later), ce = cyc_of_pc(c.earlier);
        if (cl > 0 && ce > 0 && cl < ce) {
            printf("OK    OOO: %s (ciclos %d < %d)\n", c.what, cl, ce);
        } else {
            printf("FAIL  OOO: %s NO se cumplio (ciclos %d vs %d)\n", c.what, cl, ce);
            fails++;
        }
    }

    // Los COMMITS son arquitectonicos: no dependen del predictor. Los
    // dispatches ahora pueden incluir especulativos descartados, asi que
    // solo se exige n_disp >= n_commit.
    const int N_EXPECTED = 76;
    if (n_commit != N_EXPECTED) {
        printf("FAIL  commits: %d, esperados %d\n", n_commit, N_EXPECTED);
        fails++;
    }
    if (n_disp < n_commit) {
        printf("FAIL  dispatches (%d) < commits (%d)\n", n_disp, n_commit);
        fails++;
    }
    if (dmem[16] != 57) {
        printf("FAIL  dmem[16] = 0x%08x, esperado 57 (sw al commit)\n", dmem[16].to_uint());
        fails++;
    } else {
        printf("OK    dmem[16] = 57 (sw escribio al commit)\n");
    }
    if (dmem[17] != 0x41C00000) {
        printf("FAIL  dmem[17] = 0x%08x, esperado 0x41C00000 (fsw al commit)\n", dmem[17].to_uint());
        fails++;
    } else {
        printf("OK    dmem[17] = 0x41C00000 (fsw escribio 24.0f al commit)\n");
    }

    // banco vectorial: leido del puerto vregs_out (no es backdoor, es un
    // puerto de salida declarado del DUT, igual que r0_out..r3_out en
    // ooo_demo.cpp y vregs_out en rv32_vector.cpp)
    struct { int vreg, lane; uint32_t expect; } vchecks[] = {
        {1,0,10},{1,1,20},{1,2,30},{1,3,40},   // v1 (vle32.v)
        {2,0,1}, {2,1,2}, {2,2,3}, {2,3,4},    // v2 (vle32.v)
        {3,0,11},{3,1,22},{3,2,33},{3,3,44},   // v3 (vadd.vv)
        {4,0,100},{4,1,400},{4,2,900},{4,3,1600}, // v4 (vmul.vv)
        // v5: vadd.vv ejecutado con vl=2 -- lanes 0..1 calculados,
        // lanes 2..3 conservan el centinela cargado con vl=4 (tail
        // undisturbed). Esta es la evidencia del largo vectorial dinamico.
        {5,0,11},{5,1,22},{5,2,30},{5,3,40},
        // --- Fase 1: familias enteras (v1={10,20,30,40}, v2={1,2,3,4}) ---
        {6,0,0},{6,1,0},{6,2,2},{6,3,0},                 // vand.vv (4&40=0)
        {7,0,11},{7,1,22},{7,2,31},{7,3,44},             // vor.vv
        {8,0,10},{8,1,20},{8,2,30},{8,3,40},             // vmax.vv
        // vsll: shift acotado a 5 bits -> 4<<(40&31)=4<<8=1024
        {9,0,1024},{9,1,2097152},{9,2,3221225472u},{9,3,1024},
        {10,0,10},{10,1,10},{10,2,10},{10,3,10},         // vdiv.vv v1/v2
        // --- Fase 1: formas vector-escalar ---
        {11,0,13},{11,1,23},{11,2,33},{11,3,43},         // vadd.vi v1+3
        {12,0,110},{12,1,120},{12,2,130},{12,3,140},     // vadd.vx v1+100
        {13,0,(uint32_t)-10},{13,1,(uint32_t)-20},{13,2,(uint32_t)-30},{13,3,(uint32_t)-40}, // vrsub.vi
        // --- Fase 1: MASCARA v0={1,0,1,0} sobre centinela {10,20,30,40} ---
        {14,0,11},{14,1,20},{14,2,33},{14,3,40},
        // --- Fase 2: EEW=8 (16 elementos de 8 bits), vadd.vi +1 ---
        {15,0,0x0000000A},{15,1,0x00000014},{15,2,0x0000001E},{15,3,0x00000028},
        {16,0,0x0101010B},{16,1,0x01010115},{16,2,0x0101011F},{16,3,0x01010129},
        // --- Fase 2: EEW=16 (8 elementos de 16 bits), vadd.vi +2 ---
        {17,0,0x0000000A},{17,1,0x00000014},{17,2,0x0000001E},{17,3,0x00000028},
        {18,0,0x0002000C},{18,1,0x00020016},{18,2,0x00020020},{18,3,0x0002002A},
        // --- Fase 3: comparaciones -> MASCARAS ---
        {19,0,0b1111},{20,0,0b0000},{21,0,0b1110},
        // --- Fase 3: logica entre mascaras ---
        {22,0,0b1110},{23,0,0b1110},
        // --- Fase 3: vmerge (v0=0b0101 elige v1) y vmv.v.v ---
        {24,0,10},{24,1,2},{24,2,30},{24,3,4},
        {25,0,10},{25,1,20},{25,2,30},{25,3,40},
        // --- Fase 3: vid.v y viota.m sobre 0b1110 ---
        {26,0,0},{26,1,1},{26,2,2},{26,3,3},
        {27,0,0},{27,1,0},{27,2,1},{27,3,2},
    };
    for (auto& c : vchecks) {
        uint32_t got = vregs_out[c.vreg * OOO_VEC_LANES + c.lane].to_uint();
        if (got != c.expect) {
            printf("FAIL  v%d[%d] = 0x%08x, esperado 0x%08x\n", c.vreg, c.lane, got, c.expect);
            fails++;
        } else {
            printf("OK    v%d[%d] = 0x%08x\n", c.vreg, c.lane, got);
        }
    }
    uint32_t m32 = dmem[8].to_uint(), m33 = dmem[9].to_uint(), m34 = dmem[10].to_uint(), m35 = dmem[11].to_uint();
    if (m32 == 11 && m33 == 22 && m34 == 33 && m35 == 44) {
        printf("OK    dmem[32..47] = {11,22,33,44} (vse32.v round-trip, resuelto en cabeza del ROB)\n");
    } else {
        printf("FAIL  dmem[32..47] = {%u,%u,%u,%u}, esperado {11,22,33,44}\n", m32, m33, m34, m35);
        fails++;
    }

    return fails;
}

// ---------------------------------------------------------------------
// Suite B: excepciones precisas y REANUDABLES (ELF real)
// ---------------------------------------------------------------------
static int suite_excepciones() {
    static ap_uint<32> dmem[OOO_DMEM_WORDS];
    long c = run_elf(trap_elf, trap_elf_len, dmem, 5000);
    if (c < 0) { printf("FAIL  el programa no termino\n"); return 1; }
    printf("      (termino en %ld ciclos)\n", c);
    static const MemCheck cks[] = {
        {0x80, 1,      "out[0]=1 escrito ANTES del ECALL"},
        {0x90, 0xBEEF, "el handler instalado en mtvec REALMENTE corrio"},
        {0x94, 11,     "mcause=11 (environment call from M-mode), por la spec"},
        {0x84, 2,      "out[1]=2 DESPUES del MRET -> excepcion REANUDABLE"},
    };
    return check_mem(dmem, cks, sizeof(cks)/sizeof(cks[0]));
}

// ---------------------------------------------------------------------
// Suite C: UART, interrupcion de timer y modos de privilegio M/U
// ---------------------------------------------------------------------
static int suite_sistema() {
    static ap_uint<32> dmem[OOO_DMEM_WORDS];
    printf("      salida del programa por el UART: >>> ");
    long c = run_elf(full_elf, full_elf_len, dmem, 50000);
    printf(" <<<\n");
    if (c < 0) { printf("FAIL  el programa no termino\n"); return 1; }
    printf("      (termino en %ld ciclos)\n", c);
    static const MemCheck cks[] = {
        {0x400, 1,    "el programa avanzo tras imprimir por el UART"},
        {0x404, 0x11, "el handler atendio la INTERRUPCION de timer y volvio con MRET"},
        {0x408, 8,    "ECALL desde U-mode reporta causa 8 (no 11): privilegios funcionan"},
    };
    return check_mem(dmem, cks, sizeof(cks)/sizeof(cks[0]));
}

// ---------------------------------------------------------------------
// Suite D: printf de la biblioteca C real (newlib, ~54 KB de codigo)
// ---------------------------------------------------------------------
static int suite_printf() {
    static ap_uint<32> dmem[OOO_DMEM_WORDS];
    printf("      salida del programa por el UART:\n>>> ");
    long c = run_elf(printf_elf, printf_elf_len, dmem, 3000000);
    printf("<<<\n");
    if (c < 0) { printf("FAIL  el programa no termino\n"); return 1; }
    printf("      (termino en %ld ciclos)\n", c);
    printf("OK    printf de newlib se ejecuto completo sobre el core\n");
    return 0;
}

// ---------------------------------------------------------------------
static int suite_vrat_directed() {
    ap_uint<32> imem[OOO_IMEM_WORDS] = {0};
    ap_uint<32> dmem[OOO_DMEM_WORDS] = {0};

    // v1 = {1,2,3,4}
    dmem[0] = 1; dmem[1] = 2; dmem[2] = 3; dmem[3] = 4;

    // v2 = {10,20,30,40}
    dmem[4] = 10; dmem[5] = 20; dmem[6] = 30; dmem[7] = 40;

    const uint32_t F6_ADD = 0b000000;
    const uint32_t F6_MUL = 0b100101;

    std::vector<uint16_t> prog;

    auto push32 = [&](uint32_t w) {
        prog.push_back(w & 0xFFFF);
        prog.push_back(w >> 16);
    };

    auto push16 = [&](uint16_t h) {
        prog.push_back(h);
    };

    // vl = 4, e32,m1
    push32(enc_vsetvli(1, 0, VTYPE_E32_M1));

    // Cargar operandos.
    push32(enc_vec_mem(rv32i::Opcode::LOAD_FP, 1, 0));

    push32(i_type(
        rv32i::Opcode::OP_IMM,
        2,
        F3A::ADD_SUB,
        0,
        16
    ));

    push32(enc_vec_mem(rv32i::Opcode::LOAD_FP, 2, 2));

    // ------------------------------------------------------------
    // RAW
    // v3 = v1 * v2
    // v4 = v3 + v2
    // v4 DEBE consumir el resultado renombrado de v3.
    // ------------------------------------------------------------
    push32(enc_op_v(F6_MUL, 1, 2, rvv_f3_opmvv, 3));
    push32(enc_op_v(F6_ADD, 3, 2, rvv_f3_opivv, 4));

    // ------------------------------------------------------------
    // WAW
    // Dos productores consecutivos de v5.
    // El segundo mapping debe sobrevivir al commit del primero.
    // ------------------------------------------------------------
    push32(enc_op_v(F6_ADD, 1, 2, rvv_f3_opivv, 5));
    push32(enc_op_v(F6_ADD, 2, 2, rvv_f3_opivv, 5));

    // ------------------------------------------------------------
    // Dos instrucciones independientes.
    // Todavia comparten vec_rs, asi que aqui solo validamos estado.
    // ------------------------------------------------------------
    push32(enc_op_v(F6_MUL, 1, 1, rvv_f3_opmvv, 6));
    push32(enc_op_v(F6_ADD, 2, 2, rvv_f3_opivv, 7));

    push16(0x0000);

    for (size_t i = 0; i < prog.size(); i++) {
        uint32_t w = imem[i / 2].to_uint();

        if ((i & 1) == 0)
            w = (w & 0xFFFF0000u) | prog[i];
        else
            w = (w & 0x0000FFFFu) |
                (uint32_t(prog[i]) << 16);

        imem[i / 2] = w;
    }

    ap_uint<1> dv;
    ap_uint<3> dt;
    ap_uint<32> dp;

    ap_uint<1> a0d,a1d,mdd,fpd,lsd,brd,vcd;
    ap_uint<3> a0t,a1t,mdt,fpt,lst,brt,vct;

    ap_uint<1> cv,cfp;
    ap_uint<5> crd;
    ap_uint<32> cval;

    static ap_uint<32> vo[OOO_VEC_REGFILE_LEN];
    ap_uint<1> halted;

    riscv_soc_tick(
        1, imem, dmem,
        dv,dt,dp,
        a0d,a0t,a1d,a1t,
        mdd,mdt,fpd,fpt,
        lsd,lst,brd,brt,vcd,vct,
        cv,cfp,crd,cval,
        vo,halted
    );

    int cycles = 0;
    int vec_completions = 0;

    int indep_mul_dispatch = -1;
    int indep_add_dispatch = -1;
    int indep_mul_complete = -1;
    int indep_add_complete = -1;

    ap_uint<3> indep_mul_tag = 0;
    ap_uint<3> indep_add_tag = 0;

    bool indep_mul_seen = false;
    bool indep_add_seen = false;

    while (!halted && cycles < 500) {
        cycles++;

        riscv_soc_tick(
            0, imem, dmem,
            dv,dt,dp,
            a0d,a0t,a1d,a1t,
            mdd,mdt,fpd,fpt,
            lsd,lst,brd,brt,vcd,vct,
            cv,cfp,crd,cval,
            vo,halted
        );

        // PC 32 = VMUL independiente.
        if (dv && dp == 32) {
            indep_mul_dispatch = cycles;
            indep_mul_tag = dt;
            indep_mul_seen = true;
        }

        // PC 36 = VADD independiente.
        if (dv && dp == 36) {
            indep_add_dispatch = cycles;
            indep_add_tag = dt;
            indep_add_seen = true;
        }

        if (vcd) {
            vec_completions++;

            if (indep_mul_seen &&
                vct == indep_mul_tag &&
                indep_mul_complete < 0) {
                indep_mul_complete = cycles;
            }

            if (indep_add_seen &&
                vct == indep_add_tag &&
                indep_add_complete < 0) {
                indep_add_complete = cycles;
            }
        }
    }

    int fails = 0;

    auto ck = [&](int vr, int lane, uint32_t expected,
                  const char* name) {
        uint32_t got =
            vo[vr * OOO_VEC_LANES + lane].to_uint();

        if (got != expected) {
            printf(
                "FAIL  %s: v%d[%d]=%u esperado=%u\n",
                name, vr, lane, got, expected
            );
            fails++;
        }
    };

    // RAW: v3 = v1*v2
    ck(3,0,10,  "RAW productor");
    ck(3,1,40,  "RAW productor");
    ck(3,2,90,  "RAW productor");
    ck(3,3,160, "RAW productor");

    // RAW consumidor: v4 = v3+v2
    ck(4,0,20,  "RAW consumidor");
    ck(4,1,60,  "RAW consumidor");
    ck(4,2,120, "RAW consumidor");
    ck(4,3,200, "RAW consumidor");

    // WAW: debe quedar el segundo resultado.
    ck(5,0,20, "WAW");
    ck(5,1,40, "WAW");
    ck(5,2,60, "WAW");
    ck(5,3,80, "WAW");

    // Independientes.
    ck(6,0,1,  "independiente VMUL");
    ck(6,1,4,  "independiente VMUL");
    ck(6,2,9,  "independiente VMUL");
    ck(6,3,16, "independiente VMUL");

    ck(7,0,20, "independiente VADD");
    ck(7,1,40, "independiente VADD");
    ck(7,2,60, "independiente VADD");
    ck(7,3,80, "independiente VADD");

    printf(
        "VRAT directed: ciclos=%d, completions_vec=%d\n",
        cycles, vec_completions
    );

    if (!halted) {
        printf("FAIL  VRAT directed no termino\n");
        fails++;
    }

    printf(
        "VRAT overlap: VMUL dispatch=%d complete=%d | "
        "VADD dispatch=%d complete=%d\n",
        indep_mul_dispatch,
        indep_mul_complete,
        indep_add_dispatch,
        indep_add_complete
    );

    if (indep_mul_dispatch < 0 ||
        indep_add_dispatch < 0 ||
        indep_mul_complete < 0 ||
        indep_add_complete < 0) {

        printf("FAIL  no se observaron todos los eventos dual-slot\n");
        fails++;

    } else {

        // La VADD joven fue despachada mientras la VMUL vieja
        // todavia estaba en vuelo.
        if (indep_add_dispatch < indep_mul_complete) {
            printf(
                "PASS  overlap: VADD fue despachada antes de "
                "completar VMUL\n"
            );
        } else {
            printf(
                "FAIL  no hubo ventana de overlap VMUL/VADD\n"
            );
            fails++;
        }

        // Evidencia mas fuerte: la VADD joven termina antes que
        // la VMUL vieja. Con el vec_rs unico esto era imposible.
        if (indep_add_complete < indep_mul_complete) {
            printf(
                "PASS  completion OoO vectorial: "
                "VADD joven termino antes que VMUL vieja\n"
            );
        } else {
            printf(
                "FAIL  VADD no termino antes que VMUL\n"
            );
            fails++;
        }
    }

    if (fails == 0)
        printf("PASS  VRAT RAW + WAW + independientes\n");

    return fails;
}
static int suite_mul_pipeline_raw() {

    printf("\n===============================================================\n");
    printf("  MUL pipeline: dependencia RAW\n");
    printf("===============================================================\n");

    static ap_uint<32> imem[OOO_IMEM_WORDS];
    static ap_uint<32> dmem[OOO_DMEM_WORDS];

    for (int i = 0; i < OOO_IMEM_WORDS; i++) imem[i] = 0;
    for (int i = 0; i < OOO_DMEM_WORDS; i++) dmem[i] = 0;

    auto enc_addi = [](int rd, int rs1, int imm) -> uint32_t {
        return ((uint32_t(imm) & 0xFFFu) << 20) |
               ((uint32_t(rs1) & 0x1Fu) << 15) |
               ((uint32_t(rd)  & 0x1Fu) << 7) |
               0x13u;
    };

    auto enc_mul = [](int rd, int rs1, int rs2) -> uint32_t {
        return (1u << 25) |
               ((uint32_t(rs2) & 0x1Fu) << 20) |
               ((uint32_t(rs1) & 0x1Fu) << 15) |
               ((uint32_t(rd)  & 0x1Fu) << 7) |
               0x33u;
    };

    imem[0] = enc_addi(1, 0, 2);    // pc 0
    imem[1] = enc_addi(2, 0, 3);    // pc 4
    imem[2] = enc_addi(3, 0, 4);    // pc 8

    imem[3] = enc_mul(10, 1, 2);    // pc 12: x10 = 6
    imem[4] = enc_mul(11, 10, 3);   // pc 16: x11 = 24, RAW sobre x10

    imem[5] = 0;

    ap_uint<1> dv;   ap_uint<3> dt;   ap_uint<32> dp;
    ap_uint<1> a0d;  ap_uint<3> a0t;
    ap_uint<1> a1d;  ap_uint<3> a1t;
    ap_uint<1> mdd;  ap_uint<3> mdt;
    ap_uint<1> fpd;  ap_uint<3> fpt;
    ap_uint<1> lsd;  ap_uint<3> lst;
    ap_uint<1> brd;  ap_uint<3> brt;
    ap_uint<1> vcd;  ap_uint<3> vct;

    ap_uint<1>  cv;
    ap_uint<1>  cfp;
    ap_uint<5>  crd;
    ap_uint<32> cval;

    ap_uint<32> vo[OOO_VEC_REGFILE_LEN];
    ap_uint<1> halted;

    riscv_soc_tick(
        1, imem, dmem,
        dv,dt,dp,
        a0d,a0t,a1d,a1t,
        mdd,mdt,fpd,fpt,
        lsd,lst,brd,brt,vcd,vct,
        cv,cfp,crd,cval,
        vo,halted
    );

    int prod_dispatch = -1;
    int cons_dispatch = -1;
    int prod_complete = -1;
    int cons_complete = -1;

    ap_uint<3> prod_tag = 0;
    ap_uint<3> cons_tag = 0;

    bool prod_seen = false;
    bool cons_seen = false;

    uint32_t x10 = 0;
    uint32_t x11 = 0;

    int cycles = 0;

    while (!halted && cycles < 100) {

        cycles++;

        riscv_soc_tick(
            0, imem, dmem,
            dv,dt,dp,
            a0d,a0t,a1d,a1t,
            mdd,mdt,fpd,fpt,
            lsd,lst,brd,brt,vcd,vct,
            cv,cfp,crd,cval,
            vo,halted
        );

        if (dv && dp == 12) {
            prod_dispatch = cycles;
            prod_tag = dt;
            prod_seen = true;
        }

        if (dv && dp == 16) {
            cons_dispatch = cycles;
            cons_tag = dt;
            cons_seen = true;
        }

        if (mdd) {
            if (prod_seen && mdt == prod_tag && prod_complete < 0)
                prod_complete = cycles;

            if (cons_seen && mdt == cons_tag && cons_complete < 0)
                cons_complete = cycles;
        }

        if (cv && !cfp) {
            if (crd == 10) x10 = cval.to_uint();
            if (crd == 11) x11 = cval.to_uint();
        }
    }

    int fails = 0;

    printf(
        "MUL RAW: productor dispatch=%d complete=%d | "
        "consumidor dispatch=%d complete=%d\n",
        prod_dispatch,
        prod_complete,
        cons_dispatch,
        cons_complete
    );

    printf("MUL RAW resultados: x10=%u x11=%u\n", x10, x11);

    if (x10 != 6) {
        printf("FAIL  MUL RAW productor: x10=%u esperado=6\n", x10);
        fails++;
    }

    if (x11 != 24) {
        printf("FAIL  MUL RAW consumidor: x11=%u esperado=24\n", x11);
        fails++;
    }

    if (cons_dispatch > 0 &&
        prod_complete > 0 &&
        cons_dispatch < prod_complete) {

        printf("PASS  consumidor RAW fue despachado antes de completar productor\n");

    } else {

        printf("FAIL  no se observo dependencia RAW en vuelo\n");
        fails++;
    }

    if (cons_complete > prod_complete) {
        printf("PASS  consumidor espero correctamente el resultado del productor\n");
    } else {
        printf("FAIL  orden de completion RAW incorrecto\n");
        fails++;
    }

    if (fails == 0)
        printf("PASS  MUL PIPELINE RAW\n");

    return fails;
}

static int suite_mul_pipeline_directed() {

    printf("\n===============================================================\n");
    printf("  MUL pipeline: 4 multiplicaciones independientes\n");
    printf("===============================================================\n");

    static ap_uint<32> imem[OOO_IMEM_WORDS];
    static ap_uint<32> dmem[OOO_DMEM_WORDS];

    for (int i = 0; i < OOO_IMEM_WORDS; i++) imem[i] = 0;
    for (int i = 0; i < OOO_DMEM_WORDS; i++) dmem[i] = 0;

    auto enc_addi = [](int rd, int rs1, int imm) -> uint32_t {
        return ((uint32_t(imm) & 0xFFFu) << 20) |
               ((uint32_t(rs1) & 0x1Fu) << 15) |
               ((uint32_t(rd)  & 0x1Fu) << 7) |
               0x13u;
    };

    auto enc_mul = [](int rd, int rs1, int rs2) -> uint32_t {
        return (1u << 25) |
               ((uint32_t(rs2) & 0x1Fu) << 20) |
               ((uint32_t(rs1) & 0x1Fu) << 15) |
               ((uint32_t(rd)  & 0x1Fu) << 7) |
               0x33u;
    };

    // Fuentes independientes.
    imem[0] = enc_addi(1, 0, 2);   // pc 0
    imem[1] = enc_addi(2, 0, 3);   // pc 4
    imem[2] = enc_addi(3, 0, 4);   // pc 8
    imem[3] = enc_addi(4, 0, 5);   // pc 12

    // Cuatro MUL consecutivas e independientes.
    imem[4] = enc_mul(10, 1, 2);   // pc 16 ->  6
    imem[5] = enc_mul(11, 2, 3);   // pc 20 -> 12
    imem[6] = enc_mul(12, 3, 4);   // pc 24 -> 20
    imem[7] = enc_mul(13, 1, 4);   // pc 28 -> 10

    imem[8] = 0;                    // halt

    ap_uint<1> dv;   ap_uint<3> dt;   ap_uint<32> dp;
    ap_uint<1> a0d;  ap_uint<3> a0t;
    ap_uint<1> a1d;  ap_uint<3> a1t;
    ap_uint<1> mdd;  ap_uint<3> mdt;
    ap_uint<1> fpd;  ap_uint<3> fpt;
    ap_uint<1> lsd;  ap_uint<3> lst;
    ap_uint<1> brd;  ap_uint<3> brt;
    ap_uint<1> vcd;  ap_uint<3> vct;

    ap_uint<1>  cv;
    ap_uint<1>  cfp;
    ap_uint<5>  crd;
    ap_uint<32> cval;

    ap_uint<32> vo[OOO_VEC_REGFILE_LEN];
    ap_uint<1> halted;

    riscv_soc_tick(
        1, imem, dmem,
        dv,dt,dp,
        a0d,a0t,a1d,a1t,
        mdd,mdt,fpd,fpt,
        lsd,lst,brd,brt,vcd,vct,
        cv,cfp,crd,cval,
        vo,halted
    );

    int disp[4] = {-1,-1,-1,-1};
    int done[4] = {-1,-1,-1,-1};
    int tag_to_mul[8];

    for (int i = 0; i < 8; i++)
        tag_to_mul[i] = -1;

    uint32_t result[4] = {0,0,0,0};
    bool committed[4] = {false,false,false,false};

    int cycles = 0;

    while (!halted && cycles < 100) {

        cycles++;

        riscv_soc_tick(
            0, imem, dmem,
            dv,dt,dp,
            a0d,a0t,a1d,a1t,
            mdd,mdt,fpd,fpt,
            lsd,lst,brd,brt,vcd,vct,
            cv,cfp,crd,cval,
            vo,halted
        );

        if (dv) {

            int idx = -1;

            if      (dp == 16) idx = 0;
            else if (dp == 20) idx = 1;
            else if (dp == 24) idx = 2;
            else if (dp == 28) idx = 3;

            if (idx >= 0) {
                disp[idx] = cycles;
                tag_to_mul[dt.to_uint()] = idx;
            }
        }

        if (mdd) {

            int idx = tag_to_mul[mdt.to_uint()];

            if (idx >= 0 && done[idx] < 0)
                done[idx] = cycles;
        }

        if (cv && !cfp) {

            int rd = crd.to_uint();

            if (rd >= 10 && rd <= 13) {
                int idx = rd - 10;
                result[idx] = cval.to_uint();
                committed[idx] = true;
            }
        }
    }

    int fails = 0;

    const uint32_t expected[4] = {6,12,20,10};

    printf("MUL pipeline: ciclos=%d\n", cycles);

    for (int i = 0; i < 4; i++) {

        printf(
            "MUL%d: dispatch=%d complete=%d result=%u\n",
            i + 1,
            disp[i],
            done[i],
            result[i]
        );

        if (disp[i] < 0 || done[i] < 0 || !committed[i]) {
            printf("FAIL  MUL%d no completo correctamente\n", i + 1);
            fails++;
        }

        if (result[i] != expected[i]) {
            printf(
                "FAIL  MUL%d resultado=%u esperado=%u\n",
                i + 1,
                result[i],
                expected[i]
            );
            fails++;
        }
    }

    bool dispatch_ii1 =
        disp[1] == disp[0] + 1 &&
        disp[2] == disp[1] + 1 &&
        disp[3] == disp[2] + 1;

    bool completion_ii1 =
        done[1] == done[0] + 1 &&
        done[2] == done[1] + 1 &&
        done[3] == done[2] + 1;

    if (dispatch_ii1)
        printf("PASS  MUL pipeline dispatch II=1\n");
    else {
        printf("FAIL  MUL pipeline dispatch no es II=1\n");
        fails++;
    }

    if (completion_ii1)
        printf("PASS  MUL pipeline completion II=1\n");
    else {
        printf("FAIL  MUL pipeline completion no es II=1\n");
        fails++;
    }

    if (disp[1] < done[0] &&
        disp[2] < done[0] &&
        disp[3] < done[0]) {

        printf("PASS  varias MUL estuvieron simultaneamente en vuelo\n");

    } else {

        printf("FAIL  no se demostro overlap de MUL\n");
        fails++;
    }

    if (fails == 0)
        printf("PASS  MUL PIPELINE DIRECTED\n");

    return fails;
}

static int suite_fp_addmul_pipeline_directed() {

    printf("\n===============================================================\n");
    printf("  FPU ADDMUL pipeline: 4 operaciones FP independientes\n");
    printf("===============================================================\n");

    static ap_uint<32> imem[OOO_IMEM_WORDS];
    static ap_uint<32> dmem[OOO_DMEM_WORDS];

    for (int i = 0; i < OOO_IMEM_WORDS; i++) imem[i] = 0;
    for (int i = 0; i < OOO_DMEM_WORDS; i++) dmem[i] = 0;

    auto enc_addi = [](int rd, int rs1, int imm) -> uint32_t {
        return ((uint32_t(imm) & 0xFFFu) << 20) |
               ((uint32_t(rs1) & 0x1Fu) << 15) |
               ((uint32_t(rd)  & 0x1Fu) << 7) |
               0x13u;
    };

    const uint32_t OP_FP = rv32i::Opcode::OP_FP;

    // Preparacion:
    // x1=2, x2=3, x3=4, x4=5
    imem[0] = enc_addi(1, 0, 2);   // pc 0
    imem[1] = enc_addi(2, 0, 3);   // pc 4
    imem[2] = enc_addi(3, 0, 4);   // pc 8
    imem[3] = enc_addi(4, 0, 5);   // pc 12

    // f1=2.0, f2=3.0, f3=4.0, f4=5.0
    imem[4] = r_type(
        OP_FP, 1, 0, 1,
        rv32i::Rs2_FCVT::W,
        rv32i::Funct7_FP::FCVT_S_W
    );                              // pc 16

    imem[5] = r_type(
        OP_FP, 2, 0, 2,
        rv32i::Rs2_FCVT::W,
        rv32i::Funct7_FP::FCVT_S_W
    );                              // pc 20

    imem[6] = r_type(
        OP_FP, 3, 0, 3,
        rv32i::Rs2_FCVT::W,
        rv32i::Funct7_FP::FCVT_S_W
    );                              // pc 24

    imem[7] = r_type(
        OP_FP, 4, 0, 4,
        rv32i::Rs2_FCVT::W,
        rv32i::Funct7_FP::FCVT_S_W
    );                              // pc 28

    // Las cuatro operaciones que queremos demostrar consecutivas.
    imem[8] = r_type(
        OP_FP, 10, 0, 1, 2,
        rv32i::Funct7_FP::FADD_S
    );                              // pc 32: 2+3 = 5

    imem[9] = r_type(
        OP_FP, 11, 0, 2, 3,
        rv32i::Funct7_FP::FMUL_S
    );                              // pc 36: 3*4 = 12

    imem[10] = r_type(
        OP_FP, 12, 0, 4, 1,
        rv32i::Funct7_FP::FSUB_S
    );                              // pc 40: 5-2 = 3

    imem[11] = r_type(
        OP_FP, 13, 0, 3, 4,
        rv32i::Funct7_FP::FADD_S
    );                              // pc 44: 4+5 = 9

    imem[12] = 0x00000073u;         // pc 48: ECALL

    ap_uint<1>  dv;
    ap_uint<3>  dt;
    ap_uint<32> dp;

    ap_uint<1> alu0d; ap_uint<3> alu0t;
    ap_uint<1> alu1d; ap_uint<3> alu1t;
    ap_uint<1> mdd;   ap_uint<3> mdt;
    ap_uint<1> fpud;  ap_uint<3> fput;
    ap_uint<1> lsud;  ap_uint<3> lsut;
    ap_uint<1> brd;   ap_uint<3> brt;
    ap_uint<1> vecd;  ap_uint<3> vect;

    ap_uint<1>  cv;
    ap_uint<1>  cfp;
    ap_uint<5>  crd;
    ap_uint<32> cval;

    ap_uint<32> vregs_out[OOO_VEC_REGFILE_LEN];
    ap_uint<1> halted;

    auto tick = [&](ap_uint<1> reset) {
        riscv_soc_tick(
            reset, imem, dmem,
            dv, dt, dp,
            alu0d, alu0t,
            alu1d, alu1t,
            mdd, mdt,
            fpud, fput,
            lsud, lsut,
            brd, brt,
            vecd, vect,
            cv, cfp, crd, cval,
            vregs_out,
            halted
        );
    };

    tick(1);

    int disp[4] = {-1,-1,-1,-1};
    int done[4] = {-1,-1,-1,-1};
    int tag_to_fp[8];

    for (int i = 0; i < 8; i++)
        tag_to_fp[i] = -1;

    uint32_t result[4] = {0,0,0,0};
    bool committed[4] = {false,false,false,false};

    const uint32_t pcs[4] = {32,36,40,44};

    int cycles = 0;

    for (cycles = 1; cycles <= 120; cycles++) {

        tick(0);

        if (dv) {
            for (int i = 0; i < 4; i++) {
                if (dp.to_uint() == pcs[i]) {
                    disp[i] = cycles;
                    tag_to_fp[dt.to_uint()] = i;
                }
            }
        }

        if (fpud) {
            int idx = tag_to_fp[fput.to_uint()];

            if (idx >= 0 && done[idx] < 0)
                done[idx] = cycles;
        }

        if (cv && cfp) {
            int rd = crd.to_uint();

            if (rd >= 10 && rd <= 13) {
                int idx = rd - 10;

                result[idx] = cval.to_uint();
                committed[idx] = true;
            }
        }

        if (committed[0] &&
            committed[1] &&
            committed[2] &&
            committed[3])
            break;
    }

    const uint32_t expected[4] = {
        0x40A00000u, // 5.0
        0x41400000u, // 12.0
        0x40400000u, // 3.0
        0x41100000u  // 9.0
    };

    int fails = 0;

    printf("FPU ADDMUL pipeline: ciclos=%d\n", cycles);

    for (int i = 0; i < 4; i++) {

        printf(
            "FP%d: dispatch=%d complete=%d result=0x%08X\n",
            i + 1,
            disp[i],
            done[i],
            result[i]
        );

        if (disp[i] < 0 || done[i] < 0 || !committed[i]) {
            printf("FAIL  FP%d no completo correctamente\n", i + 1);
            fails++;
        }

        if (result[i] != expected[i]) {
            printf(
                "FAIL  FP%d resultado=0x%08X esperado=0x%08X\n",
                i + 1,
                result[i],
                expected[i]
            );
            fails++;
        }
    }

    bool dispatch_ii1 =
        disp[1] == disp[0] + 1 &&
        disp[2] == disp[1] + 1 &&
        disp[3] == disp[2] + 1;

    bool completion_ii1 =
        done[1] == done[0] + 1 &&
        done[2] == done[1] + 1 &&
        done[3] == done[2] + 1;

    if (dispatch_ii1)
        printf("PASS  FPU ADDMUL pipeline dispatch II=1\n");
    else {
        printf("FAIL  FPU ADDMUL pipeline dispatch no es II=1\n");
        fails++;
    }

    if (completion_ii1)
        printf("PASS  FPU ADDMUL pipeline completion II=1\n");
    else {
        printf("FAIL  FPU ADDMUL pipeline completion no es II=1\n");
        fails++;
    }

    if (disp[1] < done[0] &&
        disp[2] < done[0] &&
        disp[3] < done[0]) {

        printf("PASS  varias operaciones FP estuvieron simultaneamente en vuelo\n");

    } else {
        printf("FAIL  no se demostro overlap de operaciones FP\n");
        fails++;
    }

    if (fails == 0)
        printf("PASS  FPU ADDMUL PIPELINE DIRECTED\n");

    return fails;
}

static int suite_fp_addmul_pipeline_raw() {

    printf("\n===============================================================\n");
    printf("  FPU ADDMUL pipeline: dependencia RAW\n");
    printf("===============================================================\n");

    static ap_uint<32> imem[OOO_IMEM_WORDS];
    static ap_uint<32> dmem[OOO_DMEM_WORDS];

    for (int i = 0; i < OOO_IMEM_WORDS; i++) imem[i] = 0;
    for (int i = 0; i < OOO_DMEM_WORDS; i++) dmem[i] = 0;

    auto enc_addi = [](int rd, int rs1, int imm) -> uint32_t {
        return ((uint32_t(imm) & 0xFFFu) << 20) |
               ((uint32_t(rs1) & 0x1Fu) << 15) |
               ((uint32_t(rd)  & 0x1Fu) << 7) |
               0x13u;
    };

    const uint32_t OP_FP = rv32i::Opcode::OP_FP;

    // x1=2, x2=3, x3=4
    imem[0] = enc_addi(1, 0, 2);    // pc 0
    imem[1] = enc_addi(2, 0, 3);    // pc 4
    imem[2] = enc_addi(3, 0, 4);    // pc 8

    // f1=2.0, f2=3.0, f3=4.0
    imem[3] = r_type(
        OP_FP, 1, 0, 1,
        rv32i::Rs2_FCVT::W,
        rv32i::Funct7_FP::FCVT_S_W
    );                               // pc 12

    imem[4] = r_type(
        OP_FP, 2, 0, 2,
        rv32i::Rs2_FCVT::W,
        rv32i::Funct7_FP::FCVT_S_W
    );                               // pc 16

    imem[5] = r_type(
        OP_FP, 3, 0, 3,
        rv32i::Rs2_FCVT::W,
        rv32i::Funct7_FP::FCVT_S_W
    );                               // pc 20

    // Productor:
    // f10 = 2.0 + 3.0 = 5.0
    imem[6] = r_type(
        OP_FP, 10, 0, 1, 2,
        rv32i::Funct7_FP::FADD_S
    );                               // pc 24

    // Consumidor RAW:
    // f11 = f10 * 4.0 = 20.0
    imem[7] = r_type(
        OP_FP, 11, 0, 10, 3,
        rv32i::Funct7_FP::FMUL_S
    );                               // pc 28

    imem[8] = 0x00000073u;           // ECALL

    ap_uint<1>  dv;
    ap_uint<3>  dt;
    ap_uint<32> dp;

    ap_uint<1> alu0d; ap_uint<3> alu0t;
    ap_uint<1> alu1d; ap_uint<3> alu1t;
    ap_uint<1> mdd;   ap_uint<3> mdt;
    ap_uint<1> fpud;  ap_uint<3> fput;
    ap_uint<1> lsud;  ap_uint<3> lsut;
    ap_uint<1> brd;   ap_uint<3> brt;
    ap_uint<1> vecd;  ap_uint<3> vect;

    ap_uint<1>  cv;
    ap_uint<1>  cfp;
    ap_uint<5>  crd;
    ap_uint<32> cval;

    ap_uint<32> vregs_out[OOO_VEC_REGFILE_LEN];
    ap_uint<1> halted;

    auto tick = [&](ap_uint<1> reset) {
        riscv_soc_tick(
            reset, imem, dmem,
            dv, dt, dp,
            alu0d, alu0t,
            alu1d, alu1t,
            mdd, mdt,
            fpud, fput,
            lsud, lsut,
            brd, brt,
            vecd, vect,
            cv, cfp, crd, cval,
            vregs_out,
            halted
        );
    };

    tick(1);

    int prod_dispatch = -1;
    int cons_dispatch = -1;

    int prod_complete = -1;
    int cons_complete = -1;

    ap_uint<3> prod_tag = 0;
    ap_uint<3> cons_tag = 0;

    bool prod_seen = false;
    bool cons_seen = false;

    uint32_t f10_result = 0;
    uint32_t f11_result = 0;

    bool f10_committed = false;
    bool f11_committed = false;

    int cycles = 0;

    for (cycles = 1; cycles <= 100; cycles++) {

        tick(0);

        if (dv && dp == 24) {
            prod_dispatch = cycles;
            prod_tag = dt;
            prod_seen = true;
        }

        if (dv && dp == 28) {
            cons_dispatch = cycles;
            cons_tag = dt;
            cons_seen = true;
        }

        if (fpud) {

            if (prod_seen &&
                fput == prod_tag &&
                prod_complete < 0)
                prod_complete = cycles;

            if (cons_seen &&
                fput == cons_tag &&
                cons_complete < 0)
                cons_complete = cycles;
        }

        if (cv && cfp) {

            if (crd == 10) {
                f10_result = cval.to_uint();
                f10_committed = true;
            }

            if (crd == 11) {
                f11_result = cval.to_uint();
                f11_committed = true;
            }
        }

        if (f10_committed && f11_committed)
            break;
    }

    int fails = 0;

    printf(
        "FPU RAW: productor dispatch=%d complete=%d | "
        "consumidor dispatch=%d complete=%d\n",
        prod_dispatch,
        prod_complete,
        cons_dispatch,
        cons_complete
    );

    printf(
        "FPU RAW resultados: f10=0x%08X f11=0x%08X\n",
        f10_result,
        f11_result
    );

    if (f10_result != 0x40A00000u) {
        printf(
            "FAIL  FPU RAW productor: f10=0x%08X esperado=0x40A00000\n",
            f10_result
        );
        fails++;
    }

    if (f11_result != 0x41A00000u) {
        printf(
            "FAIL  FPU RAW consumidor: f11=0x%08X esperado=0x41A00000\n",
            f11_result
        );
        fails++;
    }

    if (prod_dispatch < 0 ||
        cons_dispatch < 0 ||
        prod_complete < 0 ||
        cons_complete < 0) {

        printf("FAIL  faltaron eventos dispatch/completion FPU RAW\n");
        fails++;
    }

    if (cons_dispatch > 0 &&
        prod_complete > 0 &&
        cons_dispatch < prod_complete) {

        printf(
            "PASS  consumidor FP RAW fue despachado antes de completar productor\n"
        );

    } else {
        printf(
            "FAIL  consumidor FP RAW no estuvo pendiente mientras productor estaba en vuelo\n"
        );
        fails++;
    }

    if (cons_complete > prod_complete) {

        printf(
            "PASS  consumidor FP RAW espero correctamente el resultado del productor\n"
        );

    } else {
        printf("FAIL  orden de completion FP RAW incorrecto\n");
        fails++;
    }

    if (fails == 0)
        printf("PASS  FPU ADDMUL PIPELINE RAW\n");

    return fails;
}

static int suite_fp_divsqrt_pipeline_directed() {

    printf("\n===============================================================\n");
    printf("  FPU DIVSQRT pipeline: 4 operaciones independientes\n");
    printf("===============================================================\n");

    static ap_uint<32> imem[OOO_IMEM_WORDS];
    static ap_uint<32> dmem[OOO_DMEM_WORDS];

    for (int i = 0; i < OOO_IMEM_WORDS; i++) imem[i] = 0;
    for (int i = 0; i < OOO_DMEM_WORDS; i++) dmem[i] = 0;

    auto enc_addi = [](int rd, int rs1, int imm) -> uint32_t {
        return ((uint32_t(imm) & 0xFFFu) << 20) |
               ((uint32_t(rs1) & 0x1Fu) << 15) |
               ((uint32_t(rd)  & 0x1Fu) << 7) |
               0x13u;
    };

    const uint32_t OP_FP = rv32i::Opcode::OP_FP;

    // Enteros fuente.
    imem[0] = enc_addi(1, 0, 8);    // pc 0
    imem[1] = enc_addi(2, 0, 2);    // pc 4
    imem[2] = enc_addi(3, 0, 9);    // pc 8
    imem[3] = enc_addi(4, 0, 3);    // pc 12
    imem[4] = enc_addi(5, 0, 16);   // pc 16

    // f1=8.0
    imem[5] = r_type(
        OP_FP, 1, 0, 1,
        rv32i::Rs2_FCVT::W,
        rv32i::Funct7_FP::FCVT_S_W
    );                               // pc 20

    // f2=2.0
    imem[6] = r_type(
        OP_FP, 2, 0, 2,
        rv32i::Rs2_FCVT::W,
        rv32i::Funct7_FP::FCVT_S_W
    );                               // pc 24

    // f3=9.0
    imem[7] = r_type(
        OP_FP, 3, 0, 3,
        rv32i::Rs2_FCVT::W,
        rv32i::Funct7_FP::FCVT_S_W
    );                               // pc 28

    // f4=3.0
    imem[8] = r_type(
        OP_FP, 4, 0, 4,
        rv32i::Rs2_FCVT::W,
        rv32i::Funct7_FP::FCVT_S_W
    );                               // pc 32

    // f5=16.0
    imem[9] = r_type(
        OP_FP, 5, 0, 5,
        rv32i::Rs2_FCVT::W,
        rv32i::Funct7_FP::FCVT_S_W
    );                               // pc 36

    // FDIV f10 = f1 / f2 = 4.0
    imem[10] = r_type(
        OP_FP, 10, 0, 1, 2,
        rv32i::Funct7_FP::FDIV_S
    );                               // pc 40

    // FSQRT f11 = sqrt(f3) = 3.0
    imem[11] = r_type(
        OP_FP, 11, 0, 3, 0,
        rv32i::Funct7_FP::FSQRT_S
    );                               // pc 44

    // FDIV f12 = f3 / f4 = 3.0
    imem[12] = r_type(
        OP_FP, 12, 0, 3, 4,
        rv32i::Funct7_FP::FDIV_S
    );                               // pc 48

    // FSQRT f13 = sqrt(f5) = 4.0
    imem[13] = r_type(
        OP_FP, 13, 0, 5, 0,
        rv32i::Funct7_FP::FSQRT_S
    );                               // pc 52

    imem[14] = 0x00000073u;          // pc 56: ECALL

    ap_uint<1>  dv;
    ap_uint<3>  dt;
    ap_uint<32> dp;

    ap_uint<1> alu0d; ap_uint<3> alu0t;
    ap_uint<1> alu1d; ap_uint<3> alu1t;
    ap_uint<1> mdd;   ap_uint<3> mdt;
    ap_uint<1> fpud;  ap_uint<3> fput;
    ap_uint<1> lsud;  ap_uint<3> lsut;
    ap_uint<1> brd;   ap_uint<3> brt;
    ap_uint<1> vecd;  ap_uint<3> vect;

    ap_uint<1>  cv;
    ap_uint<1>  cfp;
    ap_uint<5>  crd;
    ap_uint<32> cval;

    ap_uint<32> vregs_out[OOO_VEC_REGFILE_LEN];
    ap_uint<1> halted;

    auto tick = [&](ap_uint<1> reset) {
        riscv_soc_tick(
            reset, imem, dmem,
            dv, dt, dp,
            alu0d, alu0t,
            alu1d, alu1t,
            mdd, mdt,
            fpud, fput,
            lsud, lsut,
            brd, brt,
            vecd, vect,
            cv, cfp, crd, cval,
            vregs_out,
            halted
        );
    };

    tick(1);

    int disp[4] = {-1,-1,-1,-1};
    int done[4] = {-1,-1,-1,-1};

    int tag_to_fp[8];

    for (int i = 0; i < 8; i++)
        tag_to_fp[i] = -1;

    uint32_t result[4] = {0,0,0,0};
    bool committed[4] = {false,false,false,false};

    const uint32_t pcs[4] = {
        40, 44, 48, 52
    };

    int cycles = 0;

    for (cycles = 1; cycles <= 160; cycles++) {

        tick(0);

        if (dv) {

            for (int i = 0; i < 4; i++) {

                if (dp.to_uint() == pcs[i]) {

                    disp[i] = cycles;
                    tag_to_fp[dt.to_uint()] = i;
                }
            }
        }

        if (fpud) {

            int idx =
                tag_to_fp[fput.to_uint()];

            if (idx >= 0 && done[idx] < 0)
                done[idx] = cycles;
        }

        if (cv && cfp) {

            int rd = crd.to_uint();

            if (rd >= 10 && rd <= 13) {

                int idx = rd - 10;

                result[idx] =
                    cval.to_uint();

                committed[idx] = true;
            }
        }

        if (committed[0] &&
            committed[1] &&
            committed[2] &&
            committed[3])
            break;
    }

    const uint32_t expected[4] = {

        0x40800000u, // 4.0
        0x40400000u, // 3.0
        0x40400000u, // 3.0
        0x40800000u  // 4.0
    };

    int fails = 0;

    printf(
        "FPU DIVSQRT pipeline: ciclos=%d\n",
        cycles
    );

    for (int i = 0; i < 4; i++) {

        printf(
            "DIVSQRT%d: dispatch=%d complete=%d result=0x%08X\n",
            i + 1,
            disp[i],
            done[i],
            result[i]
        );

        if (disp[i] < 0 ||
            done[i] < 0 ||
            !committed[i]) {

            printf(
                "FAIL  DIVSQRT%d no completo correctamente\n",
                i + 1
            );

            fails++;
        }

        if (result[i] != expected[i]) {

            printf(
                "FAIL  DIVSQRT%d resultado=0x%08X esperado=0x%08X\n",
                i + 1,
                result[i],
                expected[i]
            );

            fails++;
        }
    }

    bool dispatch_ii1 =
        disp[1] == disp[0] + 1 &&
        disp[2] == disp[1] + 1 &&
        disp[3] == disp[2] + 1;

    bool completion_ii1 =
        done[1] == done[0] + 1 &&
        done[2] == done[1] + 1 &&
        done[3] == done[2] + 1;

    if (dispatch_ii1) {

        printf(
            "PASS  FPU DIVSQRT pipeline dispatch II=1\n"
        );

    } else {

        printf(
            "FAIL  FPU DIVSQRT pipeline dispatch no es II=1\n"
        );

        fails++;
    }

    if (completion_ii1) {

        printf(
            "PASS  FPU DIVSQRT pipeline completion II=1\n"
        );

    } else {

        printf(
            "FAIL  FPU DIVSQRT pipeline completion no es II=1\n"
        );

        fails++;
    }

    if (disp[1] < done[0] &&
        disp[2] < done[0] &&
        disp[3] < done[0]) {

        printf(
            "PASS  varias operaciones DIVSQRT estuvieron simultaneamente en vuelo\n"
        );

    } else {

        printf(
            "FAIL  no se demostro overlap de DIVSQRT\n"
        );

        fails++;
    }

    if (fails == 0)
        printf(
            "PASS  FPU DIVSQRT PIPELINE DIRECTED\n"
        );

    return fails;
}

static int suite_fp_divsqrt_pipeline_raw() {

    printf("\n===============================================================\n");
    printf("  FPU DIVSQRT pipeline: dependencia RAW\n");
    printf("===============================================================\n");

    static ap_uint<32> imem[OOO_IMEM_WORDS];
    static ap_uint<32> dmem[OOO_DMEM_WORDS];

    for (int i = 0; i < OOO_IMEM_WORDS; i++) imem[i] = 0;
    for (int i = 0; i < OOO_DMEM_WORDS; i++) dmem[i] = 0;

    auto enc_addi = [](int rd, int rs1, int imm) -> uint32_t {
        return ((uint32_t(imm) & 0xFFFu) << 20) |
               ((uint32_t(rs1) & 0x1Fu) << 15) |
               ((uint32_t(rd)  & 0x1Fu) << 7) |
               0x13u;
    };

    const uint32_t OP_FP = rv32i::Opcode::OP_FP;

    // x1 = 8
    // x2 = 2
    imem[0] = enc_addi(1, 0, 8);     // pc 0
    imem[1] = enc_addi(2, 0, 2);     // pc 4

    // f1 = 8.0
    imem[2] = r_type(
        OP_FP, 1, 0, 1,
        rv32i::Rs2_FCVT::W,
        rv32i::Funct7_FP::FCVT_S_W
    );                               // pc 8

    // f2 = 2.0
    imem[3] = r_type(
        OP_FP, 2, 0, 2,
        rv32i::Rs2_FCVT::W,
        rv32i::Funct7_FP::FCVT_S_W
    );                               // pc 12

    // Productor:
    // f10 = 8.0 / 2.0 = 4.0
    imem[4] = r_type(
        OP_FP, 10, 0, 1, 2,
        rv32i::Funct7_FP::FDIV_S
    );                               // pc 16

    // Consumidor RAW:
    // f11 = sqrt(f10) = 2.0
    imem[5] = r_type(
        OP_FP, 11, 0, 10, 0,
        rv32i::Funct7_FP::FSQRT_S
    );                               // pc 20

    imem[6] = 0x00000073u;           // ECALL

    ap_uint<1>  dv;
    ap_uint<3>  dt;
    ap_uint<32> dp;

    ap_uint<1> alu0d; ap_uint<3> alu0t;
    ap_uint<1> alu1d; ap_uint<3> alu1t;
    ap_uint<1> mdd;   ap_uint<3> mdt;
    ap_uint<1> fpud;  ap_uint<3> fput;
    ap_uint<1> lsud;  ap_uint<3> lsut;
    ap_uint<1> brd;   ap_uint<3> brt;
    ap_uint<1> vecd;  ap_uint<3> vect;

    ap_uint<1>  cv;
    ap_uint<1>  cfp;
    ap_uint<5>  crd;
    ap_uint<32> cval;

    ap_uint<32> vregs_out[OOO_VEC_REGFILE_LEN];
    ap_uint<1> halted;

    auto tick = [&](ap_uint<1> reset) {
        riscv_soc_tick(
            reset, imem, dmem,
            dv, dt, dp,
            alu0d, alu0t,
            alu1d, alu1t,
            mdd, mdt,
            fpud, fput,
            lsud, lsut,
            brd, brt,
            vecd, vect,
            cv, cfp, crd, cval,
            vregs_out,
            halted
        );
    };

    tick(1);

    int prod_dispatch = -1;
    int cons_dispatch = -1;

    int prod_complete = -1;
    int cons_complete = -1;

    ap_uint<3> prod_tag = 0;
    ap_uint<3> cons_tag = 0;

    bool prod_seen = false;
    bool cons_seen = false;

    uint32_t f10_result = 0;
    uint32_t f11_result = 0;

    bool f10_committed = false;
    bool f11_committed = false;

    int cycles = 0;

    for (cycles = 1; cycles <= 140; cycles++) {

        tick(0);

        if (dv && dp == 16) {
            prod_dispatch = cycles;
            prod_tag = dt;
            prod_seen = true;
        }

        if (dv && dp == 20) {
            cons_dispatch = cycles;
            cons_tag = dt;
            cons_seen = true;
        }

        if (fpud) {

            if (prod_seen &&
                fput == prod_tag &&
                prod_complete < 0) {

                prod_complete = cycles;
            }

            if (cons_seen &&
                fput == cons_tag &&
                cons_complete < 0) {

                cons_complete = cycles;
            }
        }

        if (cv && cfp) {

            if (crd == 10) {
                f10_result = cval.to_uint();
                f10_committed = true;
            }

            if (crd == 11) {
                f11_result = cval.to_uint();
                f11_committed = true;
            }
        }

        if (f10_committed && f11_committed)
            break;
    }

    int fails = 0;

    printf(
        "FPU DIVSQRT RAW: productor dispatch=%d complete=%d | "
        "consumidor dispatch=%d complete=%d\n",
        prod_dispatch,
        prod_complete,
        cons_dispatch,
        cons_complete
    );

    printf(
        "FPU DIVSQRT RAW resultados: f10=0x%08X f11=0x%08X\n",
        f10_result,
        f11_result
    );

    // 4.0
    if (f10_result != 0x40800000u) {

        printf(
            "FAIL  FPU DIVSQRT RAW productor: "
            "f10=0x%08X esperado=0x40800000\n",
            f10_result
        );

        fails++;
    }

    // 2.0
    if (f11_result != 0x40000000u) {

        printf(
            "FAIL  FPU DIVSQRT RAW consumidor: "
            "f11=0x%08X esperado=0x40000000\n",
            f11_result
        );

        fails++;
    }

    if (prod_dispatch < 0 ||
        cons_dispatch < 0 ||
        prod_complete < 0 ||
        cons_complete < 0) {

        printf(
            "FAIL  faltaron eventos dispatch/completion DIVSQRT RAW\n"
        );

        fails++;
    }

    // El consumidor debe poder entrar al OoO antes de que
    // el productor haya producido el dato.
    if (cons_dispatch > 0 &&
        prod_complete > 0 &&
        cons_dispatch < prod_complete) {

        printf(
            "PASS  consumidor DIVSQRT RAW fue despachado antes "
            "de completar productor\n"
        );

    } else {

        printf(
            "FAIL  consumidor DIVSQRT RAW no estuvo pendiente "
            "mientras productor estaba en vuelo\n"
        );

        fails++;
    }

    // Pero solo puede terminar despues de que el productor
    // haya generado y difundido el resultado.
    if (cons_complete > prod_complete) {

        printf(
            "PASS  consumidor DIVSQRT RAW espero correctamente "
            "el resultado del productor\n"
        );

    } else {

        printf(
            "FAIL  orden de completion DIVSQRT RAW incorrecto\n"
        );

        fails++;
    }

    if (fails == 0)
        printf("PASS  FPU DIVSQRT PIPELINE RAW\n");

    return fails;
}

static int suite_vrat_legacy_barrier() {

    printf("\n===============================================================\n");
    printf("  VRAT: productor renombrado -> consumidor legacy\n");
    printf("===============================================================\n");

    ap_uint<32> imem[OOO_IMEM_WORDS] = {0};
    ap_uint<32> dmem[OOO_DMEM_WORDS] = {0};

    // v1 = {1,2,3,4}
    dmem[0] = 1;
    dmem[1] = 2;
    dmem[2] = 3;
    dmem[3] = 4;

    // v2 = {10,20,30,40}
    dmem[4] = 10;
    dmem[5] = 20;
    dmem[6] = 30;
    dmem[7] = 40;

    // Valor arquitectonico VIEJO de v3.
    dmem[12] = 100;
    dmem[13] = 100;
    dmem[14] = 100;
    dmem[15] = 100;

    // v0 = mascara completamente activa.
    dmem[16] = 0xFFFFFFFFu;
    dmem[17] = 0xFFFFFFFFu;
    dmem[18] = 0xFFFFFFFFu;
    dmem[19] = 0xFFFFFFFFu;

    const uint32_t F6_ADD = 0b000000;
    const uint32_t F6_MUL = 0b100101;

    std::vector<uint16_t> prog;

    auto push32 = [&](uint32_t w) {
        prog.push_back(w & 0xFFFF);
        prog.push_back(w >> 16);
    };

    auto push16 = [&](uint16_t h) {
        prog.push_back(h);
    };

    // pc 0: VL=4, SEW=32, LMUL=1
    push32(enc_vsetvli(1, 0, VTYPE_E32_M1));

    // pc 4: v1
    push32(enc_vec_mem(
        rv32i::Opcode::LOAD_FP, 1, 0
    ));

    // pc 8
    push32(i_type(
        rv32i::Opcode::OP_IMM,
        2,
        F3A::ADD_SUB,
        0,
        16
    ));

    // pc 12: v2
    push32(enc_vec_mem(
        rv32i::Opcode::LOAD_FP, 2, 2
    ));

    // pc 16: x3 = 48
    push32(i_type(
        rv32i::Opcode::OP_IMM,
        3,
        F3A::ADD_SUB,
        0,
        48
    ));

    // pc 20: v3 VIEJO = {100,100,100,100}
    push32(enc_vec_mem(
        rv32i::Opcode::LOAD_FP, 3, 3
    ));

    // pc 24: x4 = 64
    push32(i_type(
        rv32i::Opcode::OP_IMM,
        4,
        F3A::ADD_SUB,
        0,
        64
    ));

    // pc 28: cargar v0 con todos los bits activos
    push32(enc_vec_mem(
        rv32i::Opcode::LOAD_FP, 0, 4
    ));

    // pc 32:
    // Productor RENOMBRADO.
    // v3 = v1 * v2 = {10,40,90,160}
    push32(enc_op_v(
        F6_MUL,
        1,
        2,
        rvv_f3_opmvv,
        3
    ));

    // pc 36:
    // vm=0 => ruta legacy.
    // v4 debe leer el v3 NUEVO, no {100,100,100,100}.
    push32(enc_op_v_m(
        F6_ADD,
        3,
        2,
        rvv_f3_opivv,
        4,
        0
    ));

    push16(0x0000);

    for (size_t i = 0; i < prog.size(); i++) {

        uint32_t w = imem[i / 2].to_uint();

        if ((i & 1) == 0)
            w = (w & 0xFFFF0000u) | prog[i];
        else
            w = (w & 0x0000FFFFu) |
                (uint32_t(prog[i]) << 16);

        imem[i / 2] = w;
    }

    ap_uint<1> dv;
    ap_uint<3> dt;
    ap_uint<32> dp;

    ap_uint<1> a0d,a1d,mdd,fpd,lsd,brd,vcd;
    ap_uint<3> a0t,a1t,mdt,fpt,lst,brt,vct;

    ap_uint<1> cv,cfp;
    ap_uint<5> crd;
    ap_uint<32> cval;

    static ap_uint<32> vo[OOO_VEC_REGFILE_LEN];

    ap_uint<1> halted;

    riscv_soc_tick(
        1, imem, dmem,
        dv,dt,dp,
        a0d,a0t,a1d,a1t,
        mdd,mdt,fpd,fpt,
        lsd,lst,brd,brt,vcd,vct,
        cv,cfp,crd,cval,
        vo,halted
    );

    int cycles = 0;

    int prod_dispatch = -1;
    int legacy_dispatch = -1;

    int prod_complete = -1;
    int legacy_complete = -1;

    int v3_commit_visible = -1;

    ap_uint<3> prod_tag = 0;
    ap_uint<3> legacy_tag = 0;

    bool prod_seen = false;
    bool legacy_seen = false;

    while (!halted && cycles < 300) {

        cycles++;

        riscv_soc_tick(
            0, imem, dmem,
            dv,dt,dp,
            a0d,a0t,a1d,a1t,
            mdd,mdt,fpd,fpt,
            lsd,lst,brd,brt,vcd,vct,
            cv,cfp,crd,cval,
            vo,halted
        );

        if (dv && dp == 32) {
            prod_dispatch = cycles;
            prod_tag = dt;
            prod_seen = true;
        }

        if (dv && dp == 36) {
            legacy_dispatch = cycles;
            legacy_tag = dt;
            legacy_seen = true;
        }

        if (vcd) {

            if (prod_seen &&
                vct == prod_tag &&
                prod_complete < 0) {

                prod_complete = cycles;
            }

            if (legacy_seen &&
                vct == legacy_tag &&
                legacy_complete < 0) {

                legacy_complete = cycles;
            }
        }

        // Momento en que el resultado especulativo del VMUL
        // se vuelve visible en el banco arquitectonico vregs.
        if (v3_commit_visible < 0 &&
            vo[3 * OOO_VEC_LANES + 0] == 10 &&
            vo[3 * OOO_VEC_LANES + 1] == 40 &&
            vo[3 * OOO_VEC_LANES + 2] == 90 &&
            vo[3 * OOO_VEC_LANES + 3] == 160) {

            v3_commit_visible = cycles;
        }
    }

    int fails = 0;

    printf(
        "VRAT legacy barrier: VMUL dispatch=%d complete=%d "
        "v3-visible=%d | legacy dispatch=%d complete=%d\n",
        prod_dispatch,
        prod_complete,
        v3_commit_visible,
        legacy_dispatch,
        legacy_complete
    );

    uint32_t expect_v3[4] = {
        10, 40, 90, 160
    };

    uint32_t expect_v4[4] = {
        20, 60, 120, 200
    };

    for (int lane = 0; lane < 4; lane++) {

        uint32_t got3 =
            vo[3 * OOO_VEC_LANES + lane].to_uint();

        uint32_t got4 =
            vo[4 * OOO_VEC_LANES + lane].to_uint();

        if (got3 != expect_v3[lane]) {

            printf(
                "FAIL  VRAT legacy productor v3[%d]=%u esperado=%u\n",
                lane,
                got3,
                expect_v3[lane]
            );

            fails++;
        }

        if (got4 != expect_v4[lane]) {

            printf(
                "FAIL  VRAT legacy consumidor v4[%d]=%u esperado=%u\n",
                lane,
                got4,
                expect_v4[lane]
            );

            fails++;
        }
    }

    if (legacy_dispatch >= 0 &&
        v3_commit_visible >= 0 &&
        legacy_dispatch < v3_commit_visible) {

        printf(
            "PASS  legacy fue despachada mientras v3 nuevo aun no "
            "era arquitectonicamente visible\n"
        );

    } else {

        printf(
            "FAIL  no se demostro espera del consumidor legacy\n"
        );

        fails++;
    }

    if (legacy_complete > v3_commit_visible) {

        printf(
            "PASS  legacy completo despues del commit visible de v3\n"
        );

    } else {

        printf(
            "FAIL  legacy completo antes de que v3 nuevo fuera visible\n"
        );

        fails++;
    }

    if (prod_complete >= 0 &&
        legacy_dispatch >= 0 &&
        legacy_dispatch < prod_complete) {

        printf(
            "PASS  legacy entro al OoO mientras VMUL seguia en vuelo\n"
        );

    } else {

        printf(
            "FAIL  no se observo overlap de dispatch productor/legacy\n"
        );

        fails++;
    }

    if (!halted) {
        printf("FAIL  VRAT legacy barrier no termino\n");
        fails++;
    }

    if (fails == 0)
        printf("PASS  VRAT LEGACY BARRIER\n");

    return fails;
}

static int suite_vrat_war() {

    printf("\n===============================================================\n");
    printf("  VRAT: dependencia WAR explicita\n");
    printf("===============================================================\n");

    ap_uint<32> imem[OOO_IMEM_WORDS] = {0};
    ap_uint<32> dmem[OOO_DMEM_WORDS] = {0};

    // v1 VIEJO = {1,2,3,4}
    dmem[0] = 1;
    dmem[1] = 2;
    dmem[2] = 3;
    dmem[3] = 4;

    // v2 = {10,20,30,40}
    dmem[4] = 10;
    dmem[5] = 20;
    dmem[6] = 30;
    dmem[7] = 40;

    const uint32_t F6_ADD = 0b000000;
    const uint32_t F6_MUL = 0b100101;

    std::vector<uint16_t> prog;

    auto push32 = [&](uint32_t w) {
        prog.push_back(w & 0xFFFF);
        prog.push_back(w >> 16);
    };

    auto push16 = [&](uint16_t h) {
        prog.push_back(h);
    };

    // pc 0: VL=4, e32,m1
    push32(enc_vsetvli(1, 0, VTYPE_E32_M1));

    // pc 4: v1 = {1,2,3,4}
    push32(enc_vec_mem(
        rv32i::Opcode::LOAD_FP,
        1,
        0
    ));

    // pc 8: x2 = 16 bytes
    push32(i_type(
        rv32i::Opcode::OP_IMM,
        2,
        F3A::ADD_SUB,
        0,
        16
    ));

    // pc 12: v2 = {10,20,30,40}
    push32(enc_vec_mem(
        rv32i::Opcode::LOAD_FP,
        2,
        2
    ));

    // ------------------------------------------------------------
    // WAR
    //
    // Vieja:
    //   VMUL v3 = v1 * v2
    //   lee v1
    //
    // Joven:
    //   VADD v1 = v2 + v2
    //   escribe v1
    //
    // La joven puede completar antes, pero NO puede alterar
    // el valor de v1 que observa la vieja.
    // ------------------------------------------------------------

    // pc 16
    push32(enc_op_v(
        F6_MUL,
        1,
        2,
        rvv_f3_opmvv,
        3
    ));

    // pc 20
    push32(enc_op_v(
        F6_ADD,
        2,
        2,
        rvv_f3_opivv,
        1
    ));

    push16(0x0000);

    for (size_t i = 0; i < prog.size(); i++) {

        uint32_t w = imem[i / 2].to_uint();

        if ((i & 1) == 0)
            w = (w & 0xFFFF0000u) | prog[i];
        else
            w = (w & 0x0000FFFFu) |
                (uint32_t(prog[i]) << 16);

        imem[i / 2] = w;
    }

    ap_uint<1> dv;
    ap_uint<3> dt;
    ap_uint<32> dp;

    ap_uint<1> a0d,a1d,mdd,fpd,lsd,brd,vcd;
    ap_uint<3> a0t,a1t,mdt,fpt,lst,brt,vct;

    ap_uint<1> cv,cfp;
    ap_uint<5> crd;
    ap_uint<32> cval;

    static ap_uint<32> vo[OOO_VEC_REGFILE_LEN];
    ap_uint<1> halted;

    riscv_soc_tick(
        1, imem, dmem,
        dv,dt,dp,
        a0d,a0t,a1d,a1t,
        mdd,mdt,fpd,fpt,
        lsd,lst,brd,brt,vcd,vct,
        cv,cfp,crd,cval,
        vo,halted
    );

    int cycles = 0;

    int old_dispatch = -1;
    int young_dispatch = -1;

    int old_complete = -1;
    int young_complete = -1;

    ap_uint<3> old_tag = 0;
    ap_uint<3> young_tag = 0;

    bool old_seen = false;
    bool young_seen = false;

    while (!halted && cycles < 300) {

        cycles++;

        riscv_soc_tick(
            0, imem, dmem,
            dv,dt,dp,
            a0d,a0t,a1d,a1t,
            mdd,mdt,fpd,fpt,
            lsd,lst,brd,brt,vcd,vct,
            cv,cfp,crd,cval,
            vo,halted
        );

        // VMUL vieja.
        if (dv && dp == 16) {
            old_dispatch = cycles;
            old_tag = dt;
            old_seen = true;
        }

        // VADD joven que sobrescribe v1.
        if (dv && dp == 20) {
            young_dispatch = cycles;
            young_tag = dt;
            young_seen = true;
        }

        if (vcd) {

            if (old_seen &&
                vct == old_tag &&
                old_complete < 0) {

                old_complete = cycles;
            }

            if (young_seen &&
                vct == young_tag &&
                young_complete < 0) {

                young_complete = cycles;
            }
        }
    }

    int fails = 0;

    printf(
        "VRAT WAR: vieja VMUL dispatch=%d complete=%d | "
        "joven VADD->v1 dispatch=%d complete=%d\n",
        old_dispatch,
        old_complete,
        young_dispatch,
        young_complete
    );

    // La VMUL vieja DEBE haber le??do el valor original:
    //
    // {1,2,3,4} * {10,20,30,40}
    // = {10,40,90,160}

    uint32_t expect_v3[4] = {
        10, 40, 90, 160
    };

    // El escritor joven debe quedar finalmente visible en v1:
    //
    // v2 + v2 = {20,40,60,80}

    uint32_t expect_v1[4] = {
        20, 40, 60, 80
    };

    for (int lane = 0; lane < 4; lane++) {

        uint32_t got3 =
            vo[3 * OOO_VEC_LANES + lane].to_uint();

        uint32_t got1 =
            vo[1 * OOO_VEC_LANES + lane].to_uint();

        if (got3 != expect_v3[lane]) {

            printf(
                "FAIL  WAR lector viejo v3[%d]=%u esperado=%u\n",
                lane,
                got3,
                expect_v3[lane]
            );

            fails++;
        }

        if (got1 != expect_v1[lane]) {

            printf(
                "FAIL  WAR escritor joven v1[%d]=%u esperado=%u\n",
                lane,
                got1,
                expect_v1[lane]
            );

            fails++;
        }
    }

    if (old_dispatch >= 0 &&
        young_dispatch >= 0 &&
        old_complete >= 0 &&
        young_complete >= 0) {

        printf(
            "PASS  WAR observo todos los eventos dispatch/completion\n"
        );

    } else {

        printf(
            "FAIL  WAR no observo todos los eventos\n"
        );

        fails++;
    }

    // La joven debe poder entrar mientras la vieja sigue en vuelo.
    if (young_dispatch >= 0 &&
        old_complete >= 0 &&
        young_dispatch < old_complete) {

        printf(
            "PASS  WAR escritor joven fue despachado "
            "antes de completar lector viejo\n"
        );

    } else {

        printf(
            "FAIL  WAR no demostro overlap de dispatch\n"
        );

        fails++;
    }

    // Esta es la evidencia fuerte del renombrado:
    // una escritura joven puede terminar antes que la lectura vieja
    // sin destruir el valor que la vieja necesitaba.
    if (young_complete >= 0 &&
        old_complete >= 0 &&
        young_complete < old_complete) {

        printf(
            "PASS  WAR escritor joven completo antes que lector viejo\n"
        );

    } else {

        printf(
            "FAIL  WAR escritor joven no completo antes que lector viejo\n"
        );

        fails++;
    }

    if (!halted) {
        printf("FAIL  VRAT WAR no termino\n");
        fails++;
    }

    if (fails == 0)
        printf("PASS  VRAT WAR EXPLICITO\n");

    return fails;
}


static int suite_vector_long_64pairs() {
    ap_uint<32> imem[OOO_IMEM_WORDS] = {0};
    ap_uint<32> dmem[OOO_DMEM_WORDS] = {0};

    // Fuentes constantes:
    // v1 = {1,2,3,4}
    // v2 = {10,20,30,40}
    dmem[0] = 1;
    dmem[1] = 2;
    dmem[2] = 3;
    dmem[3] = 4;

    dmem[4] = 10;
    dmem[5] = 20;
    dmem[6] = 30;
    dmem[7] = 40;

    const uint32_t F6_ADD = 0b000000;
    const uint32_t F6_MUL = 0b100101;

    const int PAIRS = 64;
    const int ARITH_OPS = PAIRS * 2;

    std::vector<uint16_t> prog;

    auto push32 = [&](uint32_t w) {
        prog.push_back(w & 0xFFFF);
        prog.push_back(w >> 16);
    };

    auto push16 = [&](uint16_t h) {
        prog.push_back(h);
    };

    // ------------------------------------------------------------
    // Setup RVV: e32,m1, vl=4
    // ------------------------------------------------------------
    push32(enc_vsetvli(1, 0, VTYPE_E32_M1));

    // v1 = {1,2,3,4}
    push32(enc_vec_mem(rv32i::Opcode::LOAD_FP, 1, 0));

    // x2 = 16
    push32(i_type(
        rv32i::Opcode::OP_IMM,
        2,
        F3A::ADD_SUB,
        0,
        16
    ));

    // v2 = {10,20,30,40}
    push32(enc_vec_mem(rv32i::Opcode::LOAD_FP, 2, 2));

    // ------------------------------------------------------------
    // 64 parejas.
    //
    // No existe RAW entre VMUL y VADD:
    //
    // VMUL -> v6 = v1 * v1 = {1,4,9,16}
    // VADD -> v7 = v2 + v2 = {20,40,60,80}
    //
    // Se reutilizan v6/v7 en cada iteracion:
    // eso crea dependencias WAW falsas entre iteraciones y estresa
    // el renombrado vectorial.
    // ------------------------------------------------------------
    for (int i = 0; i < PAIRS; i++) {
        push32(enc_op_v(
            F6_MUL,
            1,
            1,
            rvv_f3_opmvv,
            6
        ));

        push32(enc_op_v(
            F6_ADD,
            2,
            2,
            rvv_f3_opivv,
            7
        ));
    }

    // Sentinel de fin de programa.
    push16(0x0000);

    // Copiar programa de halfwords a IMEM.
    for (size_t i = 0; i < prog.size(); i++) {
        uint32_t w = imem[i / 2].to_uint();

        if ((i & 1) == 0)
            w = (w & 0xFFFF0000u) | prog[i];
        else
            w = (w & 0x0000FFFFu) |
                (uint32_t(prog[i]) << 16);

        imem[i / 2] = w;
    }

    // ------------------------------------------------------------
    // Se?ales del core.
    // ------------------------------------------------------------
    ap_uint<1> dv;
    ap_uint<3> dt;
    ap_uint<32> dp;

    ap_uint<1> a0d,a1d,mdd,fpd,lsd,brd,vcd;
    ap_uint<3> a0t,a1t,mdt,fpt,lst,brt,vct;

    ap_uint<1> cv,cfp;
    ap_uint<5> crd;
    ap_uint<32> cval;

    static ap_uint<32> vo[OOO_VEC_REGFILE_LEN];
    ap_uint<1> halted;

    // Reset.
    riscv_soc_tick(
        1, imem, dmem,
        dv,dt,dp,
        a0d,a0t,a1d,a1t,
        mdd,mdt,fpd,fpt,
        lsd,lst,brd,brt,vcd,vct,
        cv,cfp,crd,cval,
        vo,halted
    );

    int cycles = 0;
    int commits = 0;
    int dispatches = 0;
    int vec_completions = 0;

    const int MAX_CYCLES = 5000;

    while (!halted && cycles < MAX_CYCLES) {
        cycles++;

        riscv_soc_tick(
            0, imem, dmem,
            dv,dt,dp,
            a0d,a0t,a1d,a1t,
            mdd,mdt,fpd,fpt,
            lsd,lst,brd,brt,vcd,vct,
            cv,cfp,crd,cval,
            vo,halted
        );

        if (dv)  dispatches++;
        if (cv)  commits++;
        if (vcd) vec_completions++;
    }

    int fails = 0;

    if (!halted) {
        printf("FAIL  VECTOR LONG no termino\n");
        fails++;
    }

    // ------------------------------------------------------------
    // Validacion arquitectonica final.
    // ------------------------------------------------------------
    const uint32_t exp_mul[4] = {
        1, 4, 9, 16
    };

    const uint32_t exp_add[4] = {
        20, 40, 60, 80
    };

    for (int lane = 0; lane < 4; lane++) {
        uint32_t got_mul =
            vo[6 * OOO_VEC_LANES + lane].to_uint();

        uint32_t got_add =
            vo[7 * OOO_VEC_LANES + lane].to_uint();

        if (got_mul != exp_mul[lane]) {
            printf(
                "FAIL  v6[%d]=%u esperado=%u\n",
                lane, got_mul, exp_mul[lane]
            );
            fails++;
        }

        if (got_add != exp_add[lane]) {
            printf(
                "FAIL  v7[%d]=%u esperado=%u\n",
                lane, got_add, exp_add[lane]
            );
            fails++;
        }
    }

    double ipc =
        cycles > 0
        ? (double)commits / (double)cycles
        : 0.0;

    double arith_per_tick =
        cycles > 0
        ? (double)ARITH_OPS / (double)cycles
        : 0.0;

    double vec_completion_rate =
        cycles > 0
        ? (double)vec_completions / (double)cycles
        : 0.0;

    printf("\n");
    printf("============================================================\n");
    printf(" VECTOR LONG - 64 VMUL+VADD PAIRS\n");
    printf("============================================================\n");

    printf(" pairs              : %d\n", PAIRS);
    printf(" vector arith ops   : %d\n", ARITH_OPS);
    printf(" ticks              : %d\n", cycles);
    printf(" dispatches         : %d\n", dispatches);
    printf(" commits            : %d\n", commits);
    printf(" vec completions    : %d\n", vec_completions);
    printf(" IPC                : %.6f\n", ipc);
    printf(" arith ops/tick     : %.6f\n", arith_per_tick);
    printf(" vec complete/tick  : %.6f\n", vec_completion_rate);
    printf(" correctness        : %s\n",
           fails == 0 ? "PASS" : "FAIL");

    printf("============================================================\n");

    return fails;
}


static int suite_mixed_mul_fpu_vrat_32() {
    ap_uint<32> imem[OOO_IMEM_WORDS] = {0};
    ap_uint<32> dmem[OOO_DMEM_WORDS] = {0};

    // ============================================================
    // DATOS VECTORIALES
    // ============================================================

    // v1 = {1,2,3,4}
    dmem[0] = 1;
    dmem[1] = 2;
    dmem[2] = 3;
    dmem[3] = 4;

    // v2 = {10,20,30,40}
    dmem[4] = 10;
    dmem[5] = 20;
    dmem[6] = 30;
    dmem[7] = 40;

    // ============================================================
    // DATOS FLOAT
    //
    // byte 64:
    // f1 = 3.0
    // f2 = 4.0
    // f3 = 5.0
    // f4 = 6.0
    // ============================================================

    dmem[16] = 0x40400000u; // 3.0f
    dmem[17] = 0x40800000u; // 4.0f
    dmem[18] = 0x40A00000u; // 5.0f
    dmem[19] = 0x40C00000u; // 6.0f

    const uint32_t F6_ADD = 0b000000;
    const uint32_t F6_MUL = 0b100101;

    const int BLOCKS = 32;

    const int MUL_OPS = BLOCKS * 4;
    const int FPU_OPS = BLOCKS * 4;
    const int VEC_ARITH_OPS = BLOCKS * 4;

    const int SPECIALIZED_OPS =
        MUL_OPS + FPU_OPS + VEC_ARITH_OPS;

    std::vector<uint16_t> prog;

    auto push32 = [&](uint32_t w) {
        prog.push_back(w & 0xFFFF);
        prog.push_back(w >> 16);
    };

    auto push16 = [&](uint16_t h) {
        prog.push_back(h);
    };

    // ============================================================
    // SETUP ENTERO
    // ============================================================

    push32(i_type(
        rv32i::Opcode::OP_IMM,
        1,
        F3A::ADD_SUB,
        0,
        2
    ));

    push32(i_type(
        rv32i::Opcode::OP_IMM,
        2,
        F3A::ADD_SUB,
        0,
        3
    ));

    push32(i_type(
        rv32i::Opcode::OP_IMM,
        3,
        F3A::ADD_SUB,
        0,
        4
    ));

    push32(i_type(
        rv32i::Opcode::OP_IMM,
        4,
        F3A::ADD_SUB,
        0,
        5
    ));

    // ============================================================
    // SETUP FPU
    // ============================================================

    push32(i_type(
        rv32i::Opcode::LOAD_FP,
        1,
        rv32i::Funct3_FP_MEM::W,
        0,
        64
    ));

    push32(i_type(
        rv32i::Opcode::LOAD_FP,
        2,
        rv32i::Funct3_FP_MEM::W,
        0,
        68
    ));

    push32(i_type(
        rv32i::Opcode::LOAD_FP,
        3,
        rv32i::Funct3_FP_MEM::W,
        0,
        72
    ));

    push32(i_type(
        rv32i::Opcode::LOAD_FP,
        4,
        rv32i::Funct3_FP_MEM::W,
        0,
        76
    ));

    // ============================================================
    // SETUP VECTORIAL
    // ============================================================

    push32(enc_vsetvli(
        6,
        0,
        VTYPE_E32_M1
    ));

    push32(enc_vec_mem(
        rv32i::Opcode::LOAD_FP,
        1,
        0
    ));

    push32(i_type(
        rv32i::Opcode::OP_IMM,
        5,
        F3A::ADD_SUB,
        0,
        16
    ));

    push32(enc_vec_mem(
        rv32i::Opcode::LOAD_FP,
        2,
        5
    ));

    // ============================================================
    // KERNEL MIXTO
    //
    // Cada bloque:
    //
    // 4 MUL entero
    // 4 FPU ADD/MUL
    // 4 RVV VMUL/VADD
    //
    // Todas las fuentes son constantes e independientes.
    // Los destinos se reutilizan para mantener presion de renombrado.
    // ============================================================

    for (int i = 0; i < BLOCKS; i++) {

        // --------------------------------------------------------
        // MUL ESCALAR
        // --------------------------------------------------------

        push32(r_type(
            rv32i::Opcode::OP,
            10,
            F3M::MUL,
            1,
            2,
            rv32i::Funct7::MULDIV
        ));

        push32(r_type(
            rv32i::Opcode::OP,
            11,
            F3M::MUL,
            3,
            4,
            rv32i::Funct7::MULDIV
        ));

        push32(r_type(
            rv32i::Opcode::OP,
            12,
            F3M::MUL,
            1,
            4,
            rv32i::Funct7::MULDIV
        ));

        push32(r_type(
            rv32i::Opcode::OP,
            13,
            F3M::MUL,
            2,
            3,
            rv32i::Funct7::MULDIV
        ));

        // --------------------------------------------------------
        // FPU ESCALAR
        // --------------------------------------------------------

        // f10 = 3 * 4 = 12
        push32(r_type(
            rv32i::Opcode::OP_FP,
            10,
            0,
            1,
            2,
            F7F::FMUL_S
        ));

        // f11 = 5 + 6 = 11
        push32(r_type(
            rv32i::Opcode::OP_FP,
            11,
            0,
            3,
            4,
            F7F::FADD_S
        ));

        // f12 = 5 * 4 = 20
        push32(r_type(
            rv32i::Opcode::OP_FP,
            12,
            0,
            3,
            2,
            F7F::FMUL_S
        ));

        // f13 = 3 + 6 = 9
        push32(r_type(
            rv32i::Opcode::OP_FP,
            13,
            0,
            1,
            4,
            F7F::FADD_S
        ));

        // --------------------------------------------------------
        // RVV
        // --------------------------------------------------------

        // v6 = v1 * v1
        push32(enc_op_v(
            F6_MUL,
            1,
            1,
            rvv_f3_opmvv,
            6
        ));

        // v7 = v2 + v2
        push32(enc_op_v(
            F6_ADD,
            2,
            2,
            rvv_f3_opivv,
            7
        ));

        // v8 = v1 * v2
        push32(enc_op_v(
            F6_MUL,
            1,
            2,
            rvv_f3_opmvv,
            8
        ));

        // v9 = v1 + v2
        push32(enc_op_v(
            F6_ADD,
            1,
            2,
            rvv_f3_opivv,
            9
        ));
    }

    // Sentinel.
    push16(0x0000);

    // ============================================================
    // CARGAR PROGRAMA EN IMEM
    // ============================================================

    for (size_t i = 0; i < prog.size(); i++) {
        uint32_t w = imem[i / 2].to_uint();

        if ((i & 1) == 0)
            w = (w & 0xFFFF0000u) | prog[i];
        else
            w = (w & 0x0000FFFFu) |
                (uint32_t(prog[i]) << 16);

        imem[i / 2] = w;
    }

    // ============================================================
    // SE?ALES
    // ============================================================

    ap_uint<1> dv;
    ap_uint<3> dt;
    ap_uint<32> dp;

    ap_uint<1> a0d,a1d,mdd,fpd,lsd,brd,vcd;
    ap_uint<3> a0t,a1t,mdt,fpt,lst,brt,vct;

    ap_uint<1> cv,cfp;
    ap_uint<5> crd;
    ap_uint<32> cval;

    static ap_uint<32> vo[OOO_VEC_REGFILE_LEN];
    ap_uint<1> halted;

    // Valores finales observados por commit.
    uint32_t int_last[4] = {0,0,0,0};
    uint32_t fp_last[4]  = {0,0,0,0};

    bool int_seen[4] = {false,false,false,false};
    bool fp_seen[4]  = {false,false,false,false};

    // Reset.
    riscv_soc_tick(
        1, imem, dmem,
        dv,dt,dp,
        a0d,a0t,a1d,a1t,
        mdd,mdt,fpd,fpt,
        lsd,lst,brd,brt,vcd,vct,
        cv,cfp,crd,cval,
        vo,halted
    );

    int ticks = 0;
    int dispatches = 0;
    int commits = 0;

    int mul_completions = 0;
    int fpu_completions = 0;
    int vec_completions = 0;

    const int MAX_TICKS = 20000;

    while (!halted && ticks < MAX_TICKS) {
        ticks++;

        riscv_soc_tick(
            0, imem, dmem,
            dv,dt,dp,
            a0d,a0t,a1d,a1t,
            mdd,mdt,fpd,fpt,
            lsd,lst,brd,brt,vcd,vct,
            cv,cfp,crd,cval,
            vo,halted
        );

        if (dv) dispatches++;
        if (cv) commits++;

        if (mdd) mul_completions++;
        if (fpd) fpu_completions++;
        if (vcd) vec_completions++;

        if (cv) {
            int rd = crd.to_uint();

            if (rd >= 10 && rd <= 13) {
                int idx = rd - 10;

                if (cfp) {
                    fp_last[idx] = cval.to_uint();
                    fp_seen[idx] = true;
                } else {
                    int_last[idx] = cval.to_uint();
                    int_seen[idx] = true;
                }
            }
        }
    }

    int fails = 0;

    if (!halted) {
        printf("FAIL  MIXED benchmark no termino\n");
        fails++;
    }

    // ============================================================
    // VALIDACION ENTERA
    // ============================================================

    const uint32_t exp_int[4] = {
        6,
        20,
        10,
        12
    };

    for (int i = 0; i < 4; i++) {
        if (!int_seen[i] || int_last[i] != exp_int[i]) {
            printf(
                "FAIL  x%d=%u esperado=%u\n",
                i + 10,
                int_last[i],
                exp_int[i]
            );

            fails++;
        }
    }

    // ============================================================
    // VALIDACION FPU
    //
    // f10 = 12.0 = 0x41400000
    // f11 = 11.0 = 0x41300000
    // f12 = 20.0 = 0x41A00000
    // f13 =  9.0 = 0x41100000
    // ============================================================

    const uint32_t exp_fp[4] = {
        0x41400000u,
        0x41300000u,
        0x41A00000u,
        0x41100000u
    };

    for (int i = 0; i < 4; i++) {
        if (!fp_seen[i] || fp_last[i] != exp_fp[i]) {
            printf(
                "FAIL  f%d=0x%08X esperado=0x%08X\n",
                i + 10,
                fp_last[i],
                exp_fp[i]
            );

            fails++;
        }
    }

    // ============================================================
    // VALIDACION VECTORIAL
    // ============================================================

    const uint32_t exp_v6[4] = {1,4,9,16};
    const uint32_t exp_v7[4] = {20,40,60,80};
    const uint32_t exp_v8[4] = {10,40,90,160};
    const uint32_t exp_v9[4] = {11,22,33,44};

    for (int lane = 0; lane < 4; lane++) {

        uint32_t v6 =
            vo[6 * OOO_VEC_LANES + lane].to_uint();

        uint32_t v7 =
            vo[7 * OOO_VEC_LANES + lane].to_uint();

        uint32_t v8 =
            vo[8 * OOO_VEC_LANES + lane].to_uint();

        uint32_t v9 =
            vo[9 * OOO_VEC_LANES + lane].to_uint();

        if (v6 != exp_v6[lane]) fails++;
        if (v7 != exp_v7[lane]) fails++;
        if (v8 != exp_v8[lane]) fails++;
        if (v9 != exp_v9[lane]) fails++;
    }

    // Los dos VLE del setup tambien producen completion vectorial.
    int vec_arith_completions =
        vec_completions >= 2
        ? vec_completions - 2
        : 0;

    double ipc =
        ticks > 0
        ? (double)commits / (double)ticks
        : 0.0;

    double specialized_per_tick =
        ticks > 0
        ? (double)SPECIALIZED_OPS / (double)ticks
        : 0.0;

    printf("\n");
    printf("============================================================\n");
    printf(" MIXED MUL + FPU + VRAT BENCHMARK\n");
    printf("============================================================\n");

    printf(" blocks                 : %d\n", BLOCKS);
    printf(" MUL ops                : %d\n", MUL_OPS);
    printf(" FPU ops                : %d\n", FPU_OPS);
    printf(" vector arith ops       : %d\n", VEC_ARITH_OPS);
    printf(" specialized ops        : %d\n", SPECIALIZED_OPS);

    printf(" ticks                  : %d\n", ticks);
    printf(" dispatches             : %d\n", dispatches);
    printf(" commits                : %d\n", commits);

    printf(" MUL completions        : %d\n", mul_completions);
    printf(" FPU completions        : %d\n", fpu_completions);
    printf(" vector completions     : %d\n", vec_completions);
    printf(" vector arith completed : %d\n", vec_arith_completions);

    printf(" IPC                    : %.6f\n", ipc);
    printf(" specialized ops/tick   : %.6f\n", specialized_per_tick);

    printf(" correctness            : %s\n",
           fails == 0 ? "PASS" : "FAIL");

    printf("============================================================\n");

    return fails;
}

int main() {
    printf("\n");
    printf("============================================================\n");
    printf(" IMPROVED - MIXED MUL + FPU + VRAT\n");
    printf("============================================================\n");

    int rc = suite_mixed_mul_fpu_vrat_32();

    printf("\nMIXED_RETURN = %d\n", rc);

    return rc == 0 ? 0 : 1;
}
