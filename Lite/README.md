# Lite — procesador RISC-V OoO reducido

**Lite** es la variante de menor complejidad del procesador RISC-V *Out-of-Order* desarrollado para este TFG. Su finalidad es estudiar la reducción de recursos hardware manteniendo los mecanismos esenciales de ejecución fuera de orden. Esta distribución conserva **únicamente la versión funcional final procedente de `LITE_OPT11_INLINE_EXPAND`**, presentada públicamente con el nombre **Lite**.

## Arquitectura

- **ISA:** RV32IMC implementada para las pruebas del proyecto (entero, multiplicación/división y comprimidas); no incluye soporte funcional de FPU ni RVV.
- **ROB:** 4 entradas; retiro arquitectónico en orden.
- **ALU:** 1 unidad de ejecución entera compartida por **3 estaciones de reserva**.
- **Renombramiento:** RAT para registrar productores de resultados enteros y gestionar dependencias.
- **Predicción de saltos:** política estática **not taken** (no tomado).
- **Memoria:** interfaces de memoria de instrucciones y datos definidas en `soc_top.h` y `axi_interconnect.h`.
- **Top HLS:** `riscv_soc_tick()` en `RISCV-SoC/soc_top.cpp`.

Las estaciones de reserva permiten preparar y seleccionar instrucciones cuyos operandos están disponibles; el ROB mantiene el retiro en orden. Aunque Lite conserva ejecución fuera de orden, **la capacidad de emisión hacia su ALU está limitada a una operación entera a la vez**.

## Cambio conservado de OPT11

La mejora puntual de esta versión incorpora `#pragma HLS INLINE` en la expansión de instrucciones comprimidas `rv32c::expand()`, dentro de `rv32c_defs.h`. La intención es reducir el costo de control asociado al frontend al sintetizarlo mediante HLS.

Resultados registrados en `OPT11_VALIDATED_RESULT.txt`:

| Verificación | Resultado |
|---|---:|
| CSim | PASS |
| Síntesis HLS | Completada |
| Estimación Fmax HLS | 252,27 MHz |
| Simulación RTL @ 200 MHz | PASS |
| Prueba de control | 30 206 ciclos RTL |
| Lite (OPT11) | **27 789 ciclos RTL** |
| Reducción | **8,00 %** |

La reducción anterior corresponde a **la prueba específica de OPT11**, no a una media de todos los benchmarks del TFG. Una estimación de Fmax de HLS no sustituye el análisis de timing de Vivado.

## Archivos principales

```text
Lite/
├── README.md
├── Makefile
├── RISCV-SoC/
│   ├── soc_top.cpp, soc_top.h
│   ├── soc_config.h, soc_state.h, soc_csr.h
│   ├── frontend_dispatch.h, backend.h
│   ├── axi_interconnect.h
│   ├── rv32i_defs.h, rv32c_defs.h, immediates_hls.h
│   └── soc_tb_lite.cpp
├── scripts/                  # scripts TCL de Lite
├── results/                  # reportes y validación seleccionados
└── third_party/ap_types/     # headers para g++ + LICENSE.TXT
```

Los archivos del núcleo y `soc_tb_lite.cpp` deben proceder de `LITE_OPT11_INLINE_EXPAND/` y copiarse juntos a `RISCV-SoC/`. No utilizar las variantes experimentales o los respaldos de código previos.

## Ejecutar la verificación funcional

Se requiere `g++` con C++14 y `make` (Linux/WSL o entorno equivalente). Desde la carpeta **`Lite/`**:

```bash
make check
```

La receta incluida en el Makefile compila `RISCV-SoC/soc_top.cpp` junto con `RISCV-SoC/soc_tb_lite.cpp`, empleando `third_party/ap_types/include`. Se comprobó esta configuración mínima: el test finalizó con **`RESULTADO: TODOS LOS CHECKS DE LITE PASARON`**, incluyendo ROB, ALU compartida, predictor estático, instrucciones comprimidas y rechazo de instrucciones FP/vectoriales no soportadas.

Para eliminar el ejecutable de prueba:

```bash
make clean
```

## Síntesis HLS y evaluación física

**Herramientas usadas:** Vitis HLS 2024.1 y Vivado 2024.1; FPGA `xck26-sfvc784-2LV-c`; reloj objetivo **5 ns (200 MHz)**.

Los scripts de la versión original se denominaban `run_opt11_csim.tcl`, `run_opt11_csynth.tcl` y `run_opt11_postroute_ooc.tcl`. Se recomienda conservarlos en `scripts/` con nombres descriptivos de Lite y **actualizar la variable `ROOT` y las rutas RTL de Windows antes de ejecutarlos desde el nuevo repositorio**. No se consideran portables hasta completar ese cambio.

El flujo OOC (*out-of-context*) permite analizar el núcleo como bloque sin intentar asignar todos sus puertos a pines físicos. Los proyectos generados, `.dcp`, cachés y logs no son necesarios para compartir el código; conservar únicamente scripts y reportes seleccionados.

## Límites y trabajo futuro

Lite no contiene FPU, coprocesador RVV ni predictor TAGE. Por lo tanto, **no debe ejecutarse ni compararse con programas que requieran esas extensiones sin una adaptación explícita de la carga**. El ROB pequeño y la ALU única también limitan la concurrencia.

Para continuar el proyecto se recomienda integrar los scripts y testbenches externos de la comparación Lite–MicroBlaze V, indicar el binario y la ventana de medición para cada prueba, y repetir la evaluación con una metodología uniforme de clocks, recursos y funcionalidad. Se deben documentar siempre por separado los ticks del modelo y los ciclos `ap_clk` del RTL.

## Licencias

El directorio `third_party/ap_types/` incluye su propia licencia Apache 2.0 (`LICENSE.TXT`). Definir por separado la licencia del código original del proyecto antes de hacerlo público.
