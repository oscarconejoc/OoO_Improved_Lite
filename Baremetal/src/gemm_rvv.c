/* GEMM kernel: C = A * B + C para matrices densas FP32.
 * Implementación vectorizada usa intrínsecos RVV cuando están disponibles.
 * Estrategia: para cada fila de A y cada bloque de columnas de C, se calcula
 * la acumulación a través de k realizando scalar A[i,k] * B[k, j:j+vl]
 * donde el lane del vector itera a través de las columnas (dimensión n).
 */
#include "rvv_math.h"

#if defined(__riscv_v)
#include <riscv_vector.h>

void rvv_gemm_f32(size_t m, size_t n, size_t k,
                  const rvv_fp32 *a, const rvv_fp32 *b, rvv_fp32 *c) {
    for (size_t i = 0; i < m; ++i) {
        size_t j = 0;
        while (j < n) {
            size_t vl = __riscv_vsetvl_e32m1(n - j);
            /* cargar el segmento(fila) de C */
            vfloat32m1_t c_vec = __riscv_vle32_v_f32m1(&c[i * n + j], vl);

            /* Se acumula sobre k usando estilo de producto externo */
            for (size_t p = 0; p < k; ++p) {
                float a_scalar = a[i * k + p];
                vfloat32m1_t b_vec = __riscv_vle32_v_f32m1(&b[p * n + j], vl);
                c_vec = __riscv_vfmacc_vf_f32m1(c_vec, a_scalar, b_vec, vl);
            }

            /* Se almacena el segmento de C actualizado */
            __riscv_vse32_v_f32m1(&c[i * n + j], c_vec, vl);
            j += vl;
        }
    }
}

#else

void rvv_gemm_f32(size_t m, size_t n, size_t k,
                  const rvv_fp32 *a, const rvv_fp32 *b, rvv_fp32 *c) {
    /* metodo escalar: bucle triple directo */
    for (size_t row = 0; row < m; ++row) {
        for (size_t col = 0; col < n; ++col) {
            rvv_fp32 acc = c[row * n + col];
            for (size_t inner = 0; inner < k; ++inner) {
                acc += a[row * k + inner] * b[inner * n + col];
            }
            c[row * n + col] = acc;
        }
    }
}

#endif

