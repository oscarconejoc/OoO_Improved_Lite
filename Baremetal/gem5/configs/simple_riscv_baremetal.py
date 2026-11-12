import argparse
import os

import m5
from m5.objects import *

parser = argparse.ArgumentParser(description="Run a bare-metal RISC-V ELF in gem5")
parser.add_argument("--binary", required=True, help="Path to the compiled ELF binary")
parser.add_argument("--maxinsts", type=int, default=1_000_000, help="Maximum instructions to simulate")
args = parser.parse_args()

system = System()
system.clk_domain = SrcClockDomain(clock='1GHz', voltage_domain=VoltageDomain())
system.mem_mode = 'atomic'
system.mem_ranges = [AddrRange('512MB')]

cpu_cls = getattr(m5.objects, 'AtomicSimpleCPU', None)
if cpu_cls is None:
    cpu_cls = getattr(m5.objects, 'RiscvTimingSimpleCPU', None)
if cpu_cls is None:
    cpu_cls = getattr(m5.objects, 'RiscvO3CPU', None)
if cpu_cls is None:
    raise RuntimeError('No supported RISC-V CPU class was found in this gem5 build')

system.cpu = cpu_cls()

# Fuerza RV32 para ejecutar el ELF compilado con ilp32/rv32.
if hasattr(system.cpu, 'ArchISA') and hasattr(system.cpu.ArchISA, 'riscv_type'):
    system.cpu.ArchISA.riscv_type = 'RV32'

if hasattr(system.cpu, 'isa'):
    for isa in system.cpu.isa:
        if hasattr(isa, 'riscv_type'):
            isa.riscv_type = 'RV32'

system.membus = SystemXBar()
system.cpu.icache_port = system.membus.cpu_side_ports
system.cpu.dcache_port = system.membus.cpu_side_ports
system.system_port = system.membus.cpu_side_ports

system.mem_ctrl = MemCtrl()
system.mem_ctrl.dram = DDR3_1600_8x8()
system.mem_ctrl.dram.range = system.mem_ranges[0]
system.mem_ctrl.port = system.membus.mem_side_ports

system.cpu.createInterruptController()
process = Process(
    cmd=[args.binary],
    cwd=os.getcwd(),
)
system.cpu.workload = process
system.cpu.createThreads()
system.workload = SEWorkload.init_compatible(args.binary)
system.cpu.max_insts_any_thread = args.maxinsts

root = Root(full_system=False, system=system)
m5.instantiate()
print(f'Running bare-metal binary: {args.binary}')
exit_event = m5.simulate()
print('Simulation completed with cause:', exit_event.getCause())
