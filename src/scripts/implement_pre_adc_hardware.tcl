# P6 pre-ADC full implementation: synth -> place -> route -> bitstream from
# the profile-variant BD produced by the last export_pre_adc_hardware run.
# Gates per the workspace report fixed constraints: WNS>=0, TNS=0, WHS>=0,
# THS=0, TPWS=0, no NSTD/UCIO DRC violations. No gate may be relaxed;
# failures exit non-zero with all reports saved for diagnosis.
# vivado -mode batch -source scripts/implement_pre_adc_hardware.tcl
set root [file normalize [file join [file dirname [info script]] ..]]
set out [file join $root build pre_adc_implementation]
file mkdir $out
foreach dir [glob -nocomplain -types d [file join $::env(XILINX_VIVADO) data XilinxTclStore support *] [file join $::env(XILINX_VIVADO) data XilinxTclStore tclapp * *]] {lappend auto_path $dir}
set rc [catch {
 set_param general.maxThreads 1
 open_project [file join $root build pre_adc_hardware project pre_adc_candidate.xpr]
 set xdc [file join $root constraints ax7021_audio_io_pre_adc.xdc]
 if {![file exists $xdc]} {error "missing board XDC"}
 add_files -fileset constrs_1 -norecurse $xdc
 set cdc_tcl [file join $root constraints audio_cdc_platform_pre_adc.tcl]
 if {![file exists $cdc_tcl]} {error "missing platform CDC constraints"}
 add_files -fileset constrs_1 -norecurse $cdc_tcl
 set_property used_in_synthesis false [get_files $cdc_tcl]
 set_property used_in_implementation true [get_files $cdc_tcl]
 set_property STEPS.PHYS_OPT_DESIGN.IS_ENABLED true [get_runs impl_1]
 set_property top capture_dma_wrapper [current_fileset]
 update_compile_order -fileset sources_1
 reset_run synth_1
 launch_runs impl_1 -to_step write_bitstream -jobs 1
 wait_on_run impl_1
 set status [get_property STATUS [get_runs impl_1]]
 set progress [get_property PROGRESS [get_runs impl_1]]
 if {$progress ne "100%" || ![string match "*Complete*" $status] || [string match "*ERROR*" $status]} {
   error "implementation run did not complete: $status"
 }
 open_run impl_1
 # Fail closed when the platform CDC exceptions are absent: sourcing errors
 # inside constraint files degrade to CRITICAL WARNINGs that do not fail the
 # run, so probe a representative crossing for its applied requirement.
 set probe_cells [get_cells -quiet -hier -regexp {^capture_dma_i/processing/inst/audio/capture/frontend/controller/active_capture_id_reg\[[0-9]+\]$}]
 if {![llength $probe_cells]} {error "CDC probe: frontend cells not found"}
 set probe_dst [get_cells -quiet -hier -regexp {^capture_dma_i/processing/inst/audio/capture/frontend/cdc/source_capture_id_reg\[[0-9]+\]$}]
 if {![llength $probe_dst]} {error "CDC probe: cdc destination cells not found"}
 set probe [get_timing_paths -quiet -from $probe_cells -to $probe_dst -max_paths 1]
 if {![llength $probe]} {error "CDC probe: no crossing path found"}
 set req [get_property REQUIREMENT $probe]
 if {$req < 19.9 || $req > 20.1} {error "CDC constraints not applied (capture id path requirement $req ns, expected the 20 ns max-delay budget)"}
 report_timing_summary -file [file join $out timing_summary.rpt] -warn_on_violation
 report_cdc -file [file join $out cdc.rpt]
 report_drc -file [file join $out drc.rpt]
 report_utilization -file [file join $out utilization.rpt]
 # Design Timing Summary row: WNS TNS TNSF TNST WHS THS THSF THST WPWS TPWS WPWSF TPWST
 set f [open [file join $out timing_summary.rpt] r]; set text [read $f]; close $f
 set parsed 0
 foreach line [split $text "\n"] {
   if {[regexp {^\s*(-?[0-9]+\.[0-9]+)\s+(-?[0-9]+\.[0-9]+)\s+([0-9]+)\s+([0-9]+)\s+(-?[0-9]+\.[0-9]+)\s+(-?[0-9]+\.[0-9]+)\s+([0-9]+)\s+([0-9]+)\s+(-?[0-9]+\.[0-9]+)\s+(-?[0-9]+\.[0-9]+)\s+([0-9]+)\s+([0-9]+)\s*$} $line -> wns tns tnsf tnst whs ths thsf thst wpws tpws wpwsf tpwst]} {
     set parsed 1; break
   }
 }
 if {!$parsed} {error "could not parse design timing summary numbers"}
 if {$wpwsf > 0} {error "pulse width violations: WPWS $wpws failing $wpwsf"}
 set f [open [file join $out drc.rpt] r]; set drc [read $f]; close $f
 foreach rule {NSTD-1 UCIO-1} {
   if {[regexp "(?m)^$rule" $drc]} {error "DRC $rule violation present (unconstrained IO)"}
 }
 set run_dir [get_property DIRECTORY [get_runs impl_1]]
 set bit [lindex [glob -nocomplain [file join $run_dir *.bit]] 0]
 if {![file exists $bit]} {error "no bitstream produced"}
 file copy -force $bit [file join $out pre_adc_impl.bit]
 set dcp [file join $run_dir capture_dma_wrapper_routed.dcp]
 if {![file exists $dcp]} {error "no routed checkpoint"}
 file copy -force $dcp [file join $out pre_adc_impl_routed.dcp]
 write_hw_platform -fixed -include_bit -force -file [file join $out pre_adc_dma_impl.xsa]
 if {$wns < 0 || $tns < 0 || $whs < 0 || $ths < 0} {error "timing gates violated: WNS $wns TNS $tns WHS $whs THS $ths"}
 set f [open [file join $out gates.json] w]
 puts $f "{\"WNS\": $wns, \"TNS\": $tns, \"WHS\": $whs, \"THS\": $ths, \"WPWS\": $wpws, \"TPWS\": $tpws,"
 puts $f " \"pulse_width_clean\": [expr {$wpwsf == 0}], \"drc_nstd_ucio_clean\": true, \"bit\": \"[file tail $bit]\"}"
 close $f
 puts "PASS pre_adc_implementation WNS=$wns TNS=$tns WHS=$whs THS=$ths WPWS=$wpws"
} message opts]
if {$rc} {puts stderr "FAIL pre_adc_implementation: $message";puts stderr [dict get $opts -errorinfo];exit 1}
close_project
exit 0
