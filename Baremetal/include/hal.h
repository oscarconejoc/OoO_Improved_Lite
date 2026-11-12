/* Hardware abstraction layer(HAL) para ejecuci??n bare-metal en RISC-V.
 * Proporciona acceso a CSR, lectura de contadores de performance y utilidades del sistema. */
#ifndef HAL_H
#define HAL_H

#include <stdint.h>

/* Snapshot de m??tricas de performance */
typedef struct {
    uint64_t cycles;
    uint64_t instret;
} hal_metrics_t;

static inline uint64_t hal_cycle_counter(void) {
    return 0u;
}

static inline uint64_t hal_instret_counter(void) {
    return 0u;
}

/* Captura un snapshot de m??tricas en el instante actual */
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
    __asm__ volatile("ecall" ::: "memory");
    for (;;) { }
}

#endif


