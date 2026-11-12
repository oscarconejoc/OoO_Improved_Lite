cd {C:/Users/Oscar/Desktop/TFG/TFG/Improved/RISCV-SoC}

open_project -reset vector_vrat_bench_improved
set_top riscv_soc_tick

add_files soc_top.cpp
add_files -tb vector_vrat_benchmark_tb.cpp

open_solution -reset solution1 -flow_target vivado
set_part {xck26-sfvc784-2LV-c}
create_clock -period 10 -name default

csim_design

exit
