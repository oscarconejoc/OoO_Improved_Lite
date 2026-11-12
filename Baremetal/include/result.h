/* Reporte de resultados para inspección del testbench. */
#ifndef RESULT_H
#define RESULT_H

#include <stdint.h>

typedef struct {
    uint32_t axpy_ok;
    uint32_t gemm_ok;
    uint32_t all_ok;
    uint32_t reserved;
} test_results_t;

typedef struct {
    uint32_t m;
    uint32_t n;
    uint32_t k;
    uint64_t cycles;
    uint64_t instret;
} benchmark_record_t;

extern volatile test_results_t __results_section;
extern volatile benchmark_record_t __benchmark_records[4];
extern volatile uint32_t __benchmark_count;

#endif
