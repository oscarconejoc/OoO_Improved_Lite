#ifndef SOC_CONFIG_H
#define SOC_CONFIG_H

// Parametros de la microarquitectura: latencias de las unidades
// funcionales y tamanos de las estructuras. Es el archivo que se toca para
// explorar el espacio de diseno (mas ALUs, ROB mas profundo, etc.).
#include "soc_top.h"
#include "rv32i_defs.h"


// ---- latencias de las unidades funcionales (en ticks) ----
static const int ALU_LAT   = 1;
static const int MUL_LAT   = 3;
static const int DIV_LAT   = 8;
static const int BR_LAT    = 1;

static const int ROB_SZ  = 4;
// Lite: tres estaciones de reserva alimentan UNA sola ALU fisica.
static const int N_ALU_RS = 3;

static const uint32_t UART_TX_ADDR = 0x20000000;

#endif // SOC_CONFIG_H
