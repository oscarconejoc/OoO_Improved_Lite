#!/usr/bin/env python3
"""
Para inspeccionar el binario bare-metal y extraer información de símbolos.
Ayuda a entender dónde se almacenan los resultados en memoria.
"""

import subprocess
import sys


def dump_binary_symbols(binary_path):
    """Extract symbol information from the ELF binary."""
    try:
        result = subprocess.run(
            ['readelf', '-s', binary_path],
            capture_output=True,
            text=True
        )
        print("Symbol Table:")
        print(result.stdout)
    except FileNotFoundError:
        print("readelf not found. Install binutils for RISC-V:")
        print("  sudo apt-get install binutils-riscv64-unknown-elf")


def dump_binary_sections(binary_path):
    """Display ELF section headers."""
    try:
        result = subprocess.run(
            ['readelf', '-S', binary_path],
            capture_output=True,
            text=True
        )
        print("\nSection Headers:")
        print(result.stdout)
    except FileNotFoundError:
        pass


def dump_results_section(binary_path):
    """Try to extract the .results section (if embedded)."""
    try:
        result = subprocess.run(
            ['objdump', '-s', '-j', '.results', binary_path],
            capture_output=True,
            text=True
        )
        print("\n.results Section Content:")
        print(result.stdout)
    except FileNotFoundError:
        print("objdump not found. Install binutils for RISC-V.")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 inspect_binary.py <binary_path>")
        sys.exit(1)
    
    binary = sys.argv[1]
    print(f"Inspecting: {binary}\n")
    
    dump_binary_symbols(binary)
    dump_binary_sections(binary)
    dump_results_section(binary)
