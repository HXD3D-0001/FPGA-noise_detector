# Internal CDC timing only. Source after clocks exist on a synthesized design.
# No board IO delays, clock groups, or CDC waivers. Exact endpoints fail closed.
namespace eval lfn_cdc {variable buses {}; variable chains {}; variable cuts {}}
proc lfn_cdc::cells {re expected} {
 set c [get_cells -quiet -hier -regexp $re]
 if {$expected >= 0 && [llength $c] != $expected} {error "CDC selector mismatch: $re expected $expected found [llength $c]"}
 if {![llength $c]} {error "Empty CDC selector: $re"}
 return $c
}
proc lfn_cdc::escape {s} {
 set out ""
 foreach ch [split $s ""] {
  if {[string first $ch {\.[]*+?^$()|}]>=0} {append out [format %c 92]}
  append out $ch
 }
 return $out
}
proc lfn_cdc::pins {c pin} {
 set p [get_pins -quiet -of_objects $c -filter "REF_PIN_NAME == $pin"]
 if {[llength $p] != [llength $c]} {error "Missing $pin on CDC registers"}
 return $p
}
proc lfn_cdc::period {c} {
 set clocks [get_clocks -quiet -of_objects [pins $c C]]
 if {[llength $clocks]!=1} {error "Expected one clock on $c, got $clocks"}
 return [get_property PERIOD $clocks]
}
proc lfn_cdc::chain {label first second} {
 variable chains
 foreach c [concat $first $second] {
  if {![get_property ASYNC_REG $c]} {error "Missing ASYNC_REG: $c"}
 }
 lappend chains [list $label $first [pins $second D]]
}
proc lfn_cdc::single {label src first second} {
 variable cuts
 chain $label $first $second
 set dst [pins $first D]
 set drivers [get_pins -quiet -leaf -of_objects [get_nets -quiet -of_objects $dst] -filter {DIRECTION == OUT}]
 if {[llength $drivers]!=1 || [get_cells -quiet -of_objects $drivers] ne $src} {error "Unexpected logic/driver on synchronizer entry $label"}
 set_false_path -from $src -to $dst
 lappend cuts [list $label $src $dst]
}
proc lfn_cdc::bus {label src dst} {
 variable buses
 if {[llength $src]!=[llength $dst]} {error "CDC width mismatch $label"}
 # Conservative bound: <= one period of the faster clock, including handshake
 # data. This is stricter than the vendor Gray source-period max-delay bound.
 set budget [expr {min([period $src],[period $dst])}]
 set dp [pins $dst D]
 set_max_delay -datapath_only $budget -from $src -to $dp
 set_bus_skew $budget -from $src -to $dp
 lappend buses [list $label $src $dp $budget]
}
proc lfn_cdc::regs {stem {index scalar} {count 1}} {
 set e [escape $stem]
 switch $index {
  scalar {set re [format {^%s_reg$} $e]}
  vector {set re [format {^%s_reg\[[0-9]+\]$} $e]}
  first_vector {set re [format {^%s_reg\[0\]\[[0-9]+\]$} $e]}
  second_vector {set re [format {^%s_reg\[1\]\[[0-9]+\]$} $e]}
  default {set re [format {^%s_reg\[%s\]$} $e $index]}
 }
 return [cells $re $count]
}
proc lfn_cdc::apply {prefix {clock_prefix ""} {channel_source ""}} {
 variable buses {}; variable chains {}; variable cuts {}
 foreach {g m s} {bck_gray bck_meta bck_sync slot_gray slot_meta slot_sync physical_gray physical_meta physical_sync format_gray format_meta format_sync} {
  set src [regs ${prefix}$g vector 64]
  set dst [regs ${prefix}$m vector 64]
  bus $g $src $dst
  chain $g $dst [regs ${prefix}$s vector 64]
 }
 foreach {name launch} {req_sync controller/source_req_toggle enable_sync cdc/enable_proc clear_sync cdc/source_clear ack_valid_sync cdc/ack_source_valid ack_toggle_sync cdc/ack_source_toggle} {
  set source [get_cells -quiet -hier -regexp [format {^%s_reg$} [escape ${prefix}$launch]]]
  if {$name eq "clear_sync" && ![llength $source]} {
   # loaded and source_clear have identical reset/busy/set behavior and may merge.
   set source [regs ${prefix}cdc/loaded]
  }
  if {[llength $source]!=1} {error "Unexpected single-bit source: $name"}
  single $name $source [regs ${prefix}cdc/$name 0] [regs ${prefix}cdc/$name 1]
 }
 bus capture_request [regs ${prefix}controller/active_capture_id vector 64] [regs ${prefix}cdc/source_capture_id vector 64]
 bus capture_ack [regs ${prefix}cdc/source_capture_id vector 64] [regs ${prefix}cdc/ack_capture_proc vector 64]
 # Installed XPM Tcl timing intent, without vendor/custom CDC waivers.
 foreach side {rd wr} {
  foreach suffix {"" _dc} {
   set stem "${prefix}cdc/fifo/gnuram_async_fifo.xpm_fifo_base_inst/gen_cdc_pntr.${side}_pntr_cdc${suffix}_inst/"
   set src [regs ${stem}src_gray_ff vector -1]
   set dst [regs ${stem}dest_graysync_ff first_vector [llength $src]]
   bus xpm_${side}${suffix} $src $dst
   chain xpm_${side}${suffix} $dst [regs ${stem}dest_graysync_ff second_vector [llength $src]]
  }
 }
 foreach {stem launch} {rrst_wr fifo_rd_rst_ic wrst_rd fifo_wr_rst_ic} {
  set r "${prefix}cdc/fifo/gnuram_async_fifo.xpm_fifo_base_inst/xpm_fifo_rst_inst/gen_rst_ic."
  single xpm_$stem [regs ${r}$launch] [regs ${r}${stem}_inst/syncstages_ff 0] [regs ${r}${stem}_inst/syncstages_ff 1]
 }
 set launch [regs ${prefix}cdc/reset_request]
 set release [regs ${prefix}cdc/source_reset_pipe vector 3]
 set dst [pins $release PRE]
 set_false_path -from $launch -to $dst
 lappend cuts [list source_reset_assert $launch $dst]
 for {set i 0} {$i<2} {incr i} {
  chain source_reset_$i [regs ${prefix}cdc/source_reset_pipe $i] [regs ${prefix}cdc/source_reset_pipe [expr {$i+1}]]
 }
 # The physical FIFO reset coalescer has a separate boot-only synchronizer.
 # Cut only its asynchronous assertion pins; keep both release stages timed.
 set launch [regs ${prefix}cdc/boot_reset_request]
 set release [regs ${prefix}cdc/boot_reset_pipe vector 3]
 set dst [pins $release PRE]
 set_false_path -from $launch -to $dst
 lappend cuts [list boot_reset_assert $launch $dst]
 for {set i 0} {$i<2} {incr i} {
  chain boot_reset_$i [regs ${prefix}cdc/boot_reset_pipe $i] [regs ${prefix}cdc/boot_reset_pipe [expr {$i+1}]]
 }
 if {$channel_source ne ""} {
  single adc_channel [regs $channel_source] [regs ${prefix}channel_sync 0] [regs ${prefix}channel_sync 1]
 } else {
  chain adc_channel [regs ${prefix}channel_sync 0] [regs ${prefix}channel_sync 1]
 }
 if {$clock_prefix ne ""} {
  foreach {n launch} {restart_sync restart_toggle applied_sync applied_toggle} {
   single clock_$n [regs ${clock_prefix}$launch] [regs ${clock_prefix}$n 0] [regs ${clock_prefix}$n 1]
  }
 }
}
proc lfn_cdc::audit {out} {
 variable buses;variable chains;variable cuts
 set f [open [file join $out cdc_constraint_audit.txt] w]
 foreach b $buses {
  lassign $b label src dst budget
  set paths [get_timing_paths -quiet -from $src -to $dst -max_paths [llength $dst]]
  if {[llength $paths]!=[llength $dst]} {error "Masked/missing CDC data path $label"}
  foreach path $paths {
   if {![string is double -strict [get_property REQUIREMENT $path]]} {error "Masked/missing CDC data path $label"}
   if {abs([get_property REQUIREMENT $path]-$budget)>0.001} {error "CDC bound overridden $label"}
   if {[get_property SLACK $path]<0} {error "CDC bound not met $label"}
  }
  puts $f "BUS $label width=[llength $dst] max_delay_ns=$budget skew_ns=$budget PASS"
 }
 foreach ch $chains {
  lassign $ch label first second
  set paths [get_timing_paths -quiet -from $first -to $second -max_paths [llength $second]]
  if {[llength $paths]!=[llength $second]} {error "Synchronizer stage timing cut: $label"}
  set expected [period $first]
  foreach path $paths {
   set req [get_property REQUIREMENT $path]
   if {![string is double -strict $req]} {error "Synchronizer stage timing cut: $label"}
   if {abs($req-$expected)>0.001} {error "Synchronizer stage timing cut: $label"}
   if {[get_property SLACK $path]<0} {error "Synchronizer stage timing failed: $label"}
  }
  puts $f "CHAIN $label stages_1_to_2_timed=[llength $paths] PASS"
 }
 foreach cut $cuts {lassign $cut label src dst;puts $f "ASYNC_ENTRY $label from=$src to=$dst"}
 close $f
 report_exceptions -coverage -file [file join $out exceptions_coverage.rpt]
 report_bus_skew -file [file join $out bus_skew.rpt]
 set f [open [file join $out bus_skew.rpt] r];set skew [read $f];close $f
 set skew_count 0
 foreach line [split $skew \n] {
  if {[regexp {^[ \t]+(Slow|Fast)[ \t]+(-?[0-9]+\.[0-9]+)[ \t]+(-?[0-9]+\.[0-9]+)[ \t]+(-?[0-9]+\.[0-9]+)[ \t]*$} $line -> corner req actual slack]} {
   if {$slack<0} {error "Bus skew bound failed"}
   incr skew_count
  }
 }
 if {$skew_count!=[llength $buses]} {error "Bus skew report incomplete: $skew_count of [llength $buses]"}
 # Warnings are visible and classified, never converted into waivers.
 report_cdc -details -file [file join $out cdc.rpt]
 set f [open [file join $out cdc.rpt] r];set text [read $f];close $f
 if {[regexp {CDC-[0-9]+[ \t]+Critical} $text]} {error "Critical CDC remains"}
 foreach line [split $text \n] {
  if {[regexp {^CDC-([0-9]+)[ \t]+Warning[ \t]+([0-9]+)} $line -> rule count]} {
   if {($rule==6 && $count==8)||($rule==15 && $count==128)} {continue}
   error "Unreviewed CDC warning: $line"
  }
 }
 report_timing_summary -file [file join $out timing.rpt]
 set f [open [file join $out timing.rpt] r];set summary [read $f];close $f
 foreach check {no_clock unconstrained_internal_endpoints multiple_clock loops latch_loops} {
  set pattern [format {checking %s \(([0-9]+)\)} $check]
  if {![regexp $pattern $summary -> count]} {error "Missing timing coverage check: $check"}
  if {$count!=0} {error "Uncovered internal timing: $check=$count"}
 }
 foreach type {max min} {
  set worst [get_timing_paths -quiet -delay_type $type -max_paths 1]
  if {![llength $worst]||[get_property SLACK $worst]<0} {error "Internal OOC $type timing failed"}
 }
 puts "PASS internal_cdc_timing_audit"
}
