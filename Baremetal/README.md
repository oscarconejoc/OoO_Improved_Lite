# Bare Metal RVV Math SW library

Proyecto base para una librería de álgebra lineal densa en C para una plataforma bare-metal RV32IMFC + RVV 1.0. Implementa operaciones altamente optimizadas con matrices grandes(AXPY y GEMM), todo esto sin sistema operativo y midiendo el performance a través de contadores en el hardware.

Tanto AXPY como GEMM pertenecen a una biblioteca estándar de matemáticas llamada BLAS (Basic Linear Algebra Subprograms).

- XPY(A X Plus Y) opera con vectores (arreglos unidimensionales o listas de números). Multiplica un escalar (un número simple, α) por un vector X, y le suma otro vector Y. El resultado se guarda en Y.

- En el caso de GEMM(NO confundir con gem5), es  una operación que opera con matrices (bloques bidimensionales de números). Multiplica una matriz A por una matriz B (escalado por α), y le suma una matriz C (escalada por β).

## Estructura

- `include/` : cabeceras del proyecto
- `src/` : implementaciones de AXPY, GEMM y el testbench
- `gem5/` : scripts de integración y configuración de Gem5
- `Makefile` : reglas básicas de compilación
- `test_reference.py` : validación de referencia en Python
- `inspect_binary.py` : herramienta para inspeccionar el binario
- `parse_metrics.py` : convierte `gem5/out/stats.txt` en CSV legible

---

## Architectura del proyecto

```
┌─────────────────────────────────────────────┐
│          main.c (Testbench)                │
│  - Inicializa los datos                    │
│  - Corre AXPY y GEMM                       │
│  - Mide el performance y valida            │
└────────────────────┬────────────────────────┘
                     │
         ┌───────────┴───────────┐
         │                       │
    ┌────▼──────────┐  ┌────────▼──────┐
    │ rvv_math.h    │  │   hal.h       │
    │ (Interfaz)    │  │ (Acceso a CSR)│
    └────┬──────────┘  └────────┬──────┘
         │                      │
    ┌────▼──────────┐  ┌────────▼──────┐
    │ axpy_rvv.c   │  │ gemm_rvv.c    │
    │ AXPY kernel  │  │ GEMM kernel   │
    └──────────────┘  └───────────────┘
         │                      │
    ┌────▼──────────────────────▼──────┐
    │      metrics.c / result.c        │
    │ (Los datos se guardan en binarios│
    │  Y se leen con scripts de python)│
    └─────────────────────────────────┘
```

---

## Archivo por archivo

- `include/rvv_math.h` : declara las rutinas `rvv_axpy_f32` y `rvv_gemm_f32`, la interfaz principal del software.
- `include/hal.h` : abstracción mínima del hardware bare-metal, incluye lectura de CSRs de ciclo e instrucciones.
- `include/metrics.h` : define la interfaz para reportar métricas de ejecución a memoria conocida.
- `include/result.h` : define la estructura de resultados y benchmark records para post-análisis.
- `src/axpy_rvv.c` : implementación del kernel AXPY; usa RVV si está disponible y un fallback scalar en caso contrario.
- `src/gemm_rvv.c` : implementación de GEMM optimizada con RVV en la dimensión de columnas, con fallback scalar.
- `src/main.c` : testbench bare-metal; ejecuta AXPY/GEMM, valida resultados y almacena métricas y estados finales.
- `src/metrics.c` : backend que guarda métricas de tiempo en secciones estáticas del binario.
- `src/result.c` : estructura de reporte de resultados y almacenamiento de benchmarks para inspección post-ejecución.
- `link.ld` : linker script base que define secciones de texto, datos, bss y resultados.
- `gem5/run_gem5.sh` : wrapper para lanzar Gem5 con el binario del proyecto.
- `gem5/configs/simple_riscv_baremetal.py` : configuración auxiliar para pruebas simples en Gem5.
- `gem5/README.md` : notas de uso específicas de la integración con Gem5.
- `test_reference.py` : referencia en Python que valida las operaciones AXPY y GEMM.
- `inspect_binary.py` : inspecciona símbolos y secciones del binario para ayudar a localizar datos exportados.
- `parse_metrics.py` : formato de salida legible para las métricas capturadas por el testbench.

## Requisitos

Para compilar este proyecto necesitas un toolchain de RISC-V compatible con bare-metal, por ejemplo:

- `riscv32-unknown-elf-gcc`
- `riscv32-unknown-elf-objdump` (opcional)

## Compilación

Desde la raíz del proyecto ejecuta:

```bash
make
```

Esto genera el binario en:

```bash
build/bare_metal_test
```

## Validación sin print

Como el código bare-metal **no tiene acceso a printf**, los resultados se almacenan en una sección dedicada de memoria (`.results`) y Gem5 expone las estadísticas de ejecución en `gem5/out/stats.txt`.

### Benchmarks de GEMM

El testbench ahora ejecuta varios micro-benchmarks de GEMM para tamaños de matriz 4x4x4, 8x8x8 y 16x16x16, y captura métricas de:
- ciclos
- instrucciones ejecutadas
- tamaño de problema (m, n, k)

Estas métricas quedan almacenadas en la sección `.results` y `.metrics` para inspección posterior desde un debugger, un simulador o un script automatizado.

### Exportación legible de métricas

Puedes procesar el resultado con:

```bash
python3 parse_metrics.py gem5/out/stats.txt
```

También acepta el directorio `gem5/out` directamente.

El script produce un formato simple tipo CSV:

```text
benchmark_id,m,n,k,flops,weight,est_cycles,est_instret,sim_seconds_total,host_seconds_total,note
0,4,4,4,128,0.013699,68493,68493,0.002500,1.140000,estimated_from_aggregate_stats
1,8,8,8,1024,0.109589,547945,547945,0.002500,1.140000,estimated_from_aggregate_stats
2,16,16,16,8192,0.876712,4383563,4383563,0.002500,1.140000,estimated_from_aggregate_stats
```

Interpretación rápida:
- `est_cycles` y `est_instret` son **estimaciones por benchmark** obtenidas al repartir los contadores agregados de Gem5 según peso de cómputo (`FLOPs = 2*m*n*k`).
- `weight` es la fracción de carga computacional de cada benchmark respecto al total de la corrida.
- `sim_seconds_total` y `host_seconds_total` corresponden al tiempo total de la simulación (mismo valor repetido por fila para referencia).

## Flujo típico de desarrollo

### 1. Compilar el proyecto

```bash
make
```

Genera `build/bare_metal_test` listo para simular o depurar.

### 2. Validar con Python (recomendado)

```bash
python3 test_reference.py
```

Esto compara los cálculos esperados con los resultados que daría el binario, sin necesidad de simulador:

```
✓ All reference tests PASSED
```

### 3. Inspeccionar el binario (avanzado)

```bash
python3 inspect_binary.py build/bare_metal_test
```

Muestra los símbolos y secciones del binario, incluyendo dónde se almacenan los resultados en memoria.

### 4. Simular con Gem5

Ejecuta la simulación bare-metal RISC-V:

```bash
./gem5/run_gem5.sh --gem5-bin=<YOUR_GEM5_REPO_PATH>/gem5/build/RISCV/gem5.opt --maxinsts=5000000
```

Notas:
- `--maxinsts` evita que la simulación quede indefinida en modo depuración.
- Si omites `--gem5-bin`, el script intenta autodetectarlo.

### 5. Analizar datos post-ejecución

Inspección de símbolos/secciones del binario:

```bash
python3 inspect_binary.py build/bare_metal_test
```

Parseo de métricas de Gem5 a CSV:

```bash
python3 parse_metrics.py gem5/out/stats.txt
```

También puedes pasar el directorio `gem5/out` directamente:

```bash
python3 parse_metrics.py gem5/out
```

Para ver una tabla alineada en consola (más legible para inspección rápida):

```bash
python3 parse_metrics.py gem5/out --format pretty
```

### 6. Modificar e iterar

Para modificar los kernels:

1. Edita `src/axpy_rvv.c` o `src/gemm_rvv.c`
2. Ejecuta `make` para compilar
3. Valida con `python3 test_reference.py`
4. Si necesitas depuración detallada, usa Gem5 con `./gem5/run_gem5.sh` (cuando el binario esté disponible)

## Estructura de Resultados

El testbench almacena los resultados en la estructura `test_results_t`:

```c
typedef struct {
    uint32_t axpy_ok;    // 1 si AXPY pasó, 0 si falló
    uint32_t gemm_ok;    // 1 si GEMM pasó, 0 si falló
    uint32_t all_ok;     // 1 si todos los tests pasaron
    uint32_t reserved;   // Valor marcador: 0xDEADBEEF
} test_results_t;
```

## Limpieza

```bash
make clean
```

## Uso de IA
Se utilizó la herramienta github copilot para el planeamiento y estrategia de diseño, generación y/o depuración de secciones de código y para generar la mayoría de scripts en este repositorio.

## Notas

- El código está pensado para correr en un entorno bare-metal.
- No se usan librerías estándar de Linux ni `printf`/`malloc`.
- La implementación inicial es una base sencilla para la fase 1 del desarrollo.
- Los resultados se validan contra una referencia en Python para asegurar correctitud funcional.


