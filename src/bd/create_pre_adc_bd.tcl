# Offline candidate only. Board execution has its own reviewed startup gate.
# No target connection, ps7_init execution, bitstream or boot image generation.
set root [file normalize [file join [file dirname [info script]] ..]]
set out [file join $root build pre_adc_hardware]
set ethernet 0
if {$argc == 1 && [lindex $argv 0] eq "--ethernet"} {
 set ethernet 1
 set out [file join $root build pre_adc_network_hardware]
} elseif {$argc != 0} {error "usage: create_pre_adc_bd.tcl ?--ethernet?"}
set profile CARRIER_SD0
if {$ethernet} {append profile _ETHERNET}
foreach dir [glob -nocomplain -types d [file join $::env(XILINX_VIVADO) data XilinxTclStore support *] [file join $::env(XILINX_VIVADO) data XilinxTclStore tclapp * *]] {lappend auto_path $dir}
set rc [catch {
 set_param general.maxThreads 1
 create_project -force pre_adc_candidate [file join $out project] -part xc7z020clg484-2
 foreach source [glob [file join $root rtl *.v]] {add_files $source}
 read_ip [file join $root build fft_ip lfn_fft lfn_fft.xci]
 foreach name {pattern_sine.mem fir_stage1.mem fir_stage2.mem fir_stage3.mem hann4096.mem} {add_files [file join $root generated $name]}
 create_bd_design capture_dma
 set ps [create_bd_cell -type ip -vlnv xilinx.com:ip:processing_system7:5.5 ps7]
 set cfg [dict create CONFIG.PCW_IMPORT_BOARD_PRESET None]
 foreach p [list_property $ps] {
  set value [get_property $p $ps]
  if {[regexp {^CONFIG\.PCW_(EN_|USE_)} $p] || ([string match CONFIG.PCW_*_ENABLE $p] && $value in {0 1})} {dict set cfg $p 0}
 }
 # ALINX's pinned official geometry/frequency and compatible memory preset.
 # Applicability and fitted-chip evidence live in the workspace's sole report.
 # Metadata export never authorizes initialization or board execution.
 foreach {p v} {
  PCW_USE_CR_FABRIC 1 PCW_EN_CLK0_PORT 1 PCW_FPGA_FCLK0_ENABLE 1 PCW_EN_RST0_PORT 1
  PCW_FPGA0_PERIPHERAL_FREQMHZ 50 PCW_CRYSTAL_PERIPHERAL_FREQMHZ 33.333333
  PCW_APU_PERIPHERAL_FREQMHZ 666.666666 PCW_EN_DDR 1 PCW_UIPARAM_DDR_ENABLE 1
  PCW_UIPARAM_DDR_PARTNO {MT41J256M16 RE-125} PCW_UIPARAM_DDR_BUS_WIDTH {32 Bit}
  PCW_UIPARAM_DDR_FREQ_MHZ 533.333333 PCW_UIPARAM_DDR_USE_INTERNAL_VREF 0
  PCW_UIPARAM_DDR_TRAIN_WRITE_LEVEL 1 PCW_UIPARAM_DDR_TRAIN_READ_GATE 1 PCW_UIPARAM_DDR_TRAIN_DATA_EYE 1
  PCW_UIPARAM_DDR_BOARD_DELAY0 0.25 PCW_UIPARAM_DDR_BOARD_DELAY1 0.25 PCW_UIPARAM_DDR_BOARD_DELAY2 0.25 PCW_UIPARAM_DDR_BOARD_DELAY3 0.25
  PCW_USE_M_AXI_GP0 1 PCW_USE_S_AXI_HP0 1 PCW_USE_S_AXI_HP1 1 PCW_USE_FABRIC_INTERRUPT 1 PCW_IRQ_F2P_INTR 1
  PCW_EN_TTC0 1 PCW_TTC0_PERIPHERAL_ENABLE 1 PCW_EN_EMIO_TTC0 0 PCW_TTC0_TTC0_IO EMIO
  PCW_TTC0_CLK0_PERIPHERAL_CLKSRC CPU_1X PCW_TTC0_CLK1_PERIPHERAL_CLKSRC CPU_1X PCW_TTC0_CLK2_PERIPHERAL_CLKSRC CPU_1X
 } {dict set cfg CONFIG.$p $v}
 set_property -dict $cfg $ps
 set_property CONFIG.PCW_EN_EMIO_TTC0 0 $ps

 # Finalized board profile overlay (carrier UART1 + SD0) from the pinned
 # pure-data profile; see the sole workspace report for fact provenance.
 # Fail closed unless every documented value is accepted and read back
 # exactly by this ps7 version. Disabled selectors normalize to <Select>.
 source [file join $root bd ax7021_pre_adc_profile.tcl]
 set overlay [::lfn_pre_adc_profile::overlay [::lfn_pre_adc_profile::make carrier 1 $ethernet]]
 set_property -dict $overlay $ps
 dict for {name expected} $overlay {
  set actual [get_property $name $ps]
  if {$expected ne {Disabled} && $actual ne $expected} {error "profile readback mismatch $name: $actual != $expected"}
 }
 if {![string equal -nocase [get_property CONFIG.PCW_DDR_RAM_HIGHADDR $ps] 0x3fffffff]} {error "profile DDR geometry mismatch"}
 if {[get_property CONFIG.PCW_EN_SDIO1 $ps] ne "0"} {error "profile requires SDIO1 disabled"}
 if {$ethernet && [get_property CONFIG.PCW_ACT_ENET0_PERIPHERAL_FREQMHZ $ps] ne "125.000000"} {error "Ethernet clock must be 125 MHz"}

 
 set processing [create_bd_cell -type module -reference capture_platform_top processing]
 set_property CONFIG.FFT_CLOCK_GATE 1 $processing
 # Local absolute ROM inputs avoid changing readmemh behavior with synth run
 # directories. They remain only in ignored generated project/XSA artifacts.
 foreach {parameter filename} {SINE_FILE pattern_sine.mem FIR0_FILE fir_stage1.mem FIR1_FILE fir_stage2.mem FIR2_FILE fir_stage3.mem HANN_FILE hann4096.mem} {
  set_property CONFIG.$parameter [file join $root generated $filename] $processing
 }
 set gp [create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 control_connect]
 set_property -dict [list CONFIG.NUM_SI 1 CONFIG.NUM_MI 5] $gp
 connect_bd_intf_net [get_bd_intf_pins ps7/M_AXI_GP0] [get_bd_intf_pins control_connect/S00_AXI]
 foreach index {2 3 4} bank {cap dat ctr} {
  connect_bd_intf_net [get_bd_intf_pins control_connect/M0${index}_AXI] [get_bd_intf_pins processing/s_${bank}_axi]
 }
 foreach n {0 1} name {spectrum pcm} {
  set dma [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_dma:7.1 dma_$name]
  set dcfg [list CONFIG.c_include_sg 1 CONFIG.c_include_mm2s 0 CONFIG.c_include_s2mm 1 CONFIG.c_sg_length_width 23 CONFIG.c_m_axi_s2mm_data_width 64 CONFIG.c_s_axis_s2mm_tdata_width 64 CONFIG.c_include_s2mm_dre 0 CONFIG.c_sg_include_stscntrl_strm 0]
  set_property -dict $dcfg $dma
  foreach {p v} $dcfg {if {[get_property $p $dma] ne $v} {error "DMA readback mismatch $name $p"}}
  # The 50 MHz data/SG paths need burst conversion and arbitration, but no
  # clock conversion. Use the vendor's area strategy with all paths retained.
  set hp [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_interconnect:2.1 hp$n]
  set_property -dict [list CONFIG.NUM_SI 2 CONFIG.NUM_MI 1 CONFIG.STRATEGY 1] $hp
  connect_bd_intf_net [get_bd_intf_pins dma_$name/M_AXI_S2MM] [get_bd_intf_pins hp$n/S00_AXI]
  connect_bd_intf_net [get_bd_intf_pins dma_$name/M_AXI_SG] [get_bd_intf_pins hp$n/S01_AXI]
  connect_bd_intf_net [get_bd_intf_pins hp$n/M00_AXI] [get_bd_intf_pins ps7/S_AXI_HP$n]
  connect_bd_intf_net [get_bd_intf_pins control_connect/M0${n}_AXI] [get_bd_intf_pins dma_$name/S_AXI_LITE]
  set stream [get_bd_intf_pins processing/m_${name}_axis]
  if {[llength $stream] != 1} {error "Missing processing stream $name"}
  connect_bd_intf_net $stream [get_bd_intf_pins dma_$name/S_AXIS_S2MM]
 }
 set reset [create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 reset]
 set one [create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant:1.1 locked]
 set_property CONFIG.CONST_VAL 1 $one
 connect_bd_net [get_bd_pins locked/dout] [get_bd_pins reset/dcm_locked]
 connect_bd_net [get_bd_pins ps7/FCLK_RESET0_N] [get_bd_pins reset/ext_reset_in]
 set clocks [list ps7/M_AXI_GP0_ACLK ps7/S_AXI_HP0_ACLK ps7/S_AXI_HP1_ACLK processing/clk reset/slowest_sync_clk control_connect/aclk hp0/ACLK hp1/ACLK]
 foreach hp {hp0 hp1} {foreach p {S00_ACLK S01_ACLK M00_ACLK} {lappend clocks $hp/$p}}
 foreach name {spectrum pcm} {foreach p {s_axi_lite_aclk m_axi_sg_aclk m_axi_s2mm_aclk} {lappend clocks dma_$name/$p}}
 foreach p $clocks {connect_bd_net [get_bd_pins ps7/FCLK_CLK0] [get_bd_pins $p]}
 set resets {processing/aresetn control_connect/aresetn hp0/ARESETN hp1/ARESETN dma_spectrum/axi_resetn dma_pcm/axi_resetn}
 foreach hp {hp0 hp1} {foreach p {S00_ARESETN S01_ARESETN M00_ARESETN} {lappend resets $hp/$p}}
 foreach p $resets {connect_bd_net [get_bd_pins reset/peripheral_aresetn] [get_bd_pins $p]}
 set irqs [create_bd_cell -type ip -vlnv xilinx.com:ip:xlconcat:2.1 irqs]
 set_property CONFIG.NUM_PORTS 2 $irqs
 connect_bd_net [get_bd_pins dma_spectrum/s2mm_introut] [get_bd_pins irqs/In0]
 connect_bd_net [get_bd_pins dma_pcm/s2mm_introut] [get_bd_pins irqs/In1]
 connect_bd_net [get_bd_pins irqs/dout] [get_bd_pins ps7/IRQ_F2P]
 make_bd_intf_pins_external [get_bd_intf_pins ps7/DDR]
 make_bd_intf_pins_external [get_bd_intf_pins ps7/FIXED_IO]
 foreach name {ref_clk_in bclk_in lrck_in sdata_in mclk_out} {
  make_bd_pins_external [get_bd_pins processing/$name]
  set_property name $name [get_bd_ports ${name}_0]
 }
 # Module-reference auto assignment otherwise narrows these 12-bit slaves
 # to 1KiB in this Vivado version. Reserve the declared full 4KiB per bank.
 # Allocate software-visible PL addresses explicitly, separate from board IO.
 # No C address constants are guessed: the XSA/BSP must match this allocation.
 foreach bank {cap dat ctr} offset {0x40000000 0x40001000 0x40002000} {
  assign_bd_address -offset $offset -range 4K -target_address_space [get_bd_addr_spaces ps7/Data] [get_bd_addr_segs processing/s_${bank}_axi/reg0]
 }
 assign_bd_address
 validate_bd_design
 save_bd_design
 generate_target all [get_files capture_dma.bd]
 add_files -norecurse [make_wrapper -files [get_files capture_dma.bd] -top]
 set_property top capture_dma_wrapper [current_fileset]
 update_compile_order -fileset sources_1
 write_hw_platform -fixed -force -file [file join $out pre_adc_dma.xsa]
 set f [open [file join $out addresses.tsv] w]
 foreach s [get_bd_addr_segs] {puts $f "$s\t[get_property OFFSET $s]\t[get_property RANGE $s]"};close $f
 set f [open [file join $out ps_configuration.tsv] w]
 foreach p [lsort [list_property $ps]] {if {[string match CONFIG.* $p]} {puts $f "$p\t[get_property $p $ps]"}};close $f
 puts "PASS pre_adc_hardware_export PROFILE_${profile}_BUILD_ONLY"
} message opts]
if {$rc} {puts stderr "FAIL capture_hardware_export: $message";puts stderr [dict get $opts -errorinfo];exit 1}
close_project
exit 0
