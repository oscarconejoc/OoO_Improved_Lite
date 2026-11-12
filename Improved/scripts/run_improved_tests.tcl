set ROOT "C:/Users/Oscar/Desktop/TFG/TFG/Improved/RISCV-SoC"

cd $ROOT

open_project -reset "$ROOT/improved_baseline_csim"

set_top riscv_soc_tick

add_files "$ROOT/soc_top.cpp"

add_files -tb "$ROOT/soc_tb.cpp"
add_files -tb "$ROOT/trap_elf.h"
add_files -tb "$ROOT/full_elf.h"
add_files -tb "$ROOT/printf_elf.h"

open_solution -reset solution1

set_part {xck26-sfvc784-2LV-c}
create_clock -period 5 -name default

csim_design

exit
