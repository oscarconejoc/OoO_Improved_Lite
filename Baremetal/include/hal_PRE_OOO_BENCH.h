/* Hardware abstraction layer(HAL) para ejecución bare-metal en RISC-V.
 * Proporciona acceso a CSR, lectura de contadores de performance y utilidades del sistema. */
#ifndef HAL_H
#define HAL_H

#include <stdint.h>

/* Snapshot de métricas de performance */
typedef struct {
    uint64_t cycles;
    uint64_t instret;
} hal_metrics_t;

static inline uint64_t hal_cycle_counter(void) {
#if defined(__riscv) && defined(__GNUC__)
    uint64_t value;
    __asm__ volatile("rdcycle %0" : "=r"(value));
    return value;
#else
    return 0u;
#endif
}

static inline uint64_t hal_instret_counter(void) {
#if defined(__riscv) && defined(__GNUC__)
    uint64_t value;
    __asm__ volatile("rdinstret %0" : "=r"(value));
    return value;
#else
    return 0u;
#endif
}

/* Captura un snapshot de métricas en el instante actual */
static inline void hal_metrics_capture(hal_metrics_t *m) {
    m->cycles = hal_cycle_counter();
    m->instret = hal_instret_counter();
}

/* Captura el delta entre dos snapshots */
static inline hal_metrics_t hal_metrics_delta(const hal_metrics_t *end,
                                              const hal_metrics_t *start) {
    hal_metrics_t delta;
    delta.cycles = end->cycles - start->cycles;
    delta.instret = end->instret - start->instret;
    return delta;
}

static inline void hal_finish(void) {
    for (;;) {
        __asm__ volatile("" ::: "memory");
    }
}

#endif
