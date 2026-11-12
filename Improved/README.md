# Improved — procesador RISC-V OoO con mayor paralelismo

**Improved** es la variante del procesador RISC-V *Out-of-Order* desarrollada para incrementar el solapamiento de operaciones independientes frente a la arquitectura Original. Mantiene el modelo de renombramiento, estaciones de reserva y retiro en orden, e introduce pipelines funcionales y mejoras en el procesamiento vectorial.

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

