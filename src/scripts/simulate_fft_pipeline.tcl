set root [file normalize [file join [file dirname [info script]] ..]]
set build [file join $root build sim_fft_pipeline]
foreach dir [glob -nocomplain -types d \
 [file join $::env(XILINX_VIVADO) data XilinxTclStore support *] \
 [file join $::env(XILINX_VIVADO) data XilinxTclStore tclapp * *]] {lappend auto_path $dir}
create_project -force fft_pipeline_sim [file join $build project] -part xc7z020clg484-2
set_property target_language Verilog [current_project]
read_ip [file join $root build fft_ip lfn_fft lfn_fft.xci]
foreach source {capture_grid_divider frame_window fft_scheduler fft_result_store axis_packetizer fft_axis_pipeline} {
 add_files [file join $root rtl ${source}.v]
}
add_files -fileset sim_1 [file join $root tests rtl tb_fft_axis_pipeline.sv]
set_property top tb_fft_axis_pipeline [get_filesets sim_1]
set_property xsim.simulate.runtime all [get_filesets sim_1]
update_compile_order -fileset sources_1
# The initial blocks read data at time zero, so stage it before launch.
set run_dir [file join $build project fft_pipeline_sim.sim sim_1 behav xsim]
file mkdir $run_dir
file copy -force [file join $build fft_samples.mem] [file join $run_dir fft_samples.mem]
file mkdir [file join $run_dir generated]
file copy -force [file join $root generated hann4096.mem] [file join $run_dir generated hann4096.mem]
launch_simulation
foreach name {fft_packets.txt fft_timing.txt fft_window.txt simulate.log} {
 file copy -force [file join $run_dir $name] [file join $build $name]
}
puts "PASS fft_pipeline_simulation_launcher"
close_sim
close_project
exit 0
