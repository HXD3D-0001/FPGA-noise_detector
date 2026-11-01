# Offline PS7 configuration readback only. No targets, initialization, output
# products, synthesis, bitstreams, XSA or application images are generated.
# vivado -mode batch -source check_pre_adc_profile.tcl -tclargs NEW_OUTPUT_DIR
set root [file normalize [file join [file dirname [info script]] ..]]
if {[llength $argv] != 1} {error {expected one new evidence directory}}
set out [file normalize [lindex $argv 0]]
if {[file exists $out]} {error "refusing existing evidence directory: $out"}
file mkdir $out
set rc [catch {
    source [file join $root bd ax7021_pre_adc_profile.tcl]
    set_param general.maxThreads 1
    create_project -in_memory pre_adc_profile_probe -part xc7z020clg484-2
    create_bd_design profile_probe
    foreach uart {none carrier core} {
        foreach sd {0 1} {
            set variant ${uart}_sd${sd}
            set p [::lfn_pre_adc_profile::make $uart $sd]
            if {[dict get $p gate board_ready] != 0 ||
                [dict get $p gate init_execution_authorized] != 0} {
                error {profile unexpectedly authorizes execution}
            }
            set ps [create_bd_cell -type ip -vlnv xilinx.com:ip:processing_system7:5.5 ps_$variant]
            # Reproduce the candidate's fail-closed peripheral baseline before
            # applying the independent profile. Do not source the candidate
            # builder, which would overwrite its XSA/evidence.
            set cfg [dict create CONFIG.PCW_IMPORT_BOARD_PRESET None]
            foreach name [list_property $ps] {
                set value [get_property $name $ps]
                if {[regexp {^CONFIG\.PCW_(EN_|USE_)} $name] ||
                    ([string match CONFIG.PCW_*_ENABLE $name] && $value in {0 1})} {
                    dict set cfg $name 0
                }
            }
            set_property -dict $cfg $ps
            set_property -dict [::lfn_pre_adc_profile::overlay $p] $ps
            set f [open [file join $out $variant.tsv] w]
            foreach name [lsort [list_property $ps]] {
                if {[string match CONFIG.* $name]} {
                    puts $f "$name\t[get_property $name $ps]"
                }
            }
            close $f
            # Match actual properties, not merely the submitted dictionary.
            # Disabled selectors are tool-normalized to <Select>; they have
            # no pin assignment and are covered by both enable assertions.
            dict for {name expected} [dict get $p ps_config] {
                if {$expected eq "Disabled"} {continue}
                set actual [get_property $name $ps]
                if {$actual ne $expected} {
                    error "$variant $name: actual '$actual' != '$expected'"
                }
            }
            # The 1-GiB geometry and inactive alternative peripheral are
            # checked independently of values present in the profile.
            set high [get_property CONFIG.PCW_DDR_RAM_HIGHADDR $ps]
            if {![string equal -nocase $high 0x3fffffff]} {
                error "$variant DDR geometry high address: $high"
            }
            foreach name {PCW_EN_SDIO1 PCW_SD1_PERIPHERAL_ENABLE} {
                if {[get_property CONFIG.$name $ps] ne "0"} {
                    error "$variant unwanted peripheral: $name"
                }
            }
            puts "PASS OFFLINE_PS7_READBACK $variant board_ready=0 board=NOT_RUN"
        }
    }
    close_project
} message options]
set f [open [file join $out result.json] w]
if {$rc} {
    puts $f {{"result":"FAIL","scope":"OFFLINE_PS7_PROFILE_READBACK","board":"NOT_RUN","board_ready":false}}
} else {
    puts $f {{"result":"PASS","scope":"OFFLINE_PS7_PROFILE_READBACK","variants":6,"board":"NOT_RUN","board_ready":false}}
}
close $f
if {$rc} {
    puts stderr $message
    puts stderr [dict get $options -errorinfo]
    exit 1
}
exit 0
