set ROOT "C:/Users/Oscar/Desktop/TFG/TFG/Improved/RISCV-SoC"

cd $ROOT

open_project -reset "$ROOT/improved_final_csynth_100mhz_flwfix"

set_top riscv_soc_tick

add_files "$ROOT/soc_top.cpp"

open_solution -reset solution1

set_part {xck26-sfvc784-2LV-c}

create_clock -period 10 -name default

csynth_design

exit
