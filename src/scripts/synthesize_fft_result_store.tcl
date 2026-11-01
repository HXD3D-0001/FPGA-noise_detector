# Standalone memory/resource evidence; no hardware/timing acceptance claim.
foreach dir [glob -nocomplain -types d [file join $::env(XILINX_VIVADO) data XilinxTclStore support *] [file join $::env(XILINX_VIVADO) data XilinxTclStore tclapp * *]] {lappend auto_path $dir}
set root [file normalize [file join [file dirname [info script]] ..]]
set out [file join $root build synth_fft_result_store]
file mkdir $out
create_project -in_memory -part xc7z020clg484-2
read_verilog [file join $root rtl fft_result_store.v]
synth_design -top fft_result_store
report_utilization -file [file join $out utilization.rpt]
set ram0 [llength [get_cells -hier -filter {REF_NAME =~ RAMB* && NAME =~ *ram0*}]]
set ram1 [llength [get_cells -hier -filter {REF_NAME =~ RAMB* && NAME =~ *ram1*}]]
set ff [llength [get_cells -hier -filter {REF_NAME =~ FD*}]]
set lut [llength [get_cells -hier -filter {REF_NAME =~ LUT*}]]
puts "RESULT_STORE_RESOURCES RAM0=$ram0 RAM1=$ram1 FF=$ff LUT=$lut"
if {$ram0 < 6 || $ram1 < 6} {error "4096x48 banks did not infer expected block RAM"}
if {$ff > 2500 || $lut > 3500} {error "unexpected register/LUT footprint"}
puts "PASS fft_result_store_synthesis"
exit 0
