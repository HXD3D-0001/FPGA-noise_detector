# Reproducible generated IP stays under ignored build/fft_ip.
set root [file normalize [file join [file dirname [info script]] ..]]
set out [file join $root build fft_ip]
file mkdir $out
foreach dir [glob -nocomplain -types d \
 [file join $::env(XILINX_VIVADO) data XilinxTclStore support *] \
 [file join $::env(XILINX_VIVADO) data XilinxTclStore tclapp * *]] {lappend auto_path $dir}
create_project -in_memory -part xc7z020clg484-2
set_property target_language Verilog [current_project]
if {[version -short] ne "2026.1"} {error "FFT evidence requires Vivado 2026.1"}
set xci [file join $out lfn_fft lfn_fft.xci]
if {[file exists $xci]} {read_ip $xci} else {
 create_ip -name xfft -vendor xilinx.com -library ip -version 9.1 -module_name lfn_fft -dir $out
}
set_property -dict [list \
 CONFIG.channels {1} CONFIG.transform_length {4096} \
 CONFIG.implementation_options {radix_4_burst_io} \
 CONFIG.data_format {fixed_point} CONFIG.input_width {24} \
 CONFIG.phase_factor_width {24} CONFIG.scaling_options {block_floating_point} \
 CONFIG.rounding_modes {convergent_rounding} CONFIG.output_ordering {natural_order} \
 CONFIG.throttle_scheme {nonrealtime} CONFIG.xk_index {true} \
 CONFIG.aresetn {true} CONFIG.ovflo {false} \
 CONFIG.run_time_configurable_transform_length {false} \
 CONFIG.target_clock_frequency {50}] [get_ips lfn_fft]
generate_target all [get_ips lfn_fft]
puts "PASS fft_ip_generation"
close_project
exit 0
