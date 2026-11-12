/* Interfaz de biblioteca matemática para álgebra lineal densa en RV32IMFC + RVV 1.0.
 * Declara kernels AXPY y GEMM optimizados para ejecución vectorial. */
#ifndef RVV_MATH_H
#define RVV_MATH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef float rvv_fp32;

void rvv_axpy_f32(size_t n, rvv_fp32 alpha, const rvv_fp32 *x, rvv_fp32 *y);
void rvv_gemm_f32(size_t m, size_t n, size_t k,
                  const rvv_fp32 *a, const rvv_fp32 *b, rvv_fp32 *c);

#ifdef __cplusplus
}
#endif

#endif
