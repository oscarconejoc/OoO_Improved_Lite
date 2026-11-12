set ROOT "C:/Users/Oscar/Desktop/TFG/TFG/Lite/RISCV-SoC/LITE_OPT11_INLINE_EXPAND"

cd $ROOT

open_project -reset "$ROOT/hls_opt11"

set_top riscv_soc_tick
add_files "$ROOT/soc_top.cpp"

open_solution -reset solution1

set_part {xck26-sfvc784-2LV-c}

create_clock -period 5.000 -name default
set_clock_uncertainty 1.0

puts ""
puts "============================================================"
puts " OPT11 INLINE EXPAND CSYNTH"
puts "============================================================"

csynth_design

puts ""
puts "============================================================"
puts " OPT11 CSYNTH TERMINADO"
puts "============================================================"

exit
