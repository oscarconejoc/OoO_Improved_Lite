/* Bare-metal testbench para rutinas AXPY y GEMM.
 * Valida la funcionalidad y mide el rendimiento en datos de prueba estáticos. */
#include "hal.h"
#include "rvv_math.h"
#include "metrics.h"
#include "result.h"

static rvv_fp32 x_vec[8] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
static rvv_fp32 y_vec[8] = {0.5f, 1.5f, 2.5f, 3.5f, 4.5f, 5.5f, 6.5f, 7.5f};

static rvv_fp32 a_mat[16] = {
    1.0f, 2.0f, 3.0f, 4.0f,
    5.0f, 6.0f, 7.0f, 8.0f,
    9.0f, 10.0f, 11.0f, 12.0f,
    13.0f, 14.0f, 15.0f, 16.0f,
};
static rvv_fp32 b_mat[16] = {
    1.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 1.0f,
    1.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 1.0f,
};
static rvv_fp32 c_mat[256];
static rvv_fp32 gemm_bench_a[256];
static rvv_fp32 gemm_bench_b[256];
static rvv_fp32 gemm_bench_c[256];

static void fill_gemm_benchmark_inputs(rvv_fp32 *a, rvv_fp32 *b, rvv_fp32 *c,
                                       size_t m, size_t n, size_t k) {
    size_t row;
    size_t col;

    for (row = 0; row < m; ++row) {
        for (col = 0; col < k; ++col) {
            a[row * k + col] = (rvv_fp32)((row + 1) * 0.25f + (col + 1) * 0.5f);
        }
    }

    for (row = 0; row < k; ++row) {
        for (col = 0; col < n; ++col) {
            b[row * n + col] = (rvv_fp32)((row + 1) * 0.5f + (col + 1) * 0.25f);
        }
    }

    for (row = 0; row < m; ++row) {
        for (col = 0; col < n; ++col) {
            c[row * n + col] = 0.0f;
        }
    }
}

static int check_axpy(void) {
    size_t i;
    for (i = 0; i < 8; ++i) {
        rvv_fp32 expected = 1.5f * x_vec[i] + y_vec[i];
        if (c_mat[i] != expected) {
            return 0;
        }
    }
    return 1;
}

static int check_gemm(void) {
    const rvv_fp32 expected[16] = {
        4.0f, 6.0f, 4.0f, 6.0f,
        12.0f, 14.0f, 12.0f, 14.0f,
        20.0f, 22.0f, 20.0f, 22.0f,
        28.0f, 30.0f, 28.0f, 30.0f,
    };
    size_t i;

    for (i = 0; i < 16; ++i) {
        if (c_mat[8 + i] != expected[i]) {
            return 0;
        }
    }
    return 1;
}

int main(void) {
    hal_metrics_t start_metrics;
    hal_metrics_t end_metrics;
    hal_metrics_t axpy_metrics;
    hal_metrics_t gemm_metrics;
    int ok;

    /* Inicializa los vectores y matrices de prueba. */
    for (size_t i = 0; i < 8; ++i) {
        c_mat[i] = y_vec[i];
    }
    for (size_t i = 8; i < 256; ++i) {
        c_mat[i] = 0.0f;
    }

    /* Mide el performance de AXPY */
    hal_metrics_capture(&start_metrics);
    rvv_axpy_f32(8, 1.5f, x_vec, c_mat);
    hal_metrics_capture(&end_metrics);
    axpy_metrics = hal_metrics_delta(&end_metrics, &start_metrics);
    metrics_report_axpy(&axpy_metrics);

    /* Mide el performance de GEMM */
    hal_metrics_capture(&start_metrics);
    rvv_gemm_f32(4, 4, 4, a_mat, b_mat, c_mat + 8);
    hal_metrics_capture(&end_metrics);
    gemm_metrics = hal_metrics_delta(&end_metrics, &start_metrics);
    metrics_report_gemm(&gemm_metrics);

    /* Micro-benchmarks de GEMM para múltiples tamaños */
    {
        const size_t sizes[][3] = {
            {4, 4, 4},
            {8, 8, 8},
            {16, 16, 16},
        };
        const size_t benchmark_count = sizeof(sizes) / sizeof(sizes[0]);

        for (size_t bench = 0; bench < benchmark_count; ++bench) {
            size_t m = sizes[bench][0];
            size_t n = sizes[bench][1];
            size_t k = sizes[bench][2];

            fill_gemm_benchmark_inputs(gemm_bench_a, gemm_bench_b, gemm_bench_c, m, n, k);
            hal_metrics_capture(&start_metrics);
            rvv_gemm_f32(m, n, k, gemm_bench_a, gemm_bench_b, gemm_bench_c);
            hal_metrics_capture(&end_metrics);
            gemm_metrics = hal_metrics_delta(&end_metrics, &start_metrics);
            metrics_report_gemm_benchmark((uint32_t)bench, (uint32_t)m, (uint32_t)n, (uint32_t)k,
                                          &gemm_metrics);

            __benchmark_records[bench].m = (uint32_t)m;
            __benchmark_records[bench].n = (uint32_t)n;
            __benchmark_records[bench].k = (uint32_t)k;
            __benchmark_records[bench].cycles = gemm_metrics.cycles;
            __benchmark_records[bench].instret = gemm_metrics.instret;
        }

        __benchmark_count = (uint32_t)benchmark_count;
    }

    /* Valida resultados y reporta a la sección de resultados */
    int axpy_ok = check_axpy();
    int gemm_ok = check_gemm();
    ok = axpy_ok && gemm_ok;

    __results_section.axpy_ok = axpy_ok ? 1 : 0;
    __results_section.gemm_ok = gemm_ok ? 1 : 0;
    __results_section.all_ok = ok ? 1 : 0;

    /* Ejecución completa; los resultados se pueden inspeccionar en memoria o mediante el depurador. */
    hal_finish();
    return 0;
}
