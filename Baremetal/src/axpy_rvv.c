/* implementación del kernel AXPY; 
usa RVV si está disponible y un metodo scalar en caso contrario. */
#include "rvv_math.h"

#if defined(__riscv_v)
/* implementación RVV: Kernel Agnóstico al Largo del Vector usando el multiplicador m1. */
void rvv_axpy_f32(size_t n, rvv_fp32 alpha, const rvv_fp32 *x, rvv_fp32 *y) {
    size_t i = 0;

    while (i < n) {
        /* Se asigna el length para los vectores; vl son los elementos por iteracion. */
        size_t vl = __riscv_vsetvl_e32m1(n - i);

        /* Se carga el segmento del vector: x_vec = x[i:i+vl], y_vec = y[i:i+vl] */
        vfloat32m1_t x_vec = __riscv_vle32_v_f32m1(&x[i], vl);
        vfloat32m1_t y_vec = __riscv_vle32_v_f32m1(&y[i], vl);

        /* FMACC: y_vec = alpha * x_vec + y_vec (fused multiply-accumulate) */
        vfloat32m1_t result = __riscv_vfmacc_vf_f32m1(y_vec, alpha, x_vec, vl);

        /* Se almacena el resultado: y[i:i+vl] = result */
        __riscv_vse32_v_f32m1(&y[i], result, vl);

        i += vl;
    }
}
#else
/* metodo escalar: Cuando no hay RVV */
void rvv_axpy_f32(size_t n, rvv_fp32 alpha, const rvv_fp32 *x, rvv_fp32 *y) {
    size_t i;

    for (i = 0; i < n; ++i) {
        y[i] = alpha * x[i] + y[i];
    }
}
#endif
