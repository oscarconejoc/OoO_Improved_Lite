/* Metricas de performance backend: guarda y reporta ciclo CPU y conteo de instrucciones.
 * Diseñado para integración con testbench para medir la eficiencia de ejecución de kernels. */
#include "metrics.h"

#define GEMM_BENCHMARK_LIMIT 4u

/* Storage estatico para métricas del testbench. */
static hal_metrics_t __attribute__((section(".metrics"))) axpy_perf = {0, 0};
static hal_metrics_t __attribute__((section(".metrics"))) gemm_perf = {0, 0};
static gemm_benchmark_result_t __attribute__((section(".metrics"))) gemm_benchmarks[GEMM_BENCHMARK_LIMIT];
static uint32_t gemm_benchmark_count = 0u;

void metrics_report_axpy(const hal_metrics_t *m) {
    axpy_perf = *m;
}

void metrics_report_gemm(const hal_metrics_t *m) {
    gemm_perf = *m;
}

void metrics_report_gemm_benchmark(uint32_t index, uint32_t m, uint32_t n, uint32_t k,
                                   const hal_metrics_t *mtr) {
    if (index >= GEMM_BENCHMARK_LIMIT) {
        return;
    }

    gemm_benchmarks[index].m = m;
    gemm_benchmarks[index].n = n;
    gemm_benchmarks[index].k = k;
    gemm_benchmarks[index].metrics = *mtr;

    if (index + 1u > gemm_benchmark_count) {
        gemm_benchmark_count = index + 1u;
    }
}

uint32_t metrics_gemm_benchmark_count(void) {
    return gemm_benchmark_count;
}
