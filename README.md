# RISC-V OoO en FPGA: Original, Lite e Improved

Repositorio de continuidad de un Trabajo Final de Graduación en Ingeniería Electrónica del Tecnológico de Costa Rica. El proyecto estudia el comportamiento **microarquitectónico** y la implementación mediante **Vitis HLS / Vivado** de un procesador RISC-V de ejecución fuera de orden (*Out-of-Order*, OoO).

Se parte de una arquitectura **Original** y se desarrollan dos variantes: **Lite**, orientada a reducir recursos, e **Improved**, orientada a aumentar el paralelismo interno. **MicroBlaze V** se utilizó como referencia de implementación para algunas comparaciones.

> **Alcance:** este repositorio organiza código fuente, pruebas y evidencia reproducible. No es una implementación de SoC lista para producción ni implica que las métricas de simulación RTL hayan sido medidas sobre una FPGA física.

## Arquitecturas

| Diseño | Objetivo | Características destacadas |
|---|---|---|
| **Original** | Punto de comparación | Núcleo RISC-V OoO con renombramiento, ROB, estaciones de reserva, TAGE y unidades especializadas. |
| **Lite** | Menor complejidad y área | ROB de 4 entradas, una ALU compartida por tres estaciones de reserva, predictor estático de no tomado, sin FPU ni RVV. Implementación final derivada de OPT11. |
| **Improved** | Mayor paralelismo de ejecución | ROB de 8 entradas, dos ALU, pipelines de multiplicación y FPU, renombramiento vectorial (VRAT) y rutas vectoriales más concurrentes. |
| **MicroBlaze V** | Referencia comercial | Procesador RISC-V de AMD utilizado en pruebas comparativas; su configuración se documenta por separado. |

El código de Lite e Improved se encuentra explicado en sus respectivos documentos:

- [Lite/README.md](Lite/README.md)
- [Improved/README.md](Improved/README.md)

## Organización recomendada

```text
RISC-V-OoO-TFG/
├── README.md
├── Lite/
│   ├── README.md
│   ├── RISCV-SoC/         # código HLS y pruebas esenciales
│   ├── scripts/           # TCL de simulación, síntesis e implementación
│   ├── results/           # resultados y reportes seleccionados
│   └── third_party/       # dependencias con su licencia
├── Improved/
│   ├── README.md
│   ├── RISCV-SoC/         # código HLS y pruebas esenciales
│   ├── scripts/
│   ├── benchmarks/        # programas e insumos adicionales, si se incluyen
│   ├── results/
│   └── third_party/
├── Original/              # incorporar la versión de referencia depurada
└── MicroBlaze-V/          # documentación/scripts, si se publican
```

Los directorios `Original/` y `MicroBlaze-V/` son **opcionales en esta etapa**: incluirlos solamente cuando su contenido esté depurado y se hayan comprobado los permisos de distribución. La estructura de `scripts/` y `results/` corresponde a la organización objetivo; antes de publicar hay que verificar que coincida con los archivos finalmente copiados.

## Entorno de desarrollo

- **Vitis HLS / Vivado 2024.1**, para síntesis y flujo FPGA.
- FPGA objetivo de los proyectos: **`xck26-sfvc784-2LV-c`**.
- **C++14 y `g++`** para las pruebas funcionales que pueden ejecutarse fuera de Vitis.
- Encabezados `ap_int` disponibles mediante `third_party/ap_types/` para esas pruebas locales; conservar siempre el archivo `LICENSE.TXT` correspondiente.

Las frecuencias objetivo difieren entre variantes: **Lite: 200 MHz** e **Improved: 100 MHz**. Una frecuencia establecida en HLS o una estimación de timing no garantiza, por sí sola, cierre de temporización en una implementación completa sobre FPGA.

## Resultados representativos

### Improved frente a Original: modelo microarquitectónico

| Benchmark | Original (ticks) | Improved (ticks) | Reducción |
|---|---:|---:|---:|
| Bare-metal AXPY + GEMM (comparativa previa) | 82 452 | 76 295 | 7,47 % |
| VECTOR64 | 398 | 271 | 31,91 % |
| MIXED32 | 983 | 407 | 58,60 % |

Son **ticks del modelo**, no ciclos del reloj físico `ap_clk`.

### Implementación RTL y limitación observada

Para el bare-metal ejecutado con **Improved FLWFIX** en XSIM a 100 MHz se registraron **76 383 ticks**, **44 377 instrucciones retiradas**, **14 832 644 ciclos `ap_clk`** y **PASS** funcional (AXPY y GEMM). Esto equivale a **194,19 ciclos `ap_clk` por tick** y **148,32644 ms** de tiempo de ejecución simulado.

El reporte HLS muestra que `riscv_soc_tick()` no se implementa como un pipeline global y presenta una latencia estimada de **138 a 1509 ciclos** por llamada. Por lo tanto, **un tick microarquitectónico no equivale a un clock `ap_clk`**. La calendarización HLS de la función explica una parte fundamental de la diferencia entre el IPC del modelo y el IPC respecto al reloj RTL.

**Importante:** el benchmark bare-metal local embebido en CSim de Improved (76 295 ticks) y la prueba RTL FLWFIX con ELF común externo (76 383 ticks) utilizan **imágenes ELF distintas**. No se deben comparar sus ticks como si fueran la misma ejecución. Asimismo, las mediciones RTL históricas de VECTOR64 y MIXED32 se obtuvieron con una síntesis anterior a FLWFIX; deben revalidarse antes de presentarlas como resultados de una única versión final.

En Lite, la optimización **OPT11** registró una reducción de **30 206 a 27 789 ciclos RTL** (aproximadamente **8,00 %**) en su prueba de control específica, a 200 MHz. No es directamente una cifra de los tres benchmarks de Improved.

## Cómo continuar el trabajo

1. Conservar una única versión funcional de cada arquitectura y mantener los experimentos históricos en respaldos externos, no en el repositorio principal.
2. Hacer portables los scripts TCL: eliminar rutas absolutas, documentar el FPGA objetivo y generar proyectos de HLS/Vivado en directorios ignorados por Git.
3. Incorporar los testbenches y scripts XSIM externos necesarios para reproducir las mediciones `ap_clk`, indicando sus límites de medición, versiones RTL y hashes de ELF.
4. Revalidar VECTOR64 y MIXED32 con el RTL FLWFIX y comparar las variantes bajo una metodología de medición consistente.
5. Investigar una implementación con etapas RTL explícitas o una reestructuración HLS con mayor concurrencia. LEAF/LEAFs puede evaluarse como línea metodológica futura, **sin asumir compatibilidad directa** con esta implementación.

## Publicación y licencias

No incluir proyectos generados de HLS, cachés `.Xil`, checkpoints `.dcp`, ejecutables, grandes logs ni archivos con rutas personales. Los componentes de terceros y las IP de AMD pueden tener condiciones de redistribución propias: revisar sus licencias antes de publicarlos. **Definir la licencia del código propio** en un archivo `LICENSE` antes de abrir el repositorio al público; la licencia Apache 2.0 incluida en `third_party/ap_types/` se refiere a esa dependencia, no automáticamente al proyecto completo.
