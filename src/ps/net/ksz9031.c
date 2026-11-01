#include "ksz9031.h"
#include "network.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "xiltimer.h"
#include "xtimer_config.h"
#include <string.h>

/* KSZ9031 ID: Microchip datasheet and Linux include/linux/micrel_phy.h.
 * Only IEEE Clause 22 registers are written; retain board strap/skew settings.
 * GEM management transaction encoding follows AMD emacps_v3_24 control.c. */
#define KSZ9031_ID 0x00221620u
static SemaphoreHandle_t mdio_lock;

void lfn_phy_initialize(void)
{
    if (!mdio_lock) mdio_lock = xSemaphoreCreateMutex();
}

static LONG transfer(XEmacPs *mac, u32 address, u32 reg, u16 *data, int write)
{
    UINTPTR base = mac->Config.BaseAddress;
    XTime start, now;
    LONG result = XST_FAILURE;
#ifdef SDT
    if (mac->Config.MdioProducerBaseAddr) base = mac->Config.MdioProducerBaseAddr;
#endif
    if (!mdio_lock || xSemaphoreTake(mdio_lock, pdMS_TO_TICKS(50)) != pdTRUE)
        return XST_FAILURE;
    if (!(XEmacPs_ReadReg(base, XEMACPS_NWSR_OFFSET) & XEMACPS_NWSR_MDIOIDLE_MASK))
        goto done;
    u32 command = XEMACPS_PHYMNTNC_OP_MASK |
        (write ? XEMACPS_PHYMNTNC_OP_W_MASK : XEMACPS_PHYMNTNC_OP_R_MASK) |
        (address << XEMACPS_PHYMNTNC_PHAD_SHFT_MSK) |
        (reg << XEMACPS_PHYMNTNC_PREG_SHFT_MSK) | (write ? *data : 0u);
    XEmacPs_WriteReg(base, XEMACPS_PHYMNTNC_OFFSET, command);
    XTime_GetTime(&start);
    do {
        if (XEmacPs_ReadReg(base, XEMACPS_NWSR_OFFSET) & XEMACPS_NWSR_MDIOIDLE_MASK) {
            if (!write) *data = (u16)XEmacPs_ReadReg(base, XEMACPS_PHYMNTNC_OFFSET);
            result = XST_SUCCESS;
            break;
        }
        XTime_GetTime(&now);
    } while (now - start < COUNTS_PER_SECOND / 500u); /* 2 ms, IRQs remain enabled. */
done:
    xSemaphoreGive(mdio_lock);
    return result;
}

/* Linker --wrap redirects all vendor MDIO traffic through the same bounded path. */
LONG __wrap_XEmacPs_PhyRead(XEmacPs *mac, u32 address, u32 reg, u16 *data)
{
    *data = 0;
    return transfer(mac, address, reg, data, 0);
}
LONG __wrap_XEmacPs_PhyWrite(XEmacPs *mac, u32 address, u32 reg, u16 data)
{
    return transfer(mac, address, reg, &data, 1);
}

static int read_reg(XEmacPs *mac, u32 address, u32 reg, u16 *value)
{
    return __wrap_XEmacPs_PhyRead(mac, address, reg, value) == XST_SUCCESS;
}

static int is_ksz9031(XEmacPs *mac, u32 address, u32 *id)
{
    u16 high, low;
    if (!read_reg(mac, address, 2, &high) || !read_reg(mac, address, 3, &low)) return 0;
    *id = ((u32)high << 16) | low;
    return (*id & 0xfffffff0u) == KSZ9031_ID;
}

void lfn_phy_detect(XEmacPs *mac, unsigned int map0[32], unsigned int map1[32])
{
    unsigned int *map = mac->Config.BaseAddress == 0xe000b000u ? map0 : map1;
    unsigned found = 0;
    memset(map, 0, 32 * sizeof(*map));
    for (u32 address = 0; address < 32; ++address) {
        u32 id = 0;
        if (is_ksz9031(mac, address, &id)) {
            map[address] = 1;
            ++found;
            lfn_network_log("NET PHY addr=%lu id=%08lx KSZ9031\r\n", address, id);
        } else if (id && id != 0xffffffffu) {
            lfn_network_log("NET PHY addr=%lu id=%08lx unsupported\r\n", address, id);
        }
    }
    lfn_network_log("NET MDIO_SCAN found=%u\r\n", found);
}

u32 lfn_phy_negotiate(XEmacPs *mac, u32 address)
{
    u32 id;
    u16 status, local, partner, gig_local, gig_partner;
    if (!is_ksz9031(mac, address, &id)) return XST_FAILURE;
    /* Full duplex only: GEM path does not switch duplex after negotiation.
     * No pause advertisement because application has no pause configuration. */
    if (__wrap_XEmacPs_PhyWrite(mac, address, 4, 0x0141u) != XST_SUCCESS ||
        __wrap_XEmacPs_PhyWrite(mac, address, 9, 0x0200u) != XST_SUCCESS ||
        __wrap_XEmacPs_PhyWrite(mac, address, 0, 0x1200u) != XST_SUCCESS)
        return XST_FAILURE;
    for (unsigned retry = 0; retry < 100; ++retry) {
        if (!read_reg(mac, address, 1, &status) || !read_reg(mac, address, 1, &status))
            return XST_FAILURE;
        if ((status & 0x0024u) == 0x0024u) {
            if (!read_reg(mac, address, 4, &local) || !read_reg(mac, address, 5, &partner) ||
                !read_reg(mac, address, 9, &gig_local) || !read_reg(mac, address, 10, &gig_partner))
                return XST_FAILURE;
            if ((gig_local & 0x0200u) && (gig_partner & 0x0800u)) return 1000;
            if ((local & partner) & 0x0100u) return 100;
            if ((local & partner) & 0x0040u) return 10;
            return XST_FAILURE;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    lfn_network_log("NET PHY_AUTONEG_TIMEOUT addr=%lu\r\n", address);
    return XST_FAILURE;
}
