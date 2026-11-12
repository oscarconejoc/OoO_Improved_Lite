# Gem5 integration scaffold

This folder contains a more realistic scaffold for running the bare-metal RISC-V binary with gem5.

## Prerequisites

- gem5 installed and available in your PATH or under a local build directory
- a gem5 build with RISC-V support

## Usage

From the project root:

```bash
./gem5/run_gem5.sh
```

You can override the target binary and gem5 binary if needed:

```bash
TARGET_BIN=/path/to/your/binary GEM5_BIN=/path/to/gem5 ./gem5/run_gem5.sh
```

The script uses a RISC-V configuration file at [gem5/configs/riscv_baremetal.py](gem5/configs/riscv_baremetal.py) and passes the ELF as a process workload to gem5.
