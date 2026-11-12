#include <cstdio>
#include <cstdint>
#include <vector>
#include <algorithm>

#include "soc_top.h"
#include "rv32i_defs.h"
#include "rv32c_defs.h"
#include "soc_config.h"

using namespace rv32c;
namespace A = rv32i::Funct3_ALU;
namespace M = rv32i::Funct3_MULDIV;

enum {
    x0,x1,x2,x3,x4,x5,x6,x7,x8,x9,x10,x11,x12,x13,x14,x15,
    x16,x17,x18,x19,x20,x21,x22,x23,x24,x25,x26,x27,x28,x29,x30,x31
};

struct P {
    std::vector<uint16_t> prog;

    void w(uint32_t instr) {
        prog.push_back(instr & 0xFFFFu);
        prog.push_back(instr >> 16);
    }

    void h(uint16_t instr) {
        prog.push_back(instr);
    }

    void end() {
        prog.push_back(0x0000);
    }

    void li(int rd, int imm) {
        w(i_type(rv32i::Opcode::OP_IMM, rd, A::ADD_SUB, x0, imm));
    }

    void addi(int rd, int rs1, int imm) {
        w(i_type(rv32i::Opcode::OP_IMM, rd, A::ADD_SUB, rs1, imm));
    }

    void add(int rd, int rs1, int rs2) {
        w(r_type(rv32i::Opcode::OP, rd, A::ADD_SUB, rs1, rs2, 0));
    }

    void sub(int rd, int rs1, int rs2) {
        w(r_type(rv32i::Opcode::OP, rd, A::ADD_SUB, rs1, rs2,
                 rv32i::Funct7::ALT));
    }

    void mul(int rd, int rs1, int rs2) {
        w(r_type(rv32i::Opcode::OP, rd, M::MUL, rs1, rs2,
                 rv32i::Funct7::MULDIV));
    }

    void sw(int rs2, int base, int off) {
        w(s_type(rv32i::Opcode::STORE, rv32i::Funct3_STORE::SW,
                 base, rs2, off));
    }

    void lw(int rd, int base, int off) {
        w(i_type(rv32i::Opcode::LOAD, rd, rv32i::Funct3_LOAD::LW,
                 base, off));
    }

    void beq(int rs1, int rs2, int off) {
        w(b_type(rv32i::Funct3_BRANCH::BEQ, rs1, rs2, off));
    }

    void illegal_fp() {
        // fadd.s f1,f2,f3. En Lite debe producir Illegal Instruction.
        w(r_type(rv32i::Opcode::OP_FP, 1, 0, 2, 3,
                 rv32i::Funct7_FP::FADD_S));
    }

    void illegal_vec() {
        // Opcode OP-V (0x57). En Lite debe producir Illegal Instruction.
        w(0x00000057u);
    }
};

struct RunResult {
    uint32_t x[32] = {0};
    ap_uint<32> dmem[OOO_DMEM_WORDS];
    int cycles = 0;
    bool halted = false;

    uint32_t branches = 0;
    uint32_t mispredicts = 0;

    int complete_pc8  = -1;
    int complete_pc12 = -1;
    int complete_pc16 = -1;
    int complete_pc20 = -1;

uint32_t max_alu_rs_busy = 0;

    std::vector<int> commit_rd;

    // Evidencia especifica del ROB.
    std::vector<int> dispatch_tags;
    int max_inflight = 0;
};

static RunResult run(P& p, int max_cycles = 4000) {
    static ap_uint<32> imem[OOO_IMEM_WORDS];
    RunResult r;

    for (int i = 0; i < OOO_IMEM_WORDS; i++) imem[i] = 0;
    for (int i = 0; i < OOO_DMEM_WORDS; i++) r.dmem[i] = 0;

    for (size_t i = 0; i < p.prog.size(); i++) {
        uint32_t word = imem[i / 2].to_uint();
        if ((i & 1) == 0)
            word = (word & 0xFFFF0000u) | p.prog[i];
        else
            word = (word & 0x0000FFFFu) | (uint32_t(p.prog[i]) << 16);
        imem[i / 2] = word;
    }

    ap_uint<1> disp_valid;
    rob_tag_t disp_tag;
    ap_uint<32> disp_pc;

    ap_uint<1> alu_done, md_done, lsu_done, br_done;
    rob_tag_t alu_tag, md_tag, lsu_tag, br_tag;

    ap_uint<1> commit_valid;
    ap_uint<5> commit_rd;
    ap_uint<32> commit_value;
    ap_uint<1> halted;

    uint32_t tag_pc[ROB_SZ];
    for (int i = 0; i < ROB_SZ; i++) tag_pc[i] = 0xFFFFFFFFu;
    int inflight = 0;

    riscv_soc_tick(
        1, imem, r.dmem,
        disp_valid, disp_tag, disp_pc,
        alu_done, alu_tag,
        md_done, md_tag,
        lsu_done, lsu_tag,
        br_done, br_tag,
        commit_valid, commit_rd, commit_value,
        halted
    );

    while (!halted && r.cycles < max_cycles) {
        r.cycles++;

        riscv_soc_tick(
            0, imem, r.dmem,
            disp_valid, disp_tag, disp_pc,
            alu_done, alu_tag,
            md_done, md_tag,
            lsu_done, lsu_tag,
            br_done, br_tag,
            commit_valid, commit_rd, commit_value,
            halted
        );

        // El tick hace COMMIT antes de DISPATCH.
        // Este contador externo reproduce ese orden para observar
        // la ocupacion maxima efectiva del ROB.
        if (commit_valid && inflight > 0)
            inflight--;

        if (disp_valid) {
            inflight++;

            r.dispatch_tags.push_back(disp_tag.to_uint());

            tag_pc[disp_tag.to_uint()] = disp_pc.to_uint();
}

r.max_inflight = std::max(r.max_inflight, inflight);

        auto record_completion = [&](ap_uint<1> done, rob_tag_t tag) {
            if (!done) return;
            uint32_t pc = tag_pc[tag.to_uint()];
            if (pc == 8)  r.complete_pc8  = r.cycles;
            if (pc == 12) r.complete_pc12 = r.cycles;
            if (pc == 16) r.complete_pc16 = r.cycles;
            if (pc == 20) r.complete_pc20 = r.cycles;
        };

        record_completion(alu_done, alu_tag);
        record_completion(md_done,  md_tag);
        record_completion(lsu_done,  lsu_tag);
        record_completion(br_done,   br_tag);

        if (commit_valid) {
            int rd = commit_rd.to_uint();
            r.commit_rd.push_back(rd);
            if (rd != 0) r.x[rd] = commit_value.to_uint();
        }
    }

    r.halted = halted;

    r.branches = soc_stat_branches();
    r.mispredicts = soc_stat_mispredicts();
    r.max_alu_rs_busy = soc_stat_max_alu_rs_busy();

    return r;
}

static int fails = 0;

static void ck(const char* name, uint32_t got, uint32_t expected) {
    if (got == expected) {
        std::printf("  OK    %-42s = %u\n", name, got);
    } else {
        std::printf("  FAIL  %-42s = %u (esperado %u)\n",
                    name, got, expected);
        fails++;
    }
}

static void ck_true(const char* name, bool cond) {
    if (cond) {
        std::printf("  OK    %s\n", name);
    } else {
        std::printf("  FAIL  %s\n", name);
        fails++;
    }
}

static int first_commit_pos(const RunResult& r, int rd) {
    for (size_t i = 0; i < r.commit_rd.size(); i++)
        if (r.commit_rd[i] == rd) return static_cast<int>(i);
    return -1;
}

int main() {
    std::printf("============================================================\n");
    std::printf(" RISC-V OoO Lite - verificacion funcional inicial\n");
    std::printf(" FPU y RVV eliminados; se conserva OoO escalar\n");
    std::printf("============================================================\n");

    // ------------------------------------------------------------
    // 1. Enteros basicos
    // ------------------------------------------------------------
    std::printf("\n[1] Enteros basicos\n");
    {
        P p;
        p.li(x1, 5);
        p.li(x2, 10);
        p.add(x3, x1, x2);
        p.sub(x4, x2, x1);
        p.end();

        RunResult r = run(p);
        ck("x3 = 5 + 10", r.x[x3], 15);
        ck("x4 = 10 - 5", r.x[x4], 5);
        ck_true("el programa termino", r.halted);
    }

    // ------------------------------------------------------------
    // 2. Memoria
    // ------------------------------------------------------------
    std::printf("\n[2] Load / Store entero\n");
    {
        P p;
        p.li(x1, 42);
        p.sw(x1, x0, 64);
        p.lw(x2, x0, 64);
        p.end();

        RunResult r = run(p);
        ck("lw recupera el valor guardado", r.x[x2], 42);
        ck("dmem[16] contiene 42", r.dmem[16].to_uint(), 42);
    }

    // ------------------------------------------------------------
    // 3. Evidencia OoO + commit en orden
    // ------------------------------------------------------------
    std::printf("\n[3] Evidencia de ejecucion fuera de orden\n");
    {
        P p;
        p.li(x1, 5);              // pc 0
        p.li(x2, 10);             // pc 4
        p.mul(x3, x1, x2);        // pc 8  - vieja y lenta
        p.li(x4, 7);              // pc 12 - joven e independiente
        p.add(x5, x3, x4);        // pc 16 - RAW con MUL
        p.end();

        RunResult r = run(p);

        ck("x3 = 5 * 10", r.x[x3], 50);
        ck("x4 = 7", r.x[x4], 7);
        ck("x5 = x3 + x4", r.x[x5], 57);

        std::printf("      completion MUL pc8  = ciclo %d\n", r.complete_pc8);
        std::printf("      completion ADDI pc12 = ciclo %d\n", r.complete_pc12);

        ck_true(
            "OOO: ADDI joven (pc12) completa antes que MUL vieja (pc8)",
            r.complete_pc12 > 0 &&
            r.complete_pc8 > 0 &&
            r.complete_pc12 < r.complete_pc8
        );

        int c3 = first_commit_pos(r, x3);
        int c4 = first_commit_pos(r, x4);

        ck_true(
            "commit en orden: x3 (MUL) se retira antes que x4 (ADDI)",
            c3 >= 0 && c4 >= 0 && c3 < c4
        );
    }

    // ------------------------------------------------------------
    // 4. WAW + RAW
    // ------------------------------------------------------------
    std::printf("\n[4] Dependencias WAW / RAW\n");
    {
        P p;
        p.li(x1, 5);
        p.mul(x2, x1, x1);        // x2 = 25
        p.li(x1, 7);              // escritor joven de x1
        p.add(x3, x1, x2);        // debe ver x1=7 y x2=25
        p.end();

        RunResult r = run(p);

        ck("WAW: x1 conserva escritor joven", r.x[x1], 7);
        ck("MUL: x2 = 25", r.x[x2], 25);
        ck("RAW: x3 = 7 + 25", r.x[x3], 32);
    }

        // ------------------------------------------------------------
    // 5. ROB Lite de 4 entradas: llenado + wrap-around
    // ------------------------------------------------------------
    std::printf("\n[5] ROB=4: llenado y reutilizacion circular\n");
    {
        P p;

        // El MUL lento bloquea temporalmente el retiro y permite
        // que el ROB alcance sus 4 entradas.
        //
        // Luego las instrucciones independientes fuerzan varias
        // reutilizaciones de los tags:
        //
        //     0,1,2,3,0,1,2,3,...

        p.li(x1, 5);               // primera instruccion
        p.li(x2, 10);              // segunda instruccion
        p.mul(x3, x1, x2);         // operacion lenta: x3 = 50

        for (int i = 4; i <= 12; i++)
            p.li(i, i * 3);

        p.end();

        RunResult r = run(p);

        // Resultado arquitectonico.
        ck("MUL durante prueba ROB", r.x[x3], 50);

        for (int i = 4; i <= 12; i++) {
            char name[64];

            std::snprintf(
                name,
                sizeof(name),
                "x%d correcto tras wrap-around",
                i
            );

            ck(name, r.x[i], i * 3);
        }

        // Debemos haber llenado realmente las cuatro entradas.
        ck(
            "ROB alcanza ocupacion maxima de 4",
            static_cast<uint32_t>(r.max_inflight),
            ROB_SZ
        );

        // El programa contiene exactamente 12 instrucciones reales.
        ck(
            "12 instrucciones despachadas",
            static_cast<uint32_t>(r.dispatch_tags.size()),
            12
        );

        // Con un ROB de cuatro entradas los tags validos son:
        //
        // 0,1,2,3,0,1,2,3,0,1,2,3
        //
        // Nunca debe aparecer 4,5,6 o 7.
        bool tags_ok = (r.dispatch_tags.size() == 12);

        for (size_t i = 0; i < r.dispatch_tags.size(); i++) {
            int expected_tag =
                static_cast<int>(i % ROB_SZ);

            if (r.dispatch_tags[i] != expected_tag) {
                tags_ok = false;
                break;
            }
        }

        ck_true(
            "tags hacen 0,1,2,3,0... sin usar 4..7",
            tags_ok
        );
    }

    // ------------------------------------------------------------
    // 6. Una ALU fisica alimentada por 3 Reservation Stations
    // ------------------------------------------------------------
    std::printf(
        "\n[6] 1 ALU fisica + 3 ALU Reservation Stations\n"
    );

    {
        P p;

        // Las tres ADDI dependen del mismo MUL lento.
        //
        // Mientras el MUL no termina, las tres quedan esperando
        // simultaneamente en las 3 ALU Reservation Stations.
        //
        // Cuando el MUL publica x3 mediante el CDB, las tres pasan
        // a READY al mismo tiempo.
        //
        // Como solamente existe UNA ALU fisica, deben completarse
        // una por una y en orden oldest-ready.

        p.li(x1, 5);               // pc 0
        p.li(x2, 10);              // pc 4

        p.mul(x3, x1, x2);         // pc 8
                                    // x3 = 50

        p.addi(x4, x3, 1);         // pc 12
                                    // x4 = 51

        p.addi(x5, x3, 2);         // pc 16
                                    // x5 = 52

        p.addi(x6, x3, 3);         // pc 20
                                    // x6 = 53

        p.end();

        RunResult r = run(p);

        ck("x3 = 50", r.x[x3], 50);
        ck("x4 = 51", r.x[x4], 51);
        ck("x5 = 52", r.x[x5], 52);
        ck("x6 = 53", r.x[x6], 53);

        // Demuestra que las tres RS fueron necesarias
        // simultaneamente mientras esperaban al MUL.
        ck(
            "las 3 ALU RS llegan a estar ocupadas",
            r.max_alu_rs_busy,
            N_ALU_RS
        );

        std::printf(
            "      completion pc12=%d, pc16=%d, pc20=%d\n",
            r.complete_pc12,
            r.complete_pc16,
            r.complete_pc20
        );

        // Una sola ALU con ALU_LAT=1 puede producir un resultado
        // por ciclo. Las tres instrucciones despertaron juntas,
        // asi que deben completarse secuencialmente:
        //
        // pc12 -> pc16 -> pc20
        //
        // y cada completion debe estar separada por 1 ciclo.
        ck_true(
            "una sola ALU completa pc12, pc16 y pc20 "
            "en ciclos consecutivos",
            r.complete_pc12 > 0 &&
            r.complete_pc16 == r.complete_pc12 + 1 &&
            r.complete_pc20 == r.complete_pc16 + 1
        );
    }


    // ------------------------------------------------------------
    // 7. Predictor estatico NOT-TAKEN
    // ------------------------------------------------------------
    std::printf("\n[7] Predictor estatico NOT-TAKEN\n");

    // Caso A: branch TOMADO.
    // Lite predice NOT-TAKEN, por lo tanto este branch DEBE
    // generar exactamente un misprediction.
    {
        P p;

        p.li(x1, 1);               // pc 0
        p.beq(x1, x1, 8);          // pc 4: SIEMPRE tomado
        p.li(x2, 99);              // pc 8: camino incorrecto
        p.li(x2, 7);               // pc 12: destino correcto
        p.end();

        RunResult r = run(p);

        ck("branch tomado redirige al target", r.x[x2], 7);
        ck("branches retirados", r.branches, 1);
        ck("PNT: tomado produce 1 misprediction", r.mispredicts, 1);
        ck_true("programa con branch tomado termino", r.halted);
    }

    // Caso B: branch NO TOMADO.
    // La prediccion NOT-TAKEN debe acertar y no debe haber flush.
    {
        P p;

        p.li(x1, 1);               // pc 0
        p.li(x2, 2);               // pc 4
        p.beq(x1, x2, 8);          // pc 8: NO tomado
        p.li(x3, 7);               // pc 12: fall-through correcto
        p.end();

        RunResult r = run(p);

        ck("branch no tomado conserva fall-through", r.x[x3], 7);
        ck("branches retirados", r.branches, 1);
        ck("PNT: no tomado produce 0 mispredictions", r.mispredicts, 0);
        ck_true("programa con branch no tomado termino", r.halted);
    }

    // ------------------------------------------------------------
    // 8. Extension C
    // ------------------------------------------------------------
    std::printf("\n[8] Instrucciones comprimidas\n");
    {
        P p;
        p.h(0x47A5);  // c.li x15,9
        p.h(0x078D);  // c.addi x15,3
        p.end();

        RunResult r = run(p);
        ck("C.LI + C.ADDI -> x15", r.x[x15], 12);
    }

    // ------------------------------------------------------------
    // 9. FPU eliminada: debe atrapar como instruccion ilegal
    // ------------------------------------------------------------
    std::printf("\n[9] FPU eliminada\n");
    {
        P p;
        p.li(x1, 11);
        p.illegal_fp();
        p.li(x2, 99);  // no debe retirarse
        p.end();

        RunResult r = run(p);

        ck("instruccion anterior al FP si committea", r.x[x1], 11);
        ck("instruccion posterior al FP no committea", r.x[x2], 0);
        ck_true("FP no soportado provoca halt/trap", r.halted);
    }

    // ------------------------------------------------------------
    // 10. RVV eliminada: debe atrapar como instruccion ilegal
    // ------------------------------------------------------------
    std::printf("\n[10] RVV eliminada\n");
    {
        P p;
        p.li(x1, 22);
        p.illegal_vec();
        p.li(x2, 88);  // no debe retirarse
        p.end();

        RunResult r = run(p);

        ck("instruccion anterior al OP-V si committea", r.x[x1], 22);
        ck("instruccion posterior al OP-V no committea", r.x[x2], 0);
        ck_true("OP-V no soportado provoca halt/trap", r.halted);
    }

    // ------------------------------------------------------------
    std::printf("\n============================================================\n");
    if (fails == 0)
        std::printf(" RESULTADO: TODOS LOS CHECKS DE LITE PASARON\n");
    else
        std::printf(" RESULTADO: %d CHECK(S) FALLARON\n", fails);
    std::printf("============================================================\n");

    return fails == 0 ? 0 : 1;
}
