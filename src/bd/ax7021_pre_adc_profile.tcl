# AX7021 pre-ADC board profile: pure Tcl data, no Vivado commands.
# Safe to source in plain tclsh and inside Vivado; defines no create_project or
# any other tool command. All values below are documentary facts of the AX7021
# carrier + AC7021 core (Zynq XC7Z020 CLG484), each with its pinned source.
# Status/authorization gates stay in the workspace sole report; this file never
# authorizes board execution.
#
# Provenance of every fact (local copies under docs/references/AX7021B_2023.1,
# ALINX repo pinned at d767329b1e12921447fe8afa6d756d6ac1f38f90):
# - part xc7z020clg484-2: AC7021 core schematic PAGE02..08 title bars
#   "XC7Z020-2CLG484" (design value; user silk photo 2026-10-23 confirms
#   XC7Z020/CLG484, speed grade -2 stays model-derived, see sole report).
# - DDR MT41J256M16 RE-125, 32 Bit, 533.333333 MHz: ALINX S1 tutorial p71/p95
#   (compatible preset for fitted D9SHG = MT41K256M16TW-107) and the official
#   21_sd_hdmi_out example ps_config.tcl pinned below.
# - Crystal 33.333333 MHz: core schematic PAGE03 (X1 33.333333Mhz) and the
#   same official example.
# - Carrier UART = UART1 MIO 12..13, bank0 LVCMOS 3.3V: S1 tutorial p70
#   ("serial port on PS MIO12-MIO13, enable UART1, bank0 LVCMOS 3.3V"); base
#   board schematic PAGE04 CP2102-GM USB-UART, PAGE02 PS_MIO12/13 nets.
# - Core UART = UART0 MIO 14..15, bank0 LVCMOS 3.3V: core schematic PAGE03
#   net-label column PS_MIO9..PS_MIO13 followed by UART_RXD/UART_TXD at the
#   MIO14/MIO15 pin positions (text-coordinate verified), those nets continue
#   to PAGE12 "USB Uart". No other MIO14/MIO15 nets exist on the page.
# - SD0 MIO 40..45 with card detect MIO 10: S1 tutorial p206/p207 and the
#   official example; base board schematic PAGE04 Micro SD J3 via TXS02612
#   level shifter, CD net on base PAGE02 PS_MIO10.
# - Bank voltages: core schematic PAGE03 straps "MIO[7]=0 -> bank0 3.3V",
#   "MIO[8]=1 -> bank1 1.8V"; MIO10/12..15 (bank0) LVCMOS 3.3V, MIO40..45
#   (bank1) LVCMOS 1.8V, WP not fitted (example WP_ENABLE 0).
# - PL reference clock 50 MHz on pin Y9 bank 13: core schematic PAGE04
#   "PL_GCLK / IO_L12P_T1_MRCC_13 Y9 / 50Mhz".
# - audio skew_min_ns/skew_max_ns/data_pins/bank_voltage stay PENDING until
#   the wiring-guide/oscilloscope facts required by P3 are confirmed; nominal
#   PCM1808 returned BCLK 3.072 MHz = 64fs @ 48 kHz is a plan constant;
#   the separate MCLK is 12.288 MHz = 256fs.

namespace eval ::lfn_pre_adc_profile_int {}

proc ::lfn_pre_adc_profile_int::build {uart sd0 {ethernet 0}} {
    if {$ethernet ni {0 1}} {error "invalid ethernet selection $ethernet (expected 0|1)"}
    set enU0 0
    set enU1 0
    set ioU0 {Disabled}
    set ioU1 {Disabled}
    switch -exact -- $uart {
        none {}
        carrier {
            set enU1 1
            set ioU1 {MIO 12 .. 13}
        }
        core {
            set enU0 1
            set ioU0 {MIO 14 .. 15}
        }
        default {
            error "invalid uart selection $uart (expected none|carrier|core)"
        }
    }
    if {$sd0 ne "0" && $sd0 ne "1"} {
        error "invalid sd0 selection $sd0 (expected 0|1)"
    }
    set enSD 0
    set ioSD {Disabled}
    set cdEn 0
    set cdIO {Disabled}
    if {$sd0 eq "1"} {
        set enSD 1
        set ioSD {MIO 40 .. 45}
        set cdEn 1
        set cdIO {MIO 10}
    }
    set ps [dict create \
        CONFIG.PCW_PRESET_BANK0_VOLTAGE {LVCMOS 3.3V} \
        CONFIG.PCW_PRESET_BANK1_VOLTAGE {LVCMOS 1.8V} \
        CONFIG.PCW_CRYSTAL_PERIPHERAL_FREQMHZ {33.333333} \
        CONFIG.PCW_EN_DDR {1} \
        CONFIG.PCW_UIPARAM_DDR_ENABLE {1} \
        CONFIG.PCW_UIPARAM_DDR_PARTNO {MT41J256M16 RE-125} \
        CONFIG.PCW_UIPARAM_DDR_BUS_WIDTH {32 Bit} \
        CONFIG.PCW_UIPARAM_DDR_FREQ_MHZ {533.333333} \
        CONFIG.PCW_EN_UART0 $enU0 \
        CONFIG.PCW_EN_UART1 $enU1 \
        CONFIG.PCW_UART0_PERIPHERAL_ENABLE $enU0 \
        CONFIG.PCW_UART1_PERIPHERAL_ENABLE $enU1 \
        CONFIG.PCW_UART0_UART0_IO $ioU0 \
        CONFIG.PCW_UART1_UART1_IO $ioU1 \
        CONFIG.PCW_UART1_BAUD_RATE {115200} \
        CONFIG.PCW_MIO_10_IOTYPE {LVCMOS 3.3V} \
        CONFIG.PCW_MIO_12_IOTYPE {LVCMOS 3.3V} \
        CONFIG.PCW_MIO_13_IOTYPE {LVCMOS 3.3V} \
        CONFIG.PCW_MIO_14_IOTYPE {LVCMOS 3.3V} \
        CONFIG.PCW_MIO_15_IOTYPE {LVCMOS 3.3V} \
        CONFIG.PCW_MIO_40_IOTYPE {LVCMOS 1.8V} \
        CONFIG.PCW_MIO_41_IOTYPE {LVCMOS 1.8V} \
        CONFIG.PCW_MIO_42_IOTYPE {LVCMOS 1.8V} \
        CONFIG.PCW_MIO_43_IOTYPE {LVCMOS 1.8V} \
        CONFIG.PCW_MIO_44_IOTYPE {LVCMOS 1.8V} \
        CONFIG.PCW_MIO_45_IOTYPE {LVCMOS 1.8V} \
        CONFIG.PCW_EN_SDIO0 $enSD \
        CONFIG.PCW_SD0_PERIPHERAL_ENABLE $enSD \
        CONFIG.PCW_SD0_SD0_IO $ioSD \
        CONFIG.PCW_SD0_GRP_CD_ENABLE $cdEn \
        CONFIG.PCW_SD0_GRP_CD_IO $cdIO \
        CONFIG.PCW_SD0_GRP_WP_ENABLE {0} \
    ]
    # Official 07_net_test ps_config.tcl, course_s1_fpga.zip, member SHA256
    # e6bebe26988d0102d57805bedbf9579de45a30efed35c609e36b2e952102b9b0.
    # GEM0 RGMII + MDIO + shared active-low PHY reset; only the explicit
    # Ethernet variant adds these values, preserving the SD-only overlay.
    if {$ethernet eq "1"} {
        foreach {name value} {
            PCW_EN_GPIO 1 PCW_GPIO_MIO_GPIO_ENABLE 1 PCW_GPIO_MIO_GPIO_IO MIO
            PCW_EN_ENET0 1 PCW_EN_EMIO_ENET0 0 PCW_EN_PTP_ENET0 0
            PCW_ENET0_PERIPHERAL_ENABLE 1 PCW_ENET0_ENET0_IO {MIO 16 .. 27}
            PCW_ENET0_GRP_MDIO_ENABLE 1 PCW_ENET0_GRP_MDIO_IO {MIO 52 .. 53}
            PCW_ENET0_PERIPHERAL_CLKSRC {IO PLL} PCW_ENET0_PERIPHERAL_FREQMHZ {1000 Mbps}
            PCW_ENET0_RESET_ENABLE 1 PCW_ENET0_RESET_IO {MIO 7}
            PCW_ENET_RESET_ENABLE 1 PCW_ENET_RESET_POLARITY {Active Low}
            PCW_ENET_RESET_SELECT {Share reset pin}
            PCW_MIO_7_IOTYPE {LVCMOS 3.3V} PCW_MIO_7_SLEW slow
        } {dict set ps CONFIG.$name $value}
        foreach pin {16 17 18 19 20 21 22 23 24 25 26 27} {
            dict set ps CONFIG.PCW_MIO_${pin}_IOTYPE {HSTL 1.8V}
            dict set ps CONFIG.PCW_MIO_${pin}_SLEW fast
            dict set ps CONFIG.PCW_MIO_${pin}_PULLUP enabled
        }
        foreach pin {52 53} {
            dict set ps CONFIG.PCW_MIO_${pin}_IOTYPE {LVCMOS 1.8V}
            dict set ps CONFIG.PCW_MIO_${pin}_SLEW slow
            dict set ps CONFIG.PCW_MIO_${pin}_PULLUP enabled
        }
    }
    return [dict create \
        uart $uart \
        sd0 $sd0 \
        ethernet $ethernet \
        part {xc7z020clg484-2} \
        ps_config $ps \
        clocks [dict create \
            pl_ref_pin {Y9} \
            pl_ref_mhz {50} \
            pl_ref_bank {13} \
            ps_crystal_mhz {33.333333} \
        ] \
        audio [dict create \
            skew_min_ns {PENDING} \
            skew_max_ns {PENDING} \
            bclk_nominal_hz {3072000} \
            data_pins {PENDING} \
            bank_voltage {PENDING} \
        ] \
        facts [dict create \
            physical_revision {PENDING} \
        ] \
        gate [dict create \
            board_ready {0} \
            init_execution_authorized {0} \
            missing_facts {physical_revision audio_data_pins audio_bank_voltage audio_io_skew_bounds sd_logical_volume} \
        ] \
        sources [dict create \
            core_schematic [dict create \
                sha256 {bc5ab9c88f28fa10a45cfa0aea091c3eb6109e9a297a1f82bd819dd075d00162} \
                file {AC7021 core board schematic 01_SCH/AC7021_core.pdf} \
            ] \
            base_schematic [dict create \
                sha256 {a276a949b862ee8eb366de119d00b9ca9fa6208812dcb50b20446346e11ee4df} \
                file {AX7021 base board schematic 01_SCH/AX7021_base.pdf} \
            ] \
            sd_example [dict create \
                member_sha256 {3c8f974643863310592ff780f7d0fe4a9b56496e55b53f98364bd232d5bfea18} \
                container {course_s2_vitis.zip} \
                member {course_s2_vitis/21_sd_hdmi_out/Vivado/auto_create_project/ps_config.tcl} \
            ] \
        ] \
    ]
}

# Schema-directed comparison: identical raw element count, identical key set,
# then equal leaf values; clocks/audio/facts/gate/sources (and entries inside
# sources) recurse. Returns "" when equal, otherwise a difference description.
proc ::lfn_pre_adc_profile_int::diff {exp act path} {
    if {[catch {lsort [dict keys $exp]}]} {
        return "$path: internal expected value is not a dict"
    }
    if {[catch {set ak [lsort [dict keys $act]]}]} {
        return "$path: value is not a dict"
    }
    if {[llength $act] != [llength $exp]} {
        return "$path element count [llength $act] != expected [llength $exp]"
    }
    if {$ak ne [lsort [dict keys $exp]]} {
        return "$path key set mismatch: $ak"
    }
    foreach k [lsort [dict keys $exp]] {
        set ev [dict get $exp $k]
        set av [dict get $act $k]
        if {[expr {$k in {clocks audio facts gate sources} || $path eq "sources"}]} {
            set d [::lfn_pre_adc_profile_int::diff $ev $av "$path $k"]
            if {$d ne ""} {
                return $d
            }
        } elseif {![string equal $ev $av]} {
            return "$path$k value [list $av] != expected [list $ev]"
        }
    }
    return ""
}

namespace eval ::lfn_pre_adc_profile {
    # Build explicit offline variants: make ?uart? ?sd0? ?ethernet?
    # with uart in {none carrier core} and sd0 in {0 1}. Defaults are the
    # fully-offline variant with no UART and no SD selected.
    proc make {{uart none} {sd0 0} {ethernet 0}} {
        if {$uart ne "none" && $uart ne "carrier" && $uart ne "core"} {
            error "invalid uart selection $uart (expected none|carrier|core)"
        }
        if {$sd0 ne "0" && $sd0 ne "1"} {
            error "invalid sd0 selection $sd0 (expected 0|1)"
        }
        return [::lfn_pre_adc_profile_int::build $uart $sd0 $ethernet]
    }

    # Strict validation: the profile must match exactly the canonical variant
    # built from its own uart/sd0 fields, including the fixed fact sheet and
    # the pending gates. Any missing, extra, duplicated or altered key fails.
    proc validate {p} {
        if {[catch {
            set uart [dict get $p uart]
            set sd0 [dict get $p sd0]
            set ethernet [dict get $p ethernet]
        }]} {
            error "profile mismatch: uart/sd0/ethernet not readable"
        }
        if {[catch {
            set exp [::lfn_pre_adc_profile_int::build $uart $sd0 $ethernet]
        } message]} {
            error "profile mismatch: $message"
        }
        set d [::lfn_pre_adc_profile_int::diff $exp $p {}]
        if {$d ne ""} {
            error "profile mismatch: $d"
        }
        return
    }

    # Complete documented board overlay for a future P6 implementation entry.
    # Apply after disabling unused peripherals: both enable representations
    # and the bank/DDR facts must survive that baseline. This is not a complete
    # PS7 preset; AXI, FCLK, DDR training/delays and unused peripherals remain
    # the caller's responsibility. No existing candidate entry consumes it.
    proc overlay {p} {
        validate $p
        return [dict get $p ps_config]
    }

    # One-line human summary for logs; validates first so a stale or edited
    # profile can never be summarized as if it were current.
    proc summary {p} {
        validate $p
        return "uart=[dict get $p uart] sd0=[dict get $p sd0] part=[dict get $p part] board_ready=[dict get $p gate board_ready] init_authorized=[dict get $p gate init_execution_authorized] pl_ref=[dict get $p clocks pl_ref_pin]/[dict get $p clocks pl_ref_mhz]MHz"
    }
}
