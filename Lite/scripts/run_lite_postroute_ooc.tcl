set RTL "C:/Users/Oscar/Desktop/TFG/TFG/Lite/RISCV-SoC/LITE_OPT11_INLINE_EXPAND/hls_opt11/solution1/syn/verilog"
set OUT "C:/Users/Oscar/Desktop/TFG/TFG/Lite/RISCV-SoC/LITE_OPT11_INLINE_EXPAND/POST_ROUTE_200MHZ"

file mkdir $OUT

create_project -force opt11_route_ooc \
    "$OUT/vivado_opt11_route_ooc" \
    -part xck26-sfvc784-2LV-c

set rtl_files [glob "$RTL/*.v"]

puts ""
puts "RTL files encontrados = [llength $rtl_files]"
puts ""

add_files $rtl_files

set_property top riscv_soc_tick [current_fileset]

# ============================================================
# CLOCK = 200 MHz
# ============================================================

set xdc "$OUT/opt11_200mhz_ooc.xdc"

set fp [open $xdc w]
puts $fp {create_clock -name ap_clk -period 5.000 [get_ports ap_clk]}
close $fp

add_files -fileset constrs_1 $xdc

update_compile_order -fileset sources_1

puts ""
puts "============================================================"
puts " SYNTHESIS OOC OPT11 @ 200 MHz"
puts "============================================================"

# MUY IMPORTANTE:
# Out-of-context evita convertir los cientos de puertos del IP
# en pines fisicos del encapsulado.
synth_design \
    -top riscv_soc_tick \
    -part xck26-sfvc784-2LV-c \
    -mode out_of_context

write_checkpoint -force "$OUT/opt11_ooc_post_synth.dcp"

puts ""
puts "============================================================"
puts " OPT DESIGN"
puts "============================================================"

opt_design

puts ""
puts "============================================================"
puts " PLACE DESIGN"
puts "============================================================"

place_design

puts ""
puts "============================================================"
puts " PHYS OPT DESIGN"
puts "============================================================"

phys_opt_design

puts ""
puts "============================================================"
puts " ROUTE DESIGN"
puts "============================================================"

route_design

write_checkpoint -force "$OUT/opt11_ooc_post_route.dcp"

puts ""
puts "============================================================"
puts " REPORTS"
puts "============================================================"

report_timing_summary \
    -delay_type max \
    -max_paths 10 \
    -report_unconstrained \
    -file "$OUT/opt11_ooc_timing_summary.rpt"

report_utilization \
    -hierarchical \
    -file "$OUT/opt11_ooc_utilization.rpt"

report_power \
    -file "$OUT/opt11_ooc_power.rpt"

puts ""
puts "============================================================"
puts " OPT11 OOC POST ROUTE TERMINADO"
puts "============================================================"

exit
