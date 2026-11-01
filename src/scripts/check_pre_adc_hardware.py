"""Validate the pre-ADC carrier UART1+SD0 variant; never authorize execution.

Same topology/payload/init machinery as the candidate checker, but the PS
external peripheral gate asserts the EXACT finalized profile set (see the
sole workspace report and bd/ax7021_pre_adc_profile.tcl provenance) instead
of the candidate all-disabled baseline. Any extra, missing or altered
peripheral, pin, voltage or DDR value fails. The candidate negative-control
suite exercises the shared topology checker; variant-specific negatives are
the in-Vivado readback assertions in bd/create_pre_adc_bd.tcl plus the
artifact-level exact-set assertion in scripts/export_pre_adc_hardware.ps1.
"""
import argparse
import json
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_capture_hardware import validate

VARIANT_PS = {
    'PCW_EN_QSPI': '0', 'PCW_EN_SDIO0': '1', 'PCW_EN_SDIO1': '0',
    'PCW_EN_USB0': '0', 'PCW_EN_USB1': '0', 'PCW_EN_ENET0': '0',
    'PCW_EN_ENET1': '0', 'PCW_EN_UART0': '0', 'PCW_EN_UART1': '1',
    'PCW_EN_EMIO_TTC0': '0',
    'PCW_UART1_UART1_IO': 'MIO 12 .. 13', 'PCW_UART1_BAUD_RATE': '115200',
    'PCW_SD0_SD0_IO': 'MIO 40 .. 45', 'PCW_SD0_GRP_CD_ENABLE': '1',
    'PCW_SD0_GRP_CD_IO': 'MIO 10', 'PCW_SD0_GRP_WP_ENABLE': '0',
    'PCW_MIO_10_IOTYPE': 'LVCMOS 3.3V', 'PCW_MIO_12_IOTYPE': 'LVCMOS 3.3V',
    'PCW_MIO_13_IOTYPE': 'LVCMOS 3.3V',
    'PCW_MIO_40_IOTYPE': 'LVCMOS 1.8V', 'PCW_MIO_41_IOTYPE': 'LVCMOS 1.8V',
    'PCW_MIO_42_IOTYPE': 'LVCMOS 1.8V', 'PCW_MIO_43_IOTYPE': 'LVCMOS 1.8V',
    'PCW_MIO_44_IOTYPE': 'LVCMOS 1.8V', 'PCW_MIO_45_IOTYPE': 'LVCMOS 1.8V',
    'PCW_PRESET_BANK0_VOLTAGE': 'LVCMOS 3.3V',
    'PCW_PRESET_BANK1_VOLTAGE': 'LVCMOS 1.8V',
    'PCW_UIPARAM_DDR_PARTNO': 'MT41J256M16 RE-125',
    'PCW_UIPARAM_DDR_BUS_WIDTH': '32 Bit',
    'PCW_UIPARAM_DDR_FREQ_MHZ': '533.333333',
    'PCW_CRYSTAL_PERIPHERAL_FREQMHZ': '33.333333',
}


NETWORK_PS = {
    **VARIANT_PS, 'PCW_EN_ENET0': '1', 'PCW_EN_EMIO_ENET0': '0',
    'PCW_EN_GPIO': '1', 'PCW_GPIO_MIO_GPIO_ENABLE': '1', 'PCW_GPIO_MIO_GPIO_IO': 'MIO',
    'PCW_GPIO_EMIO_GPIO_ENABLE': '0',
    'PCW_EN_PTP_ENET0': '0', 'PCW_ENET0_PERIPHERAL_ENABLE': '1',
    'PCW_ENET0_ENET0_IO': 'MIO 16 .. 27',
    'PCW_ENET0_GRP_MDIO_ENABLE': '1', 'PCW_ENET0_GRP_MDIO_IO': 'MIO 52 .. 53',
    'PCW_ENET0_PERIPHERAL_CLKSRC': 'IO PLL',
    'PCW_ENET0_PERIPHERAL_FREQMHZ': '1000 Mbps',
    'PCW_ACT_ENET0_PERIPHERAL_FREQMHZ': '125.000000',
    'PCW_ENET0_BASEADDR': '0xE000B000', 'PCW_ENET0_HIGHADDR': '0xE000BFFF',
    'PCW_ENET0_RESET_ENABLE': '1', 'PCW_ENET0_RESET_IO': 'MIO 7',
    'PCW_ENET_RESET_ENABLE': '1', 'PCW_ENET_RESET_POLARITY': 'Active Low',
    'PCW_ENET_RESET_SELECT': 'Share reset pin',
    'PCW_MIO_7_IOTYPE': 'LVCMOS 3.3V', 'PCW_MIO_7_SLEW': 'slow',
    'PCW_MIO_7_DIRECTION': 'out', 'PCW_MIO_7_PULLUP': 'disabled',
}
for pin in range(16, 28):
    NETWORK_PS.update({f'PCW_MIO_{pin}_IOTYPE': 'HSTL 1.8V',
                       f'PCW_MIO_{pin}_SLEW': 'fast', f'PCW_MIO_{pin}_PULLUP': 'enabled'})
for pin in (52, 53):
    NETWORK_PS.update({f'PCW_MIO_{pin}_IOTYPE': 'LVCMOS 1.8V',
                       f'PCW_MIO_{pin}_SLEW': 'slow', f'PCW_MIO_{pin}_PULLUP': 'enabled'})


def validate_variant(path, *, ethernet=False):
    profile = 'CARRIER_SD0_ETHERNET' if ethernet else 'CARRIER_SD0'
    return validate(path, NETWORK_PS if ethernet else VARIANT_PS,
                    scope=f'PRE_ADC_PROFILE_{profile}_METADATA_ONLY',
                    board_profile_status=f'PROFILE_{profile}_FITTED_DDR_AUDIO_SKEW_UNVERIFIED')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('xsa', type=Path)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--ethernet', action='store_true', help='Require the separate GEM0 Ethernet profile')
    a = p.parse_args()
    record = {'status': 'RUNNING', 'board_execution_authorized': False,
              'init_execution_authorized': False}
    a.output.write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
    try:
        record = validate_variant(a.xsa, ethernet=a.ethernet)
    except Exception as e:
        record.update(status='FAIL', error=str(e))
        a.output.write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
        raise
    a.output.write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
    profile = 'CARRIER_SD0_ETHERNET' if a.ethernet else 'CARRIER_SD0'
    print(f'PASS pre_adc_hardware PROFILE_{profile}_METADATA_ONLY')


if __name__ == '__main__':
    main()
