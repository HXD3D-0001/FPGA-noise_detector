"""Generate an application-local AMD PHY adapter, preserving its license and clocks.

The generated translation unit replaces the library's physpeed object at link time.
No vendor installation or BSP files are edited. Link ksz9031.c and use
--wrap=XEmacPs_PhyRead,--wrap=XEmacPs_PhyWrite for bounded MDIO throughout lwIP.
"""
import argparse
import hashlib
import json
from pathlib import Path


def replace_function(text, signature, body):
    start = text.index(signature + "\n{")
    brace = text.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[:brace] + "{\n" + body + "\n}" + text[end:]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--vendor", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = args.vendor.read_text(encoding="utf-8")
    source = source.replace('#include "netif/xemacpsif.h"',
                            '#include "netif/xemacpsif.h"\n#include "ksz9031.h"\n#include "network.h"', 1)
    source = source.replace('xil_printf(', 'lfn_network_log(')
    source = replace_function(source,
        "static u32_t get_IEEE_phy_speed(XEmacPs *xemacpsp, u32_t phy_addr)",
        "    return lfn_phy_negotiate(xemacpsp, phy_addr);")
    source = replace_function(source, "void detect_phy(XEmacPs *xemacpsp)",
        "    lfn_phy_detect(xemacpsp, phymapemac0, phymapemac1);")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(source, encoding="utf-8")
    adapter_vendor = args.vendor.with_name('xadapter.c')
    adapter = adapter_vendor.read_text(encoding='utf-8')
    adapter = adapter.replace('#include "lwipopts.h"',
        '#include "lwipopts.h"\n#include "network.h"', 1)
    adapter = adapter.replace('xil_printf(', 'lfn_network_log(')
    # Queued diagnostic formatting uses libc, unlike the vendor tiny printf.
    assert adapter.count('#define THREAD_STACKSIZE 256') == 1
    adapter = adapter.replace('#define THREAD_STACKSIZE 256', '#define THREAD_STACKSIZE 1024')
    anchor = '\t#ifdef OS_IS_FREERTOS\n\t\t/* Start thread'
    assert adapter.count(anchor) == 1
    adapter = adapter.replace(anchor, '''#ifdef XLWIP_CONFIG_INCLUDE_GEM
    /* The vendor netif init initially sets LINK_UP even if AN failed. */
    if (nif && find_mac_type(mac_baseaddr) == xemac_type_emacps) {
        struct xemac_s *mac = (struct xemac_s *)nif->state;
        xemacpsif_s *gem = (xemacpsif_s *)mac->state;
        if (gem->eth_link_status != ETH_LINK_UP)
            netif_clear_flags(nif, NETIF_FLAG_LINK_UP);
    }
#endif
''' + anchor, 1)
    anchor = '\t\t\t\tXEmacPs_SetOperatingSpeed(xemacp, link_speed);'
    assert adapter.count(anchor) == 1
    adapter = adapter.replace(anchor, '''                if (link_speed != 10 && link_speed != 100 && link_speed != 1000) {
                    lfn_network_log("NET PHY_RENEGOTIATION_FAILED\\r\\n");
                    break;
                }
''' + anchor, 1)
    # Link-state callbacks can alter TCP and DHCP; keep them on tcpip_thread.
    anchor = '/* global lwip debug variable'
    assert adapter.count(anchor) == 1
    adapter = adapter.replace(anchor, '''#if NO_SYS
#error This application-local adapter requires the FreeRTOS socket BSP
#endif
static void lfn_adapter_link_up(void *netif) { netif_set_link_up(netif); }
static void lfn_adapter_link_down(void *netif) { netif_set_link_down(netif); }

''' + anchor, 1)
    # Only the original calls have a tab indentation, not the helpers above.
    adapter = adapter.replace('\tnetif_set_link_up(netif);',
                              '\tif (tcpip_callback(lfn_adapter_link_up, netif) != ERR_OK) return;')
    adapter = adapter.replace('\tnetif_set_link_down(netif);',
                              '\tif (tcpip_callback(lfn_adapter_link_down, netif) != ERR_OK) return;')
    adapter_output = args.output.with_name('xadapter.c')
    adapter_output.write_text(adapter, encoding='utf-8')
    receipt = {
        "vendor": str(args.vendor.resolve()),
        "vendor_sha256": hashlib.sha256(args.vendor.read_bytes()).hexdigest(),
        "output": str(args.output.resolve()),
        "output_sha256": hashlib.sha256(args.output.read_bytes()).hexdigest(),
        "adapter_vendor": str(adapter_vendor.resolve()),
        "adapter_vendor_sha256": hashlib.sha256(adapter_vendor.read_bytes()).hexdigest(),
        "adapter_output": str(adapter_output.resolve()),
        "adapter_output_sha256": hashlib.sha256(adapter_output.read_bytes()).hexdigest(),
        "phy": "KSZ9031 0x0022162x; IEEE full-duplex auto-negotiation; retain strap skew",
        "compile": "-Ips/net -Wno-unused-function; CONFIG_LINKSPEED_AUTODETECT required",
        "link": "--wrap=XEmacPs_PhyRead,--wrap=XEmacPs_PhyWrite",
    }
    args.output.with_suffix(".json").write_text(json.dumps(receipt, indent=2) + "\n")


if __name__ == "__main__":
    main()
