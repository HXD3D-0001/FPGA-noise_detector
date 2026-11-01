# Platform-scoped internal CDC constraints for the pre-ADC implementation.
# Reuses the reviewed frontend exception set (audio_cdc_internal.tcl) against
# the capture platform hierarchy inside the BD. No CDC waivers, no blanket
# false paths; exact endpoint counts fail closed. The board XDC must already
# define adc_bclk. Impl-only (used_in_synthesis false) so cached synthesis
# results stay valid.
# NOTE: under the project run flow [info script] is relative and the working
# directory is the run directory, so locate the library by walking up.
proc lfn_cdc_find_up {name} {
 set d [file normalize [pwd]]
 for {set i 0} {$i < 12} {incr i} {
  set p [file join $d constraints $name]
  if {[file exists $p]} {return $p}
  set parent [file dirname $d]
  if {$parent eq $d} {break}
  set d $parent
 }
 error "cannot locate $name from [pwd]"
}
source [lfn_cdc_find_up audio_cdc_internal.tcl]
lfn_cdc::apply {capture_dma_i/processing/inst/audio/capture/frontend/} {} {capture_dma_i/processing/inst/audio/capture/saved_channel}
# External asynchronous reset assertion: the BD proc_sys_reset replica
# asynchronously asserts reset into the audio/frontend pipelines; every
# consumer re-synchronizes the release through registered pipes (frontend
# proc_reset_pipe/bck_reset_pipe, clocks state release pipe). Cut only the
# assert into asynchronous control pins; keep all synchronous timing intact.
set reset_flops [get_cells -quiet -hier -regexp {^capture_dma_i/reset/U0/ACTIVE_LOW_PR_OUT_DFF.*}]
if {![llength $reset_flops]} {error "platform CDC: reset source flops not found"}
set async_dst [get_pins -quiet -hier -regexp {^capture_dma_i/processing/inst/audio/.*_reg(\[[0-9]+\])?/(PRE|CLR)$}]
if {![llength $async_dst]} {error "platform CDC: no audio async control pins found"}
set_false_path -from [get_pins -quiet -of_objects $reset_flops -filter {REF_PIN_NAME == C}] -to $async_dst
