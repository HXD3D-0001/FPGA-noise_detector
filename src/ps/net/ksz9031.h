#ifndef LFN_KSZ9031_H
#define LFN_KSZ9031_H
#include "xemacps.h"
void lfn_phy_initialize(void);
u32 lfn_phy_negotiate(XEmacPs *mac, u32 address);
/* lwIP's u32_t is unsigned int; Xilinx u32 is unsigned long on this BSP. */
void lfn_phy_detect(XEmacPs *mac, unsigned int map0[32], unsigned int map1[32]);
#endif
