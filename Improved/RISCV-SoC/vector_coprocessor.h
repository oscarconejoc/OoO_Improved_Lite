#ifndef VECTOR_COPROCESSOR_H
#define VECTOR_COPROCESSOR_H

// coprocesador vectorial: VIQ (fifo 4) -> unidades VALU/VMUL/VSLDU/VLSU.
// el escalar despacha la vectorial a la viq (con vl/vstart/sew y su tag) y
// sigue: hasta 4 en vuelo mientras avanza; el rob comun retira en orden.
// dentro del coproc la ejecucion es en orden, ruteada por unidad:
//   valu (lat 2) aritmetica/mascaras/reduce/widening; vmul (lat 4) mul/div;
//   vsldu (lat 2) permutaciones; vlsu loads/stores por el arbitro (en la cabeza del rob).
// no especula (vregs sin renombrar): retiene vectoriales si hay branch en vuelo.
#include "soc_top.h"
#include "soc_state.h"
#include "exec_vector.h"

// clasificacion por unidad (decide la LATENCIA y, en el RTL, que
// hardware se instancia)
static bool vec_is_vmul(const VecRs& u) {
    ap_uint<6> f6 = u.funct6;
    bool muldiv = (u.funct3 == rvv::FUNCT3_OPMVV || u.funct3 == rvv::FUNCT3_OPMVX) &&
                  (f6 == rvv::F6_VMUL  || f6 == rvv::F6_VMULH   ||
                   f6 == rvv::F6_VMULHU|| f6 == rvv::F6_VMULHSU ||
                   f6 == rvv::F6_VDIVU || f6 == rvv::F6_VDIV    ||
                   f6 == rvv::F6_VREMU || f6 == rvv::F6_VREM);
    bool wmul = (u.vcat == rvv::VCAT_WIDE) &&
                (f6 == rvv::F6_VWMULU || f6 == rvv::F6_VWMULSU ||
                 f6 == rvv::F6_VWMUL  || f6 == rvv::F6_VWMACCU ||
                 f6 == rvv::F6_VWMACC || f6 == rvv::F6_VWMACCSU||
                 f6 == rvv::F6_VWMACCUS);
    return muldiv || wmul;
}

// ============================================================================
// VRAT Stage 2A
//
// Ejecucion especulativa de operaciones vectoriales simples:
//   - VCAT_ALU
//   - SEW=32
//   - VL=4
//   - vm=1
//   - vstart=0
//
// El resultado NO modifica el banco arquitectonico vregs[].
// Se almacena temporalmente en vrob_data[tag][lane] y solo se hace
// visible arquitectonicamente en el commit.
// ============================================================================

static void vec_vrat_compute_simple(const VecRs& u) {
#pragma HLS INLINE

    bool scalar_form =
        (u.funct3 == rvv::FUNCT3_OPIVX ||
         u.funct3 == rvv::FUNCT3_OPIVI ||
         u.funct3 == rvv::FUNCT3_OPMVX);

    for (int lane = 0; lane < OOO_VEC_LANES; lane++) {
#pragma HLS UNROLL

        // Los operandos vectoriales ya fueron capturados en dispatch
        // o mediante wakeup cuando termino su productor.
        ap_uint<32> a = u.vs2_vval[lane];

        ap_uint<32> b;

        if (scalar_form) {
            b = u.s1.val;
        } else {
            b = u.vs1_vval[lane];
        }

        vrob_data[u.rob_tag][lane] =
            vec_alu(
                u.funct3,
                u.funct6,
                a,
                b,
                4
            );
    }
}
static void vec_vrat_broadcast(ap_uint<3> tag) {
#pragma HLS INLINE

    // Una instruccion puede haber salido ya de la VIQ y seguir
    // esperando al productor dentro de alguno de los slots.

    if (vec_rs.busy && vec_rs.use_vrat) {
        if (vec_rs.vs1_vdep && !vec_rs.vs1_vready &&
            vec_rs.vs1_vtag == tag) {
            for (int lane = 0; lane < OOO_VEC_LANES; lane++) {
#pragma HLS UNROLL
                vec_rs.vs1_vval[lane] = vrob_data[tag][lane];
            }
            vec_rs.vs1_vready = true;
        }

        if (vec_rs.vs2_vdep && !vec_rs.vs2_vready &&
            vec_rs.vs2_vtag == tag) {
            for (int lane = 0; lane < OOO_VEC_LANES; lane++) {
#pragma HLS UNROLL
                vec_rs.vs2_vval[lane] = vrob_data[tag][lane];
            }
            vec_rs.vs2_vready = true;
        }
    }

    if (vec_mul_rs.busy && vec_mul_rs.use_vrat) {
        if (vec_mul_rs.vs1_vdep && !vec_mul_rs.vs1_vready &&
            vec_mul_rs.vs1_vtag == tag) {
            for (int lane = 0; lane < OOO_VEC_LANES; lane++) {
#pragma HLS UNROLL
                vec_mul_rs.vs1_vval[lane] = vrob_data[tag][lane];
            }
            vec_mul_rs.vs1_vready = true;
        }

        if (vec_mul_rs.vs2_vdep && !vec_mul_rs.vs2_vready &&
            vec_mul_rs.vs2_vtag == tag) {
            for (int lane = 0; lane < OOO_VEC_LANES; lane++) {
#pragma HLS UNROLL
                vec_mul_rs.vs2_vval[lane] = vrob_data[tag][lane];
            }
            vec_mul_rs.vs2_vready = true;
        }
    }
    // Las instrucciones que esperan en la VIQ capturan el resultado
    // vectorial cuando su productor termina.
    for (int q = 0; q < VIQ_SZ; q++) {
#pragma HLS UNROLL
        if (viq[q].use_vrat) {

            if (viq[q].vs1_vdep &&
                !viq[q].vs1_vready &&
                viq[q].vs1_vtag == tag) {

                for (int lane = 0; lane < OOO_VEC_LANES; lane++) {
#pragma HLS UNROLL
                    viq[q].vs1_vval[lane] = vrob_data[tag][lane];
                }

                viq[q].vs1_vready = true;
            }

            if (viq[q].vs2_vdep &&
                !viq[q].vs2_vready &&
                viq[q].vs2_vtag == tag) {

                for (int lane = 0; lane < OOO_VEC_LANES; lane++) {
#pragma HLS UNROLL
                    viq[q].vs2_vval[lane] = vrob_data[tag][lane];
                }

                viq[q].vs2_vready = true;
            }
        }
    }
}
static void vec_capture_vrat_sources(VecRs& u) {
#pragma HLS INLINE

    if (!u.use_vrat)
        return;

    bool scalar_form =
        (u.funct3 == rvv::FUNCT3_OPIVX ||
         u.funct3 == rvv::FUNCT3_OPIVI ||
         u.funct3 == rvv::FUNCT3_OPMVX);

    // vs2 siempre es vectorial en el subconjunto VRAT.
    if (!u.vs2_vdep && !u.vs2_vready) {

        int base2 = u.vs2.to_uint() * OOO_VEC_LANES;

        for (int lane = 0; lane < OOO_VEC_LANES; lane++) {
#pragma HLS UNROLL
            u.vs2_vval[lane] = vregs[base2 + lane];
        }

        u.vs2_vready = true;
    }

    // En .vv/.mv, vs1 tambien es vectorial.
    // En .vx/.vi el segundo operando viene por s1.
    if (!scalar_form &&
        !u.vs1_vdep &&
        !u.vs1_vready) {

        int base1 = u.vs1.to_uint() * OOO_VEC_LANES;

        for (int lane = 0; lane < OOO_VEC_LANES; lane++) {
#pragma HLS UNROLL
            u.vs1_vval[lane] = vregs[base1 + lane];
        }

        u.vs1_vready = true;
    }
}
// Un paso del coprocesador (llamado una vez por tick del SoC).
// `vec_done/vec_tag` son las salidas de observabilidad del top.
static void vector_coprocessor_tick(ap_uint<32> dmem[OOO_DMEM_WORDS],
                                    ap_uint<1>& vec_done, ap_uint<3>& vec_tag) {
#pragma HLS INLINE
    // ---- despachar desde la VIQ hacia las unidades vectoriales ----
    if (viq_count != 0) {

        VecRs& head = viq[viq_head];

        // Primera separacion de unidades:
        // VMUL renombrada -> vec_mul_rs
        // resto          -> vec_rs
        bool head_to_mul =
            head.use_vrat &&
            head.is_arith &&
            vec_is_vmul(head);

        // Una aritmetica legacy lee vregs[] directamente.
        // Debe esperar hasta ser la instruccion mas vieja del ROB para
        // observar todos los resultados VRAT anteriores ya comprometidos.
        //
        // VLSU no necesita este barrier aqui: su ejecucion ya esta
        // restringida a rob_head mas abajo.
        bool legacy_arith_safe =
            head.use_vrat ||
            !head.is_arith ||
            (head.rob_tag == rob_head && rob[rob_head].valid);

        if (head_to_mul &&
            !vec_mul_rs.busy &&
            !vec_rs.busy) {

            vec_mul_rs = head;

            viq_head = viq_head + 1;
            viq_count = viq_count - 1;

            vec_mul_rs.busy = true;
            vec_mul_rs.executing = false;

            vec_capture_vrat_sources(vec_mul_rs);

        } else if (!head_to_mul &&
                   !vec_rs.busy &&
                   legacy_arith_safe) {

            vec_rs = head;

            viq_head = viq_head + 1;
            viq_count = viq_count - 1;

            vec_rs.busy = true;
            vec_rs.executing = false;

            vec_capture_vrat_sources(vec_rs);
        }
    }
    // ---- issue: la unidad arranca cuando sus operandos existen ----
    bool vec_scalar_form =
        (vec_rs.funct3 == rvv::FUNCT3_OPIVX ||
         vec_rs.funct3 == rvv::FUNCT3_OPIVI ||
         vec_rs.funct3 == rvv::FUNCT3_OPMVX);

    bool vrat_sources_ready =
        !vec_rs.use_vrat ||
        (vec_rs.vs2_vready &&
         (vec_scalar_form || vec_rs.vs1_vready));

    if (vec_rs.busy && vec_rs.is_arith &&
        !vec_rs.executing &&
        vec_rs.s1.ready &&
        vrat_sources_ready) {
        // sin operandos que esperar (lee vregs directo, sin RAT): arranca
        // en el primer ciclo de issue disponible, igual que las demas
        vec_rs.executing = true;
        // latencia por UNIDAD: VMUL es mas cara que VALU/VSLDU
        vec_rs.remaining = vec_is_vmul(vec_rs)          ? VMUL_LAT :
                           (vec_rs.vcat == rvv::VCAT_PERM) ? VSLDU_LAT :
                                                             VALU_LAT;
    }

    // ---- issue independiente de VMUL renombrada ----
    bool vec_mul_scalar_form =
        (vec_mul_rs.funct3 == rvv::FUNCT3_OPIVX ||
         vec_mul_rs.funct3 == rvv::FUNCT3_OPIVI ||
         vec_mul_rs.funct3 == rvv::FUNCT3_OPMVX);

    bool vec_mul_sources_ready =
        vec_mul_rs.vs2_vready &&
        (vec_mul_scalar_form || vec_mul_rs.vs1_vready);

    if (vec_mul_rs.busy &&
        vec_mul_rs.is_arith &&
        !vec_mul_rs.executing &&
        vec_mul_rs.s1.ready &&
        vec_mul_sources_ready) {

        vec_mul_rs.executing = true;
        vec_mul_rs.remaining = VMUL_LAT;
    }
    // ---- VALU / VMUL / VSLDU: comparten el datapath por elemento ----
    if (vec_rs.busy && vec_rs.is_arith && vec_rs.executing) {
        if (vec_rs.remaining > 0) vec_rs.remaining = vec_rs.remaining - 1;
        if (vec_rs.remaining == 0) {
            ap_uint<32> xres = 0;

            if (vec_rs.use_vrat) {
                // Resultado especulativo: todavia NO modifica vregs.
                vec_vrat_compute_simple(vec_rs);
            } else {
                // Ruta original para instrucciones fuera del subconjunto VRAT.
                xres = vec_arith_compute(vec_rs, vregs);
            }

            // vcpop.m / vfirst.m escriben un registro ENTERO.
            if (vec_rs.vcat == rvv::VCAT_XRES) {
                rob[vec_rs.rob_tag].value = xres;
                cdb_broadcast(vec_rs.rob_tag, xres);
            }

            rob[vec_rs.rob_tag].ready = true;

            // Wakeup de instrucciones vectoriales dependientes en la VIQ.
            if (vec_rs.use_vrat) {
                vec_vrat_broadcast(vec_rs.rob_tag);
            }
            vec_done = 1; vec_tag = vec_rs.rob_tag;
            vec_rs.busy = false;
            vec_rs.executing = false;
            csr_vstart = 0;   // Fase 6: toda vectorial que COMPLETA resetea vstart
        }
    }

    // ---- VMUL VRAT: unidad independiente ----
    if (vec_mul_rs.busy &&
        vec_mul_rs.is_arith &&
        vec_mul_rs.executing) {

        if (vec_mul_rs.remaining > 0)
            vec_mul_rs.remaining = vec_mul_rs.remaining - 1;

        if (vec_mul_rs.remaining == 0) {

            // El resultado permanece especulativo hasta commit.
            vec_vrat_compute_simple(vec_mul_rs);

            rob[vec_mul_rs.rob_tag].ready = true;

            // Despierta consumidores vectoriales por tag.
            vec_vrat_broadcast(vec_mul_rs.rob_tag);

            vec_done = 1;
            vec_tag  = vec_mul_rs.rob_tag;

            vec_mul_rs.busy = false;
            vec_mul_rs.executing = false;

            csr_vstart = 0;
        }
    }
    // ---- VLSU: memoria vectorial, a traves del ARBITRO del interconnect ----
    // Ejecuta SOLO en la cabeza del ROB (memoria en orden, igual que el
    // load escalar) y mueve UN ELEMENTO POR CICLO: cada acceso pide el
    // puerto de datos al arbitro (axi_grant) y compite con la LSU
    // escalar. `elem` y `fld` llevan el progreso entre ciclos -- la
    // instruccion permanece en la unidad hasta terminar su ultimo
    // elemento. Los cuatro modos de direccionamiento solo cambian COMO
    // se calcula la direccion:
    //
    //   unit-stride : base + (e*nf + f)*eew      -- elementos contiguos
    //   strided     : base + e*paso   + f*eew    -- paso en BYTES de x[rs2]
    //   indexado    : base + vs2[e]   + f*eew    -- offsets desde un vector
    //
    if (vec_rs.busy && (vec_rs.is_load || vec_rs.is_store) &&
        vec_rs.s1.ready && vec_rs.s2.ready &&
        vec_rs.rob_tag == rob_head && rob[rob_head].valid) {
        const ap_uint<32> base   = vec_rs.s1.val;
        const ap_uint<32> stride = vec_rs.s2.val;
        const uint32_t sb    = vec_rs.sew_b.to_uint();   // ancho del DATO
        const uint32_t ib    = vec_rs.idx_b.to_uint();   // ancho del INDICE
        const uint32_t nfld  = vec_rs.nf.to_uint() + 1;  // campos por segmento
        const uint32_t f     = vec_rs.fld.to_uint();     // campo en curso
        const uint32_t e     = vec_rs.elem.to_uint();    // elemento en curso
        const uint32_t emask = (sb == 4) ? 0xFFFFFFFFu : ((1u << (sb * 8)) - 1);
        const bool indexed   = (vec_rs.mop == rvv::MOP_IDX_UNORD ||
                                vec_rs.mop == rvv::MOP_IDX_ORD);
        bool done = false;

        if (vec_rs.lsmode == rvv::LSM_WHOLE) {
            // ---- vl<nf>r.v / vs<nf>r.v: registros COMPLETOS ----
            // Ignora vl, vtype y la mascara (por eso sirve para cambios de
            // contexto). UNA PALABRA por ciclo a traves del arbitro; `elem`
            // recorre las palabras del registro y `fld` los registros.
            if (axi_grant()) {
                uint32_t vi = (vec_rs.vd_or_vs3.to_uint() + f) & 31;
                ap_uint<32> a = base + (f * OOO_VEC_LANES + e) * 4;
                uint32_t wi = (a >> 2) & (OOO_DMEM_WORDS - 1);
                if (vec_rs.is_load) vregs[vi * OOO_VEC_LANES + e] = dmem[wi];
                else                dmem[wi] = vregs[vi * OOO_VEC_LANES + e];
                if (e + 1 < (uint32_t)OOO_VEC_LANES) {
                    vec_rs.elem = e + 1;
                } else if (f + 1 < nfld) {
                    vec_rs.fld = f + 1; vec_rs.elem = 0;
                } else done = true;
            }
        } else if (vec_rs.lsmode == rvv::LSM_MASK) {
            // ---- vlm.v / vsm.v: la MASCARA como bytes, ceil(vl/8) ----
            uint32_t evl = (vec_rs.vl.to_uint() + 7) / 8;
            if (e >= evl) {
                done = true;   // vl=0: no hay nada que mover
            } else if (axi_grant()) {
                ap_uint<32> ba = base + e;
                uint32_t wi = (ba >> 2) & (OOO_DMEM_WORDS - 1);
                int shift = (ba & 3) * 8;
                if (vec_rs.is_load) {
                    ap_uint<32> v = (dmem[wi] >> shift) & ap_uint<32>(0xFF);
                    vreg_set(vregs, vec_rs.vd_or_vs3.to_uint(), e, 1, v);
                } else {
                    ap_uint<32> v = vreg_get(vregs, vec_rs.vd_or_vs3.to_uint(), e, 1);
                    dmem[wi] = (dmem[wi] & ap_uint<32>(~(0xFFu << shift))) |
                               ((v & ap_uint<32>(0xFF)) << shift);
                }
                if (e + 1 < evl) vec_rs.elem = e + 1; else done = true;
            }
        } else {
            // ---- unit-stride / strided / indexado, con o sin segmentos ----
            uint32_t vl_eff = vec_rs.vl.to_uint();

            if (vec_rs.lsmode == rvv::LSM_FOF && e == 0 && f == 0) {
                // ---- fault-only-first: se resuelve ANTES de mover nada ----
                // Solo el elemento 0 puede atrapar; si falla uno posterior
                // se RECORTA vl y el software reintenta. El chequeo es
                // solo de rangos (no hay MMU): no gasta el puerto.
                uint32_t trim = vl_eff;
                for (int k = 0; k < rvv::MAX_ELEMS; k++) {
#pragma HLS UNROLL
                    if (k < (int)vl_eff) {
                        ap_uint<32> ba = base + k * sb;
                        bool fault = (ba.to_uint() >= OOO_DMEM_WORDS * 4u);
                        if (fault && (uint32_t)k < trim) trim = k;
                    }
                }
                if (trim == 0 && vl_eff > 0) {
                    rob[vec_rs.rob_tag].takes_trap = true;
                    rob[vec_rs.rob_tag].cause = 5;  // load access fault
                    vl_eff = 0;
                } else if (trim < vl_eff) {
                    csr_vl = trim;      // recorte visible al software
                    vl_eff = trim;
                }
                vec_rs.vl = vl_eff;     // el resto de los ciclos usa el recortado
            }

            uint32_t vst = vec_rs.vstart.to_uint();  // Fase 6
            if (e >= vl_eff) {
                // no quedan elementos en este campo del segmento
                if (f + 1 < nfld) { vec_rs.fld = f + 1; vec_rs.elem = 0; }
                else done = true;
            } else if (e < vst || !vec_elem_active(vec_rs, (int)e, vregs)) {
                vec_rs.elem = e + 1;   // inactivo: avanza sin gastar puerto
            } else if (axi_grant()) {
                // --- generador de direcciones, comun a los tres modos ---
                ap_uint<32> off;
                if (indexed)
                    off = vreg_get(vregs, vec_rs.vs2.to_uint(), (int)e, ib);
                else if (vec_rs.mop == rvv::MOP_STRIDED)
                    off = ap_uint<32>(e * stride.to_uint());
                else
                    off = ap_uint<32>(e * nfld * sb);
                ap_uint<32> ba = base + off + ap_uint<32>(f * sb);
                uint32_t wi = (ba >> 2) & (OOO_DMEM_WORDS - 1);
                int shift = (ba & 3) * 8;
                uint32_t vreg = (vec_rs.vd_or_vs3.to_uint() + f) & 31;
                if (vec_rs.is_load) {
                    ap_uint<32> v = (dmem[wi] >> shift) & ap_uint<32>(emask);
                    vreg_set(vregs, vreg, (int)e, sb, v);
                } else {
                    ap_uint<32> v = vreg_get(vregs, vreg, (int)e, sb);
                    if (sb == 4) dmem[wi] = v;
                    else dmem[wi] = (dmem[wi] & ap_uint<32>(~(emask << shift))) |
                                    ((v & ap_uint<32>(emask)) << shift);
                }
                vec_rs.elem = e + 1;
            }
            // si el arbitro nego el puerto, la unidad reintenta el mismo
            // elemento el ciclo siguiente: eso ES la contencion del bus
        }

        if (done) {
            rob[vec_rs.rob_tag].ready = true;
            vec_done = 1; vec_tag = vec_rs.rob_tag;
            vec_rs.busy = false;
            // Fase 6: toda vectorial que COMPLETA resetea vstart; si
            // ATRAPO, vstart queda apuntando al elemento culpable.
            if (!rob[vec_rs.rob_tag].takes_trap) csr_vstart = 0;
        }
    }
}

#endif // VECTOR_COPROCESSOR_H
