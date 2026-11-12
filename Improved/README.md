# Improved — procesador RISC-V OoO con mayor paralelismo

**Improved** es la variante del procesador RISC-V *Out-of-Order* desarrollada para incrementar el solapamiento de operaciones independientes frente a la arquitectura Original. Mantiene el modelo de renombramiento, estaciones de reserva y retiro en orden, e introduce pipelines funcionales y mejoras en el procesamiento vectorial.

Esta distribución corresponde al **código activo final con la corrección FLW/FSW** de `frontend_dispatch.h` (versión utilizada para generar la síntesis `improved_final_csynth_100mhz_flwfix`). Los archivos con sufijos `PRE_`, `STAGE*`, `DIRECTED_PASS`, etc., son experimentos históricos y no forman parte de la versión publicada.

## Arquitectura

| Componente | Implementación |
|---|---|
| Modelo de ejecución | OoO con estaciones de reserva y commit en orden |
| Reorder Buffer (ROB) | **8 entradas** |
| ALU enteras | **2** |
| Renombramiento escalar | RAT y FRAT |
| Predicción de saltos | TAGE |
| Multiplicación entera | Pipeline de **3 ticks** del modelo |
| FADD/FSUB/FMUL | Pipeline FP de **3 ticks** del modelo |
| FDIV/FSQRT | Pipeline FP de **8 ticks** del modelo |
| Subsistema vectorial | VRAT, almacenamiento de resultados pendientes y rutas para VALU / VMUL |
| Top HLS | `riscv_soc_tick()` |

El código contempla instrucciones enteras, comprimidas, de punto flotante y un subconjunto de operaciones vectoriales RVV utilizado en los benchmarks. **No debe interpretarse como una certificación de conformidad completa** con todas las extensiones RISC-V.

### Recorrido de una instrucción

1. **Fetch y decode:** lectura desde `imem`, decodificación y predicción de saltos.
2. **Renombramiento y dispatch:** asignación de entrada ROB y estación de reserva; RAT/FRAT/VRAT identifican productores pendientes.
3. **Issue y ejecución:** las operaciones avanzan cuando disponen de operandos y unidad compatible; instrucciones independientes pueden completarse fuera de orden.
4. **Completion:** los resultados actualizan su entrada de ROB y se difunden a consumidores dependientes.
5. **Commit:** retiro desde la cabeza del ROB, en orden, para actualizar el estado arquitectónico.

El flujo principal está implementado en `soc_top.cpp`; las estructuras del procesador se encuentran en `soc_state.h` y la lógica de frontend en `frontend_dispatch.h`.

## Mejoras respecto a Original

- **MUL canalizada:** permite solapar multiplicaciones independientes sin esperar necesariamente a que finalice la anterior.
- **FPU canalizada:** las familias FP compatibles pueden mantener varias operaciones en vuelo, con restricciones entre determinados grupos de operaciones.
- **Renombramiento vectorial (VRAT):** rastrea productores de registros vectoriales; los resultados temporales se guardan asociados al ROB.
- **Rutas vectoriales separadas:** `vec_rs` y `vec_mul_rs` permiten mayor concurrencia entre operaciones generales y de multiplicación/división vectorial.

La mejora principal se relaciona con el **throughput del modelo microarquitectónico**, no con una garantía automática de que cada tick necesite un único reloj físico tras HLS.

## Estructura recomendada

```text
Improved/
├── README.md
├── RISCV-SoC/
│   ├── soc_top.cpp, soc_top.h
│   ├── soc_config.h, soc_state.h, soc_csr.h
│   ├── frontend_dispatch.h, backend.h, axi_interconnect.h
│   ├── rv32i_defs.h, rv32c_defs.h, rvv_encoding.h
│   ├── immediates_hls.h, fp_ops.h, tage.h
│   ├── exec_vector.h, vector_dispatch.h, vector_coprocessor.h
│   ├── soc_tb.cpp
│   ├── baremetal_benchmark_tb.cpp, baremetal_test_elf.h
│   ├── vector_long_64pairs_tb.cpp
│   ├── mixed_mul_fpu_vrat_32_tb.cpp
│   ├── vector_vrat_benchmark_tb.cpp
│   └── trap_elf.h, full_elf.h, printf_elf.h
├── scripts/                  # TCL seleccionados y adaptados
├── benchmarks/               # programas/ELF y pruebas extra, si se incluyen
├── results/                  # mediciones y reportes con procedencia verificada
└── third_party/ap_types/     # encabezados para C++ local + licencia
```

Los nombres de archivo anteriores son los de la versión funcional. Mantener `RISCV-SoC/` con los testbenches junto al código permite usar directamente los comandos de compilación siguientes. Si se mueven testbenches a `benchmarks/`, actualizar también las rutas de compilación y TCL.

## Reproducir las pruebas microarquitectónicas

Se requiere `g++` con soporte C++14. Desde **`Improved/`** (en Linux/WSL o entorno equivalente):

```bash
# Bare-metal embebido: AXPY + GEMM
g++ -std=c++14 -O2 -I RISCV-SoC -I third_party/ap_types/include \
  RISCV-SoC/soc_top.cpp RISCV-SoC/baremetal_benchmark_tb.cpp -o improved_baremetal
./improved_baremetal

# VECTOR64: 64 pares de operaciones vectoriales
g++ -std=c++14 -O2 -I RISCV-SoC -I third_party/ap_types/include \
  RISCV-SoC/soc_top.cpp RISCV-SoC/vector_long_64pairs_tb.cpp -o improved_vector64
./improved_vector64

# MIXED32: multiplicación entera + FPU + vectorial
g++ -std=c++14 -O2 -I RISCV-SoC -I third_party/ap_types/include \
  RISCV-SoC/soc_top.cpp RISCV-SoC/mixed_mul_fpu_vrat_32_tb.cpp -o improved_mixed32
./improved_mixed32
```

Estos tres comandos se **compilaron y ejecutaron con éxito usando exclusivamente los 17 archivos activos del núcleo, los testbenches y encabezados requeridos**, además de `third_party/ap_types`.

| Benchmark C++ local | Ticks del modelo | Commits | Resultado |
|---|---:|---:|---|
| Bare-metal embebido | **76 295** | **44 367** | PASS: AXPY y GEMM |
| VECTOR64 | **271** | **132** | PASS |
| MIXED32 | **407** | **396** | PASS |

Los ticks anteriores son unidades del **modelo C++**, no ciclos físicos del reloj `ap_clk`.

## Síntesis y simulación RTL

- **Herramientas empleadas:** Vitis HLS / Vivado **2024.1**.
- **Dispositivo:** `xck26-sfvc784-2LV-c`.
- **Reloj objetivo Improved:** **10 ns (100 MHz)**.
- **Top HLS:** `riscv_soc_tick`.
- **Síntesis final:** `run_improved_final_csynth_100mhz_flwfix.tcl` (recomendado renombrar a `run_improved_csynth.tcl` después de actualizar su ruta `ROOT`).

Los TCL originales usan rutas absolutas de Windows. Antes de publicar o ejecutar desde otra computadora, cambiarlas por rutas relativas y revisar las referencias a proyectos, testbenches e IP de FPU generadas.

**Resultado RTL verificado de bare-metal FLWFIX (XSIM, 100 MHz objetivo):**

| Métrica | Resultado |
|---|---:|
| Ticks microarquitectónicos | **76 383** |
| Instrucciones retiradas | **44 377** |
| Ciclos `ap_clk` | **14 832 644** |
| Ciclos `ap_clk` por tick | **194,19** |
| Tiempo simulado a 100 MHz | **148,32644 ms** |
| Firmas de AXPY / GEMM / ALL | **PASS / PASS / PASS** |
| GEMM tests / signature / DONE | **3 / `0xDEADBEEF` / `0x000005A5`** |

El resultado se obtuvo con un **ELF común externo** a las pruebas C++ embebidas. En particular, el ELF del bare-metal embebido en `baremetal_test_elf.h` no es idéntico al ELF externo utilizado en la validación RTL FLWFIX, por lo que **76 295 y 76 383 ticks no deben atribuirse al mismo ejecutable**.

Las mediciones RTL históricas de **VECTOR64 (42 836 clocks del cuerpo)** y **MIXED32 (67 733 clocks del cuerpo)** corresponden al RTL previo a FLWFIX. Deben etiquetarse como históricas hasta repetirlas con la versión final. Los testbenches XSIM del bare-metal completo y sus archivos `.mem` se encontraban en una carpeta externa de comparación y deben agregarse de manera depurada si se desea reproducir esa medición desde este repositorio.

### ¿Por qué hay tantos clocks por tick?

`riscv_soc_tick()` describe un avance completo del procesador en el **modelo**, pero Vitis HLS calendariza sus operaciones internas durante varios ciclos del **reloj físico simulado**. El reporte final de síntesis estima **138–1509 ciclos por llamada**, con intervalo de iniciación **139–1510** y **sin pipeline global**. Esto es coherente con el promedio de **194,19 `ap_clk`/tick** observado en la simulación RTL. No implica que cada instrucción RISC-V necesite inherentemente esa cantidad de clocks.

## Validación de la corrección FLW/FSW

En `frontend_dispatch.h` se añadió una exclusión para `f3 == 2` al clasificar determinadas instrucciones `LOAD_FP` / `STORE_FP` como vectoriales. Así se evita que instrucciones escalares `FLW/FSW` se interpreten erróneamente como operaciones vectoriales. Tras regenerar el RTL, el diagnóstico AXPY produjo sus ocho valores correctos y el benchmark bare-metal completo terminó en **PASS**.

## Continuación recomendada

1. Incorporar los scripts XSIM y memorias externas usados para bare-metal, con sus hashes y ventanas de medición.
2. Repetir VECTOR64 y MIXED32 con el RTL FLWFIX para cerrar una tabla de tres benchmarks homogénea.
3. Verificar síntesis, recursos y timing con un flujo reproducible de implementación física.
4. Estudiar la calendarización HLS, dependencias entre etapas y alternativas RTL explícitas; evaluar enfoques metodológicos como LEAF/LEAFs solamente después de confirmar su adecuación.

## Licencias

`third_party/ap_types/` está acompañado por una licencia Apache 2.0 propia. El código principal del TFG necesita una decisión de licencia independiente antes de su publicación.
