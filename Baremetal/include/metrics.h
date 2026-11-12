/* Métricas de performance: interfaz de recolección y reporte.
 * Captura y almacena ciclos e instrucciones para benchmarking. */
#ifndef METRICS_H
#define METRICS_H

#include <stdint.h>
#include "hal.h"

typedef struct {
    uint32_t m;
    uint32_t n;
    uint32_t k;
    hal_metrics_t metrics;
} gemm_benchmark_result_t;

/* Reporta métricas a una ubicación de memoria conocida para la verificación del testbench. */
void metrics_report_axpy(const hal_metrics_t *m);
void metrics_report_gemm(const hal_metrics_t *m);
void metrics_report_gemm_benchmark(uint32_t index, uint32_t m, uint32_t n, uint32_t k,
                                   const hal_metrics_t *mtr);
uint32_t metrics_gemm_benchmark_count(void);

#endif
