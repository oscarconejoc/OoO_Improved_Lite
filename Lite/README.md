# Lite — procesador RISC-V OoO reducido

**Lite** es la variante de menor complejidad del procesador RISC-V *Out-of-Order*. Su finalidad es estudiar la reducción de recursos hardware manteniendo los mecanismos esenciales de ejecución fuera de orden. 

## Arquitectura

- **ISA:** RV32IMC implementada para las pruebas del proyecto (entero, multiplicación/división y comprimidas); no incluye soporte funcional de FPU ni RVV.
- **ROB:** 4 entradas; retiro arquitectónico en orden.
- **ALU:** 1 unidad de ejecución entera compartida por **3 estaciones de reserva**.
- **Renombramiento:** RAT para registrar productores de resultados enteros y gestionar dependencias.
- **Predicción de saltos:** política estática **not taken** (no tomado).
- **Memoria:** interfaces de memoria de instrucciones y datos definidas en `soc_top.h` y `axi_interconnect.h`.
- **Top HLS:** `riscv_soc_tick()` en `RISCV-SoC/soc_top.cpp`.

Las estaciones de reserva permiten preparar y seleccionar instrucciones cuyos operandos están disponibles; el ROB mantiene el retiro en orden. Aunque Lite conserva ejecución fuera de orden, **la capacidad de emisión hacia su ALU está limitada a una operación entera a la vez**.

