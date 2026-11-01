# Pre-ADC board constraints built ONLY from schematic-verified facts
# (provenance in the sole workspace report, P3 offline verification):
#   Y9 = IO_L12P_T1_MRCC_13, core schematic PAGE04 "PL_GCLK ... 50Mhz".
#   J16 audio route (base PAGE13, all through 33R packs):
#     MCLK J16-3 -> V12 = IO_L4P_T0_13;  BCLK J16-33 -> AA9 = IO_L11P_T1_SRCC_13;
#     LRCK J16-4 -> W12 = IO_L4N_T0_13; DATA J16-5 -> U12 = IO_L5P_T0_13.
#   Core PAGE04 states "3.3V BANK13 VCCIO" -> LVCMOS33. Occupancy verified:
#   these four balls carry no other base-board function.
# PRE-WIRING STATUS: the board/cable skew of LRCK/DATA relative to BCLK is
# NOT yet measured; the input delays below are PCM1808 datasheet values
# only (-10..20 ns vs falling BCK). They MUST be updated with measured
# skew at ADC connection time; ADC receive functionality stays unverified
# until then. No false paths, no blanket exceptions added here.

set_property PACKAGE_PIN Y9 [get_ports ref_clk_in]
set_property IOSTANDARD LVCMOS33 [get_ports ref_clk_in]
create_clock -name sys_clk -period 20.000 [get_ports ref_clk_in]

set_property PACKAGE_PIN AA9 [get_ports bclk_in]
set_property IOSTANDARD LVCMOS33 [get_ports bclk_in]
set_property PACKAGE_PIN W12 [get_ports lrck_in]
set_property IOSTANDARD LVCMOS33 [get_ports lrck_in]
set_property PACKAGE_PIN U12 [get_ports sdata_in]
set_property IOSTANDARD LVCMOS33 [get_ports sdata_in]
set_property PACKAGE_PIN V12 [get_ports mclk_out]
set_property IOSTANDARD LVCMOS33 [get_ports mclk_out]

create_clock -name adc_bclk -period 325.520833333 [get_ports bclk_in]
set_input_delay -clock adc_bclk -clock_fall -min -10.000 [get_ports {lrck_in sdata_in}]
set_input_delay -clock adc_bclk -clock_fall -max  20.000 [get_ports {lrck_in sdata_in}]
