/* Guardado de resultados: se coloca en una sección dedicada del linker para su inspección post-ejecución. */
#include "result.h"

volatile test_results_t __attribute__((section(".results"))) __results_section = {
    .axpy_ok = 0,
    .gemm_ok = 0,
    .all_ok = 0,
    .reserved = 0xDEADBEEF
};

volatile benchmark_record_t __attribute__((section(".results"))) __benchmark_records[4] = {0};
volatile uint32_t __attribute__((section(".results"))) __benchmark_count = 0u;
