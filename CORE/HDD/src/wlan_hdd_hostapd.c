/*
 * Copyright (c) 2012-2018 The Linux Foundation. All rights reserved.
 *
 * Previously licensed under the ISC license by Qualcomm Atheros, Inc.
 *
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * This file was originally distributed by Qualcomm Atheros, Inc.
 * under proprietary terms before Copyright ownership was assigned
 * to the Linux Foundation.
 */

/**========================================================================

  \file  wlan_hdd_hostapd.c
  \brief WLAN Host Device Driver implementation

  ========================================================================*/
/**=========================================================================
                       EDIT HISTORY FOR FILE


  This section contains comments describing changes made to the module.
  Notice that changes are listed in reverse chronological order.

  $Header:$   $DateTime: $ $Author: $


  when        who    what, where, why
  --------    ---    --------------------------------------------------------
  04/5/09     Shailender     Created module.
  06/03/10    js - Added support to hostapd driven deauth/disassoc/mic failure
  ==========================================================================*/
/*--------------------------------------------------------------------------
  Include Files
  ------------------------------------------------------------------------*/

#include <linux/version.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/wireless.h>
#include <linux/semaphore.h>
#include <linux/compat.h>
#include <vos_api.h>
#include <vos_sched.h>
#include <linux/etherdevice.h>
#include <wlan_hdd_includes.h>
#include <qc_sap_ioctl.h>
#include <wlan_hdd_hostapd.h>
#include <sapApi.h>
#include <sapInternal.h>
#include <wlan_qct_tl.h>
#include <wlan_hdd_softap_tx_rx.h>
#include <wlan_hdd_main.h>
#include <linux/netdevice.h>
#include <linux/mmc/sdio_func.h>
#include "wlan_nlink_common.h"
#include "wlan_hdd_p2p.h"
#ifdef IPA_OFFLOAD
#include <wlan_hdd_ipa.h>
#endif
#include "cfgApi.h"
#include "wni_cfg.h"
#include "wlan_hdd_misc.h"
#include <vos_utils.h>
#include "vos_cnss.h"
#include "tl_shim.h"

#include "wma.h"
#ifdef WLAN_DEBUG
#include "wma_api.h"
#endif
extern int process_wma_set_command(int sessid, int paramid,
                                   int sval, int vpdev);
#include "wlan_hdd_trace.h"
#include "vos_types.h"
#include "vos_trace.h"
#include "adf_trace.h"
#include "wlan_hdd_cfg.h"
#include <wlan_hdd_wowl.h>
#include "wlan_hdd_tsf.h"
#include "wlan_hdd_oemdata.h"
#include "wlan_hdd_request_manager.h"

#ifdef WLAN_FEATURE_SAP_TO_FOLLOW_STA_CHAN
#include <vos_utils.h>
#endif//#ifdef WLAN_FEATURE_SAP_TO_FOLLOW_STA_CHAN

#define    IS_UP(_dev) \
    (((_dev)->flags & (IFF_RUNNING|IFF_UP)) == (IFF_RUNNING|IFF_UP))
#define    IS_UP_AUTO(_ic) \
    (IS_UP((_ic)->ic_dev) && (_ic)->ic_roaming == IEEE80211_ROAMING_AUTO)
#define WE_WLAN_VERSION     1
#define WE_GET_STA_INFO_SIZE 30
/* WEXT limitation: MAX allowed buf len for any *
 * IW_PRIV_TYPE_CHAR is 2Kbytes *
 */
#define WE_SAP_MAX_STA_INFO 0x7FF

#define RC_2_RATE_IDX(_rc)        ((_rc) & 0x7)
#define HT_RC_2_STREAMS(_rc)    ((((_rc) & 0x78) >> 3) + 1)
#define RC_2_RATE_IDX_11AC(_rc)        ((_rc) & 0xf)
#define HT_RC_2_STREAMS_11AC(_rc)    ((((_rc) & 0x30) >> 4) + 1)

#define SAP_24GHZ_CH_COUNT (14)
#define ACS_SCAN_EXPIRY_TIMEOUT_S 4

/* EID byte + length byte + four byte WiFi OUI */
#define DOT11F_EID_HEADER_LEN (6)

/* Data rate 100KBPS based on IE Index */
struct index_data_rate_type {
	uint8_t   beacon_rate_index;
	uint16_t  supported_rate[4];
};

/*
 * 11B, 11G Rate table include Basic rate and Extended rate
 * The IDX field is the rate index
 * The HI field is the rate when RSSI is strong or being ignored
 * (in this case we report actual rate)
 * The MID field is the rate when RSSI is moderate
 * (in this case we cap 11b rates at 5.5 and 11g rates at 24)
 * The LO field is the rate when RSSI is low
 * (in this case we don't report rates, actual current rate used)
 */
static const struct index_data_rate_type supported_data_rate[] = {
	/* IDX     HI  HM  LM LO (RSSI-based index */
	{2,   { 10,  10, 10, 0} },
	{4,   { 20,  20, 10, 0} },
	{11,  { 55,  20, 10, 0} },
	{12,  { 60,  55, 20, 0} },
	{18,  { 90,  55, 20, 0} },
	{22,  {110,  55, 20, 0} },
	{24,  {120,  90, 60, 0} },
	{36,  {180, 120, 60, 0} },
	{44,  {220, 180, 60, 0} },
	{48,  {240, 180, 90, 0} },
	{66,  {330, 180, 90, 0} },
	{72,  {360, 240, 90, 0} },
	{96,  {480, 240, 120, 0} },
	{108, {540, 240, 120, 0} }
};

/* MCS Based rate table */
/* HT MCS parameters with Nss = 1 */
static struct index_data_rate_type supported_mcs_rate_nss1[] = {
	/* MCS  L20   L40   S20  S40 */
	{0,  { 65,  135,  72,  150} },
	{1,  { 130, 270,  144, 300} },
	{2,  { 195, 405,  217, 450} },
	{3,  { 260, 540,  289, 600} },
	{4,  { 390, 810,  433, 900} },
	{5,  { 520, 1080, 578, 1200} },
	{6,  { 585, 1215, 650, 1350} },
	{7,  { 650, 1350, 722, 1500} }
};

/* HT MCS parameters with Nss = 2 */
static struct index_data_rate_type supported_mcs_rate_nss2[] = {
	/* MCS  L20    L40   S20   S40 */
	{0,  {130,  270,  144,  300} },
	{1,  {260,  540,  289,  600} },
	{2,  {390,  810,  433,  900} },
	{3,  {520,  1080, 578,  1200} },
	{4,  {780,  1620, 867,  1800} },
	{5,  {1040, 2160, 1156, 2400} },
	{6,  {1170, 2430, 1300, 2700} },
	{7,  {1300, 2700, 1444, 3000} }
};

#ifdef WLAN_FEATURE_11AC
#define DATA_RATE_11AC_MCS_MASK    0x03

enum data_rate_11ac_max_mcs {
	DATA_RATE_11AC_MAX_MCS_7,
	DATA_RATE_11AC_MAX_MCS_8,
	DATA_RATE_11AC_MAX_MCS_9,
	DATA_RATE_11AC_MAX_MCS_NA
};

struct index_vht_data_rate_type {
	uint8_t   beacon_rate_index;
	uint16_t  supported_VHT80_rate[2];
	uint16_t  supported_VHT40_rate[2];
	uint16_t  supported_VHT20_rate[2];
};

/* MCS Based VHT rate table */
/* MCS parameters with Nss = 1*/
static struct index_vht_data_rate_type supported_vht_mcs_rate_nss1[] = {
	/* MCS  L80    S80     L40   S40    L20   S40*/
	{0,  {293,  325},  {135,  150},  {65,   72} },
	{1,  {585,  650},  {270,  300},  {130,  144} },
	{2,  {878,  975},  {405,  450},  {195,  217} },
	{3,  {1170, 1300}, {540,  600},  {260,  289} },
	{4,  {1755, 1950}, {810,  900},  {390,  433} },
	{5,  {2340, 2600}, {1080, 1200}, {520,  578} },
	{6,  {2633, 2925}, {1215, 1350}, {585,  650} },
	{7,  {2925, 3250}, {1350, 1500}, {650,  722} },
	{8,  {3510, 3900}, {1620, 1800}, {780,  867} },
	{9,  {3900, 4333}, {1800, 2000}, {780,  867} }
};

/*MCS parameters with Nss = 2*/
static struct index_vht_data_rate_type supported_vht_mcs_rate_nss2[] = {
	/* MCS  L80    S80     L40   S40    L20   S40*/
	{0,  {585,  650},  {270,  300},  {130,  144} },
	{1,  {1170, 1300}, {540,  600},  {260,  289} },
	{2,  {1755, 1950}, {810,  900},  {390,  433} },
	{3,  {2340, 2600}, {1080, 1200}, {520,  578} },
	{4,  {3510, 3900}, {1620, 1800}, {780,  867} },
	{5,  {4680, 5200}, {2160, 2400}, {1040, 1156} },
	{6,  {5265, 5850}, {2430, 2700}, {1170, 1300} },
	{7,  {5850, 6500}, {2700, 3000}, {1300, 1444} },
	{8,  {7020, 7800}, {3240, 3600}, {1560, 1733} },
	{9,  {7800, 8667}, {3600, 4000}, {1560, 1733} }
};
#endif /* WLAN_FEATURE_11AC */

/*---------------------------------------------------------------------------
 *   Function definitions
 *-------------------------------------------------------------------------*/

/**---------------------------------------------------------------------------

  \brief hdd_hostapd_channel_wakelock_init

  \param  - Pointer to HDD context

  \return - None

  --------------------------------------------------------------------------*/
void hdd_hostapd_channel_wakelock_init(hdd_context_t *pHddCtx)
{
    /* Initialize the wakelock */
    vos_wake_lock_init(&pHddCtx->sap_dfs_wakelock, "sap_dfs_wakelock");
    atomic_set(&pHddCtx->sap_dfs_ref_cnt, 0);
}

/**---------------------------------------------------------------------------

  \brief hdd_hostapd_channel_allow_suspend - Allow suspend in a channel.

            Called when,
                1. BSS stopped
                2. Channel switch

  \param  - pAdapter, channel

  \return - None

  --------------------------------------------------------------------------*/
void hdd_hostapd_channel_allow_suspend(hdd_adapter_t *pAdapter,
        u_int8_t channel)
{

    hdd_context_t *pHddCtx = (hdd_context_t*)(pAdapter->pHddCtx);
    hdd_hostapd_state_t *pHostapdState =
        WLAN_HDD_GET_HOSTAP_STATE_PTR(pAdapter);

    hddLog(LOG1, FL("bssState: %d, channel: %d, dfs_ref_cnt: %d"),
            pHostapdState->bssState, channel,
            atomic_read(&pHddCtx->sap_dfs_ref_cnt));

    /* Return if BSS is already stopped */
    if (pHostapdState->bssState == BSS_STOP)
        return;

    /* Release wakelock when no more DFS channels are used */
    if (NV_CHANNEL_DFS == vos_nv_getChannelEnabledState(channel)) {
        if (atomic_dec_and_test(&pHddCtx->sap_dfs_ref_cnt)) {
            hddLog(LOGE, FL("DFS: allowing suspend (chan %d)"), channel);
            vos_wake_lock_release(&pHddCtx->sap_dfs_wakelock,
                                  WIFI_POWER_EVENT_WAKELOCK_DFS);
            vos_runtime_pm_allow_suspend(pHddCtx->runtime_context.dfs);
        }
    }
}

/**---------------------------------------------------------------------------

  \brief hdd_hostapd_channel_prevent_suspend - Prevent suspend in a channel.

            Called when,
                1. BSS started
                2. Channel switch

  \param  - pAdapter, channel

  \return - None

  --------------------------------------------------------------------------*/
void hdd_hostapd_channel_prevent_suspend(hdd_adapter_t *pAdapter,
        u_int8_t channel)
{
    hdd_context_t *pHddCtx = (hdd_context_t*)(pAdapter->pHddCtx);
    hdd_hostapd_state_t *pHostapdState =
        WLAN_HDD_GET_HOSTAP_STATE_PTR(pAdapter);

    hddLog(LOG1, FL("bssState: %d, channel: %d, dfs_ref_cnt: %d"),
            pHostapdState->bssState, channel,
            atomic_read(&pHddCtx->sap_dfs_ref_cnt));

    /* Return if BSS is already started && wakelock is acquired */
    if ((pHostapdState->bssState == BSS_START) &&
            (atomic_read(&pHddCtx->sap_dfs_ref_cnt) >= 1))
        return;

    /* Acquire wakelock if we have at least one DFS channel in use */
    if (NV_CHANNEL_DFS == vos_nv_getChannelEnabledState(channel)) {
        if (atomic_inc_return(&pHddCtx->sap_dfs_ref_cnt) == 1) {
            hddLog(LOGE, FL("DFS: preventing suspend (chan %d)"), channel);
            vos_runtime_pm_prevent_suspend(pHddCtx->runtime_context.dfs);
            vos_wake_lock_acquire(&pHddCtx->sap_dfs_wakelock,
                                  WIFI_POWER_EVENT_WAKELOCK_DFS);
        }
    }
}

/**---------------------------------------------------------------------------

  \brief hdd_hostapd_channel_wakelock_deinit

  \param  - Pointer to HDD context

  \return - None

  --------------------------------------------------------------------------*/
void hdd_hostapd_channel_wakelock_deinit(hdd_context_t *pHddCtx)
{
    if (atomic_read(&pHddCtx->sap_dfs_ref_cnt)) {
        /* Release wakelock */
        vos_wake_lock_release(&pHddCtx->sap_dfs_wakelock,
                              WIFI_POWER_EVENT_WAKELOCK_DRIVER_EXIT);
        /* Reset the reference count */
        atomic_set(&pHddCtx->sap_dfs_ref_cnt, 0);
        hddLog(LOGE, FL("DFS: allowing suspend"));
    }

    /* Destroy lock */
    vos_wake_lock_destroy(&pHddCtx->sap_dfs_wakelock);
}

#ifdef FEATURE_WLAN_SUB_20_MHZ
/**
  * hdd_hostapd_sub20_channelwidth_can_switch() - check
  *	 channel width switch to 5/10M condition
  * @adapter: pointer to HDD context
  * @sub20_channel_width: 5MHz/10MHz channel width
  *
  * Return:  true or false
  */
bool hdd_hostapd_sub20_channelwidth_can_switch(
	hdd_adapter_t *adapter, uint32_t *sub20_channel_width)
{
	int i;
	int sta_count = 0;
	uint8_t sap_s20_caps;
	uint8_t sap_s20_config;
	uint8_t sta_s20_caps = SUB20_MODE_NONE;
	tHalHandle hal_ptr = WLAN_HDD_GET_HAL_CTX(adapter);
	tSmeConfigParams *sme_config;
	hdd_station_info_t *sta;
	hdd_ap_ctx_t *ap = WLAN_HDD_GET_AP_CTX_PTR(adapter);

	sme_config = vos_mem_malloc(sizeof(*sme_config));
	if (!sme_config) {
		hddLog(LOGE, FL("mem alloc failed for sme_config"));
		return false;
	}
	vos_mem_zero(sme_config, sizeof(*sme_config));

	sme_GetConfigParam(hal_ptr, sme_config);
	sap_s20_caps = sme_config->sub20_dynamic_channelwidth;
	sap_s20_config = sme_config->sub20_config_info;
	vos_mem_free(sme_config);
	if (sap_s20_caps == SUB20_MODE_NONE ||
	    sap_s20_config == CFG_SUB_20_CHANNEL_WIDTH_MANUAL) {
		hddLog(LOGE, FL("sub20 not switch"));
		return false;
	}

	spin_lock_bh(&adapter->staInfo_lock);
	for (i = 0; i < WLAN_MAX_STA_COUNT; i++) {
		sta = &adapter->aStaInfo[i];
		if (sta->isUsed && (ap->uBCStaId != i)) {
			sta_count++;
			sta_s20_caps |=
				sta->sub20_dynamic_channelwidth;
		}
	}
	spin_unlock_bh(&adapter->staInfo_lock);

	if (sta_count != 1) {
		hddLog(VOS_TRACE_LEVEL_ERROR,
		       "%d STAs connected with sub20 Channelwidth %d",
		       sta_count, sta_s20_caps);
		return false;
	}

	*sub20_channel_width = sta_s20_caps & sap_s20_caps;

	if (*sub20_channel_width == (SUB20_MODE_5MHZ | SUB20_MODE_10MHZ))
		*sub20_channel_width = SUB20_MODE_10MHZ;

	if (*sub20_channel_width != 0)
		return true;
	else
		return false;
}

/**
  * hdd_hostapd_sub20_channelwidth_can_restore() - check
  *	 channel width switch to normal condition
  * @adapter: pointer to HDD context
  *
  * Return:  true or false
  */
bool hdd_hostapd_sub20_channelwidth_can_restore(
	hdd_adapter_t *adapter)
{
	int i;
	int sta_count = 0;
	uint8_t sap_s20_caps;
	uint8_t sta_s20_caps = SUB20_MODE_NONE;
	tHalHandle hal_ptr = WLAN_HDD_GET_HAL_CTX(adapter);
	tSmeConfigParams *sme_config;
	hdd_station_info_t *sta;
	hdd_ap_ctx_t *ap = WLAN_HDD_GET_AP_CTX_PTR(adapter);

	sme_config = vos_mem_malloc(sizeof(*sme_config));
	if (!sme_config) {
		hddLog(LOGE, FL("mem alloc failed for sme_config"));
		return false;
	}
	vos_mem_zero(sme_config, sizeof(*sme_config));
	sme_GetConfigParam(hal_ptr, sme_config);

	sap_s20_caps = sme_config->sub20_dynamic_channelwidth;
	vos_mem_free(sme_config);
	if (sap_s20_caps == SUB20_MODE_NONE) {
		hddLog(LOGE, FL("sub20 none"));
		return false;
	}
	spin_lock_bh(&adapter->staInfo_lock);
	for (i = 0; i < WLAN_MAX_STA_COUNT; i++) {
		sta = &adapter->aStaInfo[i];
		if (sta->isUsed && (ap->uBCStaId != i)) {
			sta_count++;
			sta_s20_caps |=
				sta->sub20_dynamic_channelwidth;
		}
	}
	spin_unlock_bh(&adapter->staInfo_lock);

	if (sta_count != 0) {
		hddLog(VOS_TRACE_LEVEL_ERROR,
		       "%d STAs connected with sub20 Channelwidth %d",
		       sta_count, sta_s20_caps);
		return false;
	} else {
		return true;
	}
}

/**
  * hdd_sub20_channelwidth_can_set() - check
  * channel width manual switch to 5/10M condition
  * @adapter: pointer to HDD context
  * @sub20_channel_width: new channel width
  *
  * Return:  true or false
  */
bool hdd_sub20_channelwidth_can_set(
	hdd_adapter_t *adapter, uint32_t sub20_channel_width)
{
	int i;
	uint32_t sta_count = 0;
	uint8_t sap_s20_config;
	uint8_t sta_s20_caps = SUB20_MODE_10MHZ|SUB20_MODE_5MHZ;
	tHalHandle hal_ptr;
	tSmeConfigParams *sme_config;
	hdd_station_info_t *sta;
	hdd_ap_ctx_t *ap;
	bool channel_support_sub20 = true;
	enum phy_ch_width phy_sub20_channel_width = CH_WIDTH_INVALID;
	hdd_station_ctx_t *hddstactx;

	if (adapter == NULL) {
		hddLog(LOGE, FL("adapter NULL"));
		return false;
	}
	hal_ptr = WLAN_HDD_GET_HAL_CTX(adapter);
	ap = WLAN_HDD_GET_AP_CTX_PTR(adapter);

	sme_config = vos_mem_malloc(sizeof(*sme_config));
	if (!sme_config) {
		hddLog(LOGE, FL("mem alloc failed for sme_config"));
		return false;
	}
	vos_mem_zero(sme_config, sizeof(*sme_config));

	sme_GetConfigParam(hal_ptr, sme_config);
	sap_s20_config = sme_config->sub20_config_info;
	vos_mem_free(sme_config);
	sme_config = NULL;
	if (sap_s20_config != CFG_SUB_20_CHANNEL_WIDTH_MANUAL) {
		hddLog(LOGE, FL("ini unsupport manual set sub20"));
		return false;
	}

	switch (sub20_channel_width) {
	case SUB20_MODE_5MHZ:
		phy_sub20_channel_width = CH_WIDTH_5MHZ;
		break;
	case SUB20_MODE_10MHZ:
		phy_sub20_channel_width = CH_WIDTH_10MHZ;
		break;
	case SUB20_MODE_NONE:
		if (WLAN_HDD_SOFTAP == adapter->device_mode)
			return true;
		break;
	default:
		return false;
	}

	if (WLAN_HDD_INFRA_STATION == adapter->device_mode) {
		hddstactx = WLAN_HDD_GET_STATION_CTX_PTR(adapter);
		if (hddstactx == NULL) {
			hddLog(LOGE, FL("hdd hddstactx is null"));
			return false;
		}
		if (hdd_connIsConnected(hddstactx)) {
			hddLog(LOGE, FL("sta in Connected state!"));
			return false;
		}
		hddLog(LOGE, FL("sta can set sub20"));
		return true;
	}

	channel_support_sub20 =
	      vos_is_channel_support_sub20(ap->operatingChannel,
					   phy_sub20_channel_width,
					   0);
	if (!channel_support_sub20) {
		hddLog(VOS_TRACE_LEVEL_ERROR,
		       FL("ch%d width%d unsupport by reg domain"),
		       ap->operatingChannel, phy_sub20_channel_width);
		return false;
	}

	spin_lock_bh(&adapter->staInfo_lock);
	for (i = 0; i < WLAN_MAX_STA_COUNT; i++) {
		sta = &adapter->aStaInfo[i];
		if (sta->isUsed && (ap->uBCStaId != i)) {
			sta_count++;
			sta_s20_caps &=
				sta->sub20_dynamic_channelwidth;
		}
	}
	spin_unlock_bh(&adapter->staInfo_lock);
	if (sta_count >= 1 && !(sta_s20_caps & sub20_channel_width)) {
		hddLog(VOS_TRACE_LEVEL_ERROR,
		       "%d STAs connected with sub20 Channelwidth %d",
		       sta_count, sta_s20_caps);
		return false;
	}

	return true;
}

#endif

/**
 * __hdd_hostapd_open() - HDD Open function for hostapd interface
 * @dev: pointer to net device
 *
 * This is called in response to ifconfig up
 *
 * Return: 0 on success, error number otherwise
 */
static int __hdd_hostapd_open(struct net_device *dev)
{
   hdd_adapter_t *pAdapter = netdev_priv(dev);

   ENTER();

   MTRACE(vos_trace(VOS_MODULE_ID_HDD,
                    TRACE_CODE_HDD_HOSTAPD_OPEN_REQUEST, NO_SESSION, 0));

   if (WLAN_HDD_GET_CTX(pAdapter)->isLoadInProgress ||
        WLAN_HDD_GET_CTX(pAdapter)->isUnloadInProgress)
   {
       hddLog(LOGE, FL("Driver load/unload in progress, ignore adapter open"));
       goto done;
   }

   //Enable all Tx queues
   hddLog(LOG1, FL("Enabling queues"));
   wlan_hdd_netif_queue_control(pAdapter,
        WLAN_START_ALL_NETIF_QUEUE_N_CARRIER,
        WLAN_CONTROL_PATH);
done:
   EXIT();
   return 0;
}

/**
 * hdd_hostapd_open() - SSR wrapper for __hdd_hostapd_open
 * @dev: pointer to net device
 *
 * Return: 0 on success, error number otherwise
 */
static int hdd_hostapd_open(struct net_device *dev)
{
	int ret;

	vos_ssr_protect(__func__);
	ret = __hdd_hostapd_open(dev);
	vos_ssr_unprotect(__func__);

	return ret;
}

/**
 * __hdd_hostapd_stop() - HDD stop function for hostapd interface
 * @dev: pointer to net_device
 *
 * This is called in response to ifconfig down
 *
 * Return: 0 on success, error number otherwise
 */
static int __hdd_hostapd_stop(struct net_device *dev)
{
   ENTER();

   if (NULL != dev) {
       hdd_adapter_t *adapter = WLAN_HDD_GET_PRIV_PTR(dev);
       //Stop all tx queues
       hddLog(LOG1, FL("Disabling queues"));
       wlan_hdd_netif_queue_control(adapter, WLAN_NETIF_TX_DISABLE_N_CARRIER,
            WLAN_CONTROL_PATH);
   }

   EXIT();
   return 0;
}

/**
 * hdd_hostapd_stop() - SSR wrapper for__hdd_hostapd_stop
 * @dev: pointer to net_device
 *
 * This is called in response to ifconfig down
 *
 * Return: 0 on success, error number otherwise
 */
int hdd_hostapd_stop(struct net_device *dev)
{
	int ret;

	vos_ssr_protect(__func__);
	ret = __hdd_hostapd_stop(dev);
	vos_ssr_unprotect(__func__);

	return ret;
}

/**
 * __hdd_hostapd_uninit() - HDD uninit function
 * @dev: pointer to net_device
 *
 * This is called during the netdev unregister to uninitialize all data
 * associated with the device
 *
 * Return: 0 on success, error number otherwise
 */
static void __hdd_hostapd_uninit(struct net_device *dev)
{
	hdd_adapter_t *adapter = netdev_priv(dev);
	hdd_context_t *hdd_ctx;

	ENTER();

	if (WLAN_HDD_ADAPTER_MAGIC != adapter->magic) {
		hddLog(LOGE, FL("Invalid magic"));
		return;
	}

	hdd_ctx = WLAN_HDD_GET_CTX(adapter);
	if (NULL == hdd_ctx) {
		hddLog(LOGE, FL("NULL hdd_ctx"));
		return;
	}

	hdd_deinit_adapter(hdd_ctx, adapter, true);

	/* after uninit our adapter structure will no longer be valid */
	adapter->dev = NULL;
	adapter->magic = 0;

	EXIT();
}

/**
 * hdd_hostapd_uninit() - SSR wrapper for __hdd_hostapd_uninit
 * @dev: pointer to net_device
 *
 * Return: 0 on success, error number otherwise
 */
static void hdd_hostapd_uninit(struct net_device *dev)
{
	vos_ssr_protect(__func__);
	__hdd_hostapd_uninit(dev);
	vos_ssr_unprotect(__func__);
}

/**
 * __hdd_hostapd_change_mtu() - change mtu
 * @dev: pointer to net_device
 * @new_mtu: new mtu
 *
 * Return: 0 on success, error number otherwise
 */
static int __hdd_hostapd_change_mtu(struct net_device *dev, int new_mtu)
{
    return 0;
}

/**
 * hdd_hostapd_change_mtu() - SSR wrapper for __hdd_hostapd_change_mtu
 * @dev: pointer to net_device
 * @new_mtu: new mtu
 *
 * Return: 0 on success, error number otherwise
 */
static int hdd_hostapd_change_mtu(struct net_device *dev, int new_mtu)
{
	int ret;

	vos_ssr_protect(__func__);
	ret = __hdd_hostapd_change_mtu(dev, new_mtu);
	vos_ssr_unprotect(__func__);

	return ret;
}

static int hdd_hostapd_driver_command(hdd_adapter_t *pAdapter,
                                      hdd_priv_data_t *priv_data)
{
   tANI_U8 *command = NULL;
   int ret = 0;

   if (VOS_FTM_MODE == hdd_get_conparam()) {
        hddLog(LOGE, FL("Command not allowed in FTM mode"));
        return -EINVAL;
   }

   /*
    * Note that valid pointers are provided by caller
    */

   ENTER();

   if (priv_data->total_len <= 0 ||
       priv_data->total_len > HOSTAPD_IOCTL_COMMAND_STRLEN_MAX)
   {
      /* below we allocate one more byte for command buffer.
       * To avoid addition overflow total_len should be
       * smaller than INT_MAX. */
      hddLog(VOS_TRACE_LEVEL_ERROR, "%s: integer out of range len %d",
             __func__, priv_data->total_len);
      ret = -EFAULT;
      goto exit;
   }

   /* Allocate +1 for '\0' */
   command = vos_mem_malloc((priv_data->total_len + 1));
   if (!command)
   {
      hddLog(VOS_TRACE_LEVEL_ERROR, "%s: failed to allocate memory", __func__);
      ret = -ENOMEM;
      goto exit;
   }

   if (copy_from_user(command, priv_data->buf, priv_data->total_len))
   {
      ret = -EFAULT;
      goto exit;
   }

   /* Make sure the command is NUL-terminated */
   command[priv_data->total_len] = '\0';

   hddLog(VOS_TRACE_LEVEL_INFO,
          "***HOSTAPD*** : Received %s cmd from Wi-Fi GUI***", command);

   if (strncmp(command, "P2P_SET_NOA", 11) == 0)
   {
      hdd_setP2pNoa(pAdapter->dev, command);
   }
   else if (strncmp(command, "P2P_SET_PS", 10) == 0)
   {
      hdd_setP2pOpps(pAdapter->dev, command);
   }
   else if (strncmp(command, "MIRACAST", 8) == 0)
   {
       hddLog(VOS_TRACE_LEVEL_INFO, "%s: Received MIRACAST command", __func__);
       ret = hdd_drv_cmd_validate(command, 8);
       if (ret)
           goto exit;

       ret = hdd_set_miracast_mode(pAdapter, command);
   }
exit:
   if (command)
   {
      vos_mem_free(command);
   }
   EXIT();
   return ret;
}

#ifdef CONFIG_COMPAT
static int hdd_hostapd_driver_compat_ioctl(hdd_adapter_t *pAdapter,
                                           struct ifreq *ifr)
{
   struct {
      compat_uptr_t buf;
      int used_len;
      int total_len;
   } compat_priv_data;
   hdd_priv_data_t priv_data;
   int ret = 0;

   /*
    * Note that pAdapter and ifr have already been verified by caller,
    * and HDD context has also been validated
    */
   if (copy_from_user(&compat_priv_data, ifr->ifr_data,
                      sizeof(compat_priv_data))) {
       ret = -EFAULT;
       goto exit;
   }
   priv_data.buf = compat_ptr(compat_priv_data.buf);
   priv_data.used_len = compat_priv_data.used_len;
   priv_data.total_len = compat_priv_data.total_len;
   ret = hdd_hostapd_driver_command(pAdapter, &priv_data);
 exit:
   return ret;
}
#else /* CONFIG_COMPAT */
static int hdd_hostapd_driver_compat_ioctl(hdd_adapter_t *pAdapter,
                                           struct ifreq *ifr)
{
   /* will never be invoked */
   return 0;
}
#endif /* CONFIG_COMPAT */

static int hdd_hostapd_driver_ioctl(hdd_adapter_t *pAdapter, struct ifreq *ifr)
{
   hdd_priv_data_t priv_data;
   int ret = 0;

   /*
    * Note that pAdapter and ifr have already been verified by caller,
    * and HDD context has also been validated
    */
   if (copy_from_user(&priv_data, ifr->ifr_data, sizeof(priv_data))) {
      ret = -EFAULT;
   } else {
      ret = hdd_hostapd_driver_command(pAdapter, &priv_data);
   }
   return ret;
}

/**
 * __hdd_hostapd_ioctl() - hostapd ioctl
 * @dev: pointer to net_device
 * @ifr: pointer to ifreq structure
 * @cmd: command
 *
 * Return; 0 on success, error number otherwise
 */
static int __hdd_hostapd_ioctl(struct net_device *dev,
				struct ifreq *ifr, int cmd)
{
   hdd_adapter_t *pAdapter = WLAN_HDD_GET_PRIV_PTR(dev);
   hdd_context_t *pHddCtx;
   int ret;

   ENTER();

   if (dev != pAdapter->dev) {
      hddLog(VOS_TRACE_LEVEL_ERROR,
             "%s: HDD adapter/dev inconsistency", __func__);
      ret = -ENODEV;
      goto exit;
   }

   if ((!ifr) || (!ifr->ifr_data))
   {
      VOS_TRACE( VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
                FL("ifr or ifr->ifr_data is NULL"));
      ret = -EINVAL;
      goto exit;
   }

   pHddCtx = WLAN_HDD_GET_CTX(pAdapter);
   ret = wlan_hdd_validate_context(pHddCtx);
   if (ret) {
      ret = -EBUSY;
      goto exit;
   }

   switch (cmd) {
   case (SIOCDEVPRIVATE + 1):
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4,6,0)) && defined(CONFIG_X86_64)
      if (in_compat_syscall())
#else
      if (is_compat_task())
#endif
         ret = hdd_hostapd_driver_compat_ioctl(pAdapter, ifr);
      else
         ret = hdd_hostapd_driver_ioctl(pAdapter, ifr);
      break;
   default:
      hddLog(VOS_TRACE_LEVEL_ERROR, "%s: unknown ioctl %d",
             __func__, cmd);
      ret = -EINVAL;
      break;
   }
 exit:
   EXIT();
   return ret;
}

/**
 * hdd_hostapd_ioctl() - SSR wrapper for __hdd_hostapd_ioctl
 * @dev: pointer to net_device
 * @ifr: pointer to ifreq structure
 * @cmd: command
 *
 * Return; 0 on success, error number otherwise
 */
static int hdd_hostapd_ioctl(struct net_device *dev,
				struct ifreq *ifr, int cmd)
{
	int ret;

	vos_ssr_protect(__func__);
	ret = __hdd_hostapd_ioctl(dev, ifr, cmd);
	vos_ssr_unprotect(__func__);

	return ret;
}


#ifdef QCA_HT_2040_COEX
VOS_STATUS hdd_set_sap_ht2040_mode(hdd_adapter_t *pHostapdAdapter,
                                   tANI_U8 channel_type)
{
    eHalStatus halStatus = eHAL_STATUS_FAILURE;
    v_PVOID_t hHal = NULL;

    VOS_TRACE( VOS_MODULE_ID_SAP, VOS_TRACE_LEVEL_INFO_HIGH,
               "%s: change HT20/40 mode", __func__);

    if (WLAN_HDD_SOFTAP == pHostapdAdapter->device_mode) {
        hHal = WLAN_HDD_GET_HAL_CTX(pHostapdAdapter);
        if ( NULL == hHal ) {
            VOS_TRACE( VOS_MODULE_ID_SAP, VOS_TRACE_LEVEL_ERROR,
                       "%s: Hal ctx is null", __func__);
            return VOS_STATUS_E_FAULT;
        }
        halStatus = sme_SetHT2040Mode(hHal, pHostapdAdapter->sessionId,
                                      channel_type, eANI_BOOLEAN_TRUE);
        if (halStatus == eHAL_STATUS_FAILURE ) {
            VOS_TRACE( VOS_MODULE_ID_SAP, VOS_TRACE_LEVEL_ERROR,
                       "%s: Failed to change HT20/40 mode", __func__);
            return VOS_STATUS_E_FAILURE;
        }
    }
    return VOS_STATUS_SUCCESS;
}
#endif

#ifdef FEATURE_WLAN_FORCE_SAP_SCC
/**---------------------------------------------------------------------------
  \brief hdd_restart_softap() -
   Restart SAP  on STA channel to support
   STA + SAP concurrency.

  --------------------------------------------------------------------------*/
void hdd_restart_softap(hdd_context_t *pHddCtx,
                        hdd_adapter_t *pHostapdAdapter)
{
   tHddAvoidFreqList   hdd_avoid_freq_list;

   /* generate vendor specific event */
   vos_mem_zero((void *)&hdd_avoid_freq_list, sizeof(tHddAvoidFreqList));
   hdd_avoid_freq_list.avoidFreqRange[0].startFreq =
        vos_chan_to_freq(pHostapdAdapter->sessionCtx.ap.operatingChannel);
   hdd_avoid_freq_list.avoidFreqRange[0].endFreq =
        vos_chan_to_freq(pHostapdAdapter->sessionCtx.ap.operatingChannel);
   hdd_avoid_freq_list.avoidFreqRangeCount = 1;
   wlan_hdd_send_avoid_freq_event(pHddCtx, &hdd_avoid_freq_list);
}
#endif /* FEATURE_WLAN_FORCE_SAP_SCC */

/**
 * __hdd_hostapd_set_mac_address() - set mac address
 * @dev: pointer to net_device
 * @addr: mac address
 *
 * This function sets the user specified mac address using
 * the command ifconfig wlanX hw ether <mac address>.
 *
 * Return: 0 on success, error number otherwise
 */
static int __hdd_hostapd_set_mac_address(struct net_device *dev, void *addr)
{
   struct sockaddr *psta_mac_addr = addr;
   hdd_adapter_t *adapter;
   hdd_context_t *hdd_ctx;
   int ret = 0;

   ENTER();

   adapter = WLAN_HDD_GET_PRIV_PTR(dev);
   hdd_ctx = WLAN_HDD_GET_CTX(adapter);
   ret = wlan_hdd_validate_context(hdd_ctx);
   if (0 != ret)
       return ret;

   memcpy(dev->dev_addr, psta_mac_addr->sa_data, ETH_ALEN);
   EXIT();
   return 0;
}

/**
 * hdd_hostapd_set_mac_address() - set mac address
 * @dev: pointer to net_device
 * @addr: mac address
 *
 * Return: 0 on success, error number otherwise
 */
static int hdd_hostapd_set_mac_address(struct net_device *dev, void *addr)
{
	int ret;

	vos_ssr_protect(__func__);
	ret = __hdd_hostapd_set_mac_address(dev, addr);
	vos_ssr_unprotect(__func__);

	return ret;
}

void hdd_hostapd_inactivity_timer_cb(v_PVOID_t usrDataForCallback)
{
    struct net_device *dev = (struct net_device *)usrDataForCallback;
    v_BYTE_t we_custom_event[64];
    union iwreq_data wrqu;
#ifdef DISABLE_CONCURRENCY_AUTOSAVE
    VOS_STATUS vos_status;
    hdd_adapter_t *pHostapdAdapter;
    hdd_ap_ctx_t *pHddApCtx;
#endif /*DISABLE_CONCURRENCY_AUTOSAVE */

    /* event_name space-delimiter driver_module_name */
    /* Format of the event is "AUTO-SHUT.indication" " " "module_name" */
    char * autoShutEvent = "AUTO-SHUT.indication" " "  KBUILD_MODNAME;
    int event_len = strlen(autoShutEvent) + 1; /* For the NULL at the end */

    ENTER();

#ifdef DISABLE_CONCURRENCY_AUTOSAVE
    if (vos_concurrent_open_sessions_running())
    {
       /*
              This timer routine is going to be called only when AP
              persona is up.
              If there are concurrent sessions running we do not want
              to shut down the Bss.Instead we run the timer again so
              that if Autosave is enabled next time and other session
              was down only then we bring down AP
             */
        pHostapdAdapter = netdev_priv(dev);
        if ((NULL == pHostapdAdapter) ||
            (WLAN_HDD_ADAPTER_MAGIC != pHostapdAdapter->magic))
        {
            hddLog(LOGE, FL("invalid adapter: %pK"), pHostapdAdapter);
            return;
        }
        pHddApCtx = WLAN_HDD_GET_AP_CTX_PTR(pHostapdAdapter);
        vos_status = vos_timer_start(
         &pHddApCtx->hdd_ap_inactivity_timer,
         (WLAN_HDD_GET_CTX(pHostapdAdapter))->cfg_ini->nAPAutoShutOff
          * 1000);
        if (!VOS_IS_STATUS_SUCCESS(vos_status))
        {
            hddLog(LOGE, FL("Failed to init AP inactivity timer"));
        }
        EXIT();
        return;
    }
#endif /*DISABLE_CONCURRENCY_AUTOSAVE */
    memset(&we_custom_event, '\0', sizeof(we_custom_event));
    memcpy(&we_custom_event, autoShutEvent, event_len);

    memset(&wrqu, 0, sizeof(wrqu));
    wrqu.data.length = event_len;

    hddLog(LOG1, FL("Shutting down AP interface due to inactivity"));
    wireless_send_event(dev, IWEVCUSTOM, &wrqu, (char *)we_custom_event);

    EXIT();
}

#ifdef WLAN_FEATURE_SAP_TO_FOLLOW_STA_CHAN
//This function runs in the timer context of hdd_ap_chan_switch_timer.
void hdd_hostapd_chan_switch_cb(v_PVOID_t usrDataForCallback)
{
    hdd_adapter_t   *pHostapdAdapter = NULL;
    hdd_context_t   *pHddCtx = NULL;
    int             ret = 0;

    ENTER();

    if(usrDataForCallback)
    {
        pHostapdAdapter = (struct hdd_adapter_s *)usrDataForCallback;
    }
    else
    {
        hddLog(LOGE, FL("hdd_hostapd_chan_switch_cb NULL cb pointer!!\n"));
                EXIT();
        return;
    }
    pHddCtx = WLAN_HDD_GET_CTX(pHostapdAdapter);

    mutex_lock(&pHddCtx->ch_switch_ctx.sap_ch_sw_lock);
    if(pHddCtx->ch_switch_ctx.sap_chan_sw_pending)
    {
        vos_ssr_protect(__func__);
        ret = hdd_softap_set_channel_change(pHostapdAdapter->dev, pHddCtx->ch_switch_ctx.def_csa_channel_on_disc);
        vos_ssr_unprotect(__func__);
        if (ret)
        {
            hddLog(LOGE, FL("hdd_softap_set_channel_change failed!!"));
        }
        pHddCtx->ch_switch_ctx.sap_chan_sw_pending = 0;
    }
    mutex_unlock(&pHddCtx->ch_switch_ctx.sap_ch_sw_lock);

    EXIT();
}
#endif //#ifdef WLAN_FEATURE_SAP_TO_FOLLOW_STA_CHAN

static VOS_STATUS
hdd_change_mcc_go_beacon_interval(hdd_adapter_t *pHostapdAdapter)
{
    eHalStatus halStatus = eHAL_STATUS_FAILURE;
    v_PVOID_t hHal = NULL;

    VOS_TRACE( VOS_MODULE_ID_SAP, VOS_TRACE_LEVEL_INFO_HIGH,
               "%s: UPDATE Beacon Params", __func__);

    if(VOS_STA_SAP_MODE == vos_get_conparam ( )){
        hHal = WLAN_HDD_GET_HAL_CTX(pHostapdAdapter);
        if ( NULL == hHal ){
            VOS_TRACE( VOS_MODULE_ID_SAP, VOS_TRACE_LEVEL_ERROR,
                       "%s: Hal ctx is null", __func__);
            return VOS_STATUS_E_FAULT;
        }
        halStatus = sme_ChangeMCCBeaconInterval(hHal, pHostapdAdapter->sessionId);
        if(halStatus == eHAL_STATUS_FAILURE ){
            VOS_TRACE( VOS_MODULE_ID_SAP, VOS_TRACE_LEVEL_ERROR,
                       "%s: Failed to update Beacon Params", __func__);
            return VOS_STATUS_E_FAILURE;
        }
    }
    return VOS_STATUS_SUCCESS;
}

void hdd_clear_all_sta(hdd_adapter_t *pHostapdAdapter, v_PVOID_t usrDataForCallback)
{
    v_U8_t staId = 0;
    struct net_device *dev;
    struct tagCsrDelStaParams delStaParams;
    dev = (struct net_device *)usrDataForCallback;

    hddLog(LOGE, FL("Clearing all the STA entry...."));
    for (staId = 0; staId < WLAN_MAX_STA_COUNT; staId++)
    {
        if ( pHostapdAdapter->aStaInfo[staId].isUsed &&
           ( staId != (WLAN_HDD_GET_AP_CTX_PTR(pHostapdAdapter))->uBCStaId))
        {
             WLANSAP_PopulateDelStaParams(&pHostapdAdapter->aStaInfo[staId].macAddrSTA.bytes[0],
                                                    eSIR_MAC_DEAUTH_LEAVING_BSS_REASON,
                                                    (SIR_MAC_MGMT_DISASSOC >> 4),
                                                     &delStaParams);
            //Disconnect all the stations
            hdd_softap_sta_disassoc(pHostapdAdapter, &delStaParams);
        }
    }
}

static int hdd_stop_bss_link(hdd_adapter_t *pHostapdAdapter,
                             v_PVOID_t usrDataForCallback)
{
    struct net_device *dev;
    hdd_context_t     *pHddCtx = NULL;
    VOS_STATUS status = VOS_STATUS_SUCCESS;
    dev = (struct net_device *)usrDataForCallback;

    ENTER();

    pHddCtx = WLAN_HDD_GET_CTX(pHostapdAdapter);
    status = wlan_hdd_validate_context(pHddCtx);
    if (0 != status)
        return status;

    if(test_bit(SOFTAP_BSS_STARTED, &pHostapdAdapter->event_flags))
    {
#ifdef WLAN_FEATURE_MBSSID
        status = WLANSAP_StopBss(WLAN_HDD_GET_SAP_CTX_PTR(pHostapdAdapter));
#else
        status = WLANSAP_StopBss((WLAN_HDD_GET_CTX(pHostapdAdapter))->pvosContext);
#endif
        if (VOS_IS_STATUS_SUCCESS(status))
            hddLog(LOGE, FL("Deleting SAP/P2P link!!!!!!"));

        clear_bit(SOFTAP_BSS_STARTED, &pHostapdAdapter->event_flags);
        wlan_hdd_decr_active_session(pHddCtx, pHostapdAdapter->device_mode);
    }
    if (pHddCtx->cfg_ini->apOBSSProtEnabled)
        vos_runtime_pm_allow_suspend(pHddCtx->runtime_context.obss);
    EXIT();
    return (status == VOS_STATUS_SUCCESS) ? 0 : -EBUSY;
}

#ifdef SAP_AUTH_OFFLOAD
void hdd_set_sap_auth_offload(hdd_adapter_t *pHostapdAdapter,
                                     bool enabled)
{
    hdd_context_t *pHddCtx = WLAN_HDD_GET_CTX(pHostapdAdapter);
    struct tSirSapOffloadInfo *sap_offload_info = NULL;

    /* Prepare the request to send to SME */
    sap_offload_info = vos_mem_malloc(sizeof(*sap_offload_info));
    if (NULL == sap_offload_info) {
        hddLog(VOS_TRACE_LEVEL_ERROR,
                  "%s: could not allocate tSirSapOffloadInfo!", __func__);
        return;
    }

    vos_mem_zero(sap_offload_info, sizeof(*sap_offload_info));

    sap_offload_info->vdev_id = pHostapdAdapter->sessionId;
    sap_offload_info->sap_auth_offload_enable =
        pHddCtx->cfg_ini->enable_sap_auth_offload && enabled;
    sap_offload_info->sap_auth_offload_sec_type =
        pHddCtx->cfg_ini->sap_auth_offload_sec_type;
    sap_offload_info->key_len =
        strlen(pHddCtx->cfg_ini->sap_auth_offload_key);

    if (sap_offload_info->sap_auth_offload_enable) {
        if (sap_offload_info->key_len < 8 ||
            sap_offload_info->key_len > WLAN_PSK_STRING_LENGTH) {
            hddLog(VOS_TRACE_LEVEL_ERROR,
                   "%s: invalid key length(%d) of WPA security!", __func__,
                   sap_offload_info->key_len);
            goto end;
        }
    }

    vos_mem_copy(sap_offload_info->key,
                            pHddCtx->cfg_ini->sap_auth_offload_key,
                            sap_offload_info->key_len);
    if (eHAL_STATUS_SUCCESS !=
        sme_set_sap_auth_offload(pHddCtx->hHal, sap_offload_info)) {
        hddLog(VOS_TRACE_LEVEL_ERROR,
                  "%s: sme_set_sap_auth_offload fail!", __func__);
        goto end;
    }

    hddLog(VOS_TRACE_LEVEL_INFO_HIGH,
           "%s: sme_set_sap_auth_offload successfully!", __func__);

end:
    vos_mem_free(sap_offload_info);
    return;
}


/**
 * hdd_set_client_block_info - get client block info from ini file
 * @padapter: hdd adapter pointer
 *
 * This function reads client block related info from ini file, these
 * configurations will be sent to fw through wmi.
 *
 * Return: 0 on success, otherwise error value
 */
int hdd_set_client_block_info(hdd_adapter_t *padapter)
{
	hdd_context_t *phddctx = WLAN_HDD_GET_CTX(padapter);
	struct sblock_info client_block_info;
	eHalStatus status;

	/* prepare the request to send to SME */
	client_block_info.vdev_id = padapter->sessionId;
	client_block_info.reconnect_cnt =
				phddctx->cfg_ini->connect_fail_count;

	client_block_info.con_fail_duration =
				phddctx->cfg_ini->connect_fail_duration;

	client_block_info.block_duration =
				phddctx->cfg_ini->connect_block_duration;

	status = sme_set_client_block_info(phddctx->hHal, &client_block_info);
	if (eHAL_STATUS_FAILURE == status) {
		hddLog(VOS_TRACE_LEVEL_ERROR,
			"%s: sme_set_client_block_info!", __func__);
		return -EIO;
	}

	hddLog(VOS_TRACE_LEVEL_INFO_HIGH,
		"%s: sme_set_client_block_info success!", __func__);

	return 0;
}
#endif /* SAP_AUTH_OFFLOAD */

/**
 * hdd_issue_stored_joinreq() - This function will trigger stations's
 *                              cached connect request to proceed.
 * @hdd_ctx: pointer to hdd context.
 * @sta_adapter: pointer to station adapter.
 *
 * This function will call SME to release station's stored/cached connect
 * request to proceed.
 *
 * Return: none.
 */
static void hdd_issue_stored_joinreq(hdd_adapter_t *sta_adapter,
                              hdd_context_t *hdd_ctx)
{
    tHalHandle hal_handle;
    uint32_t roam_id = 0;

    if (NULL == sta_adapter) {
        hddLog(VOS_TRACE_LEVEL_ERROR,
               FL("Invalid station adapter, ignore issueing join req"));
        return;
    }
    hal_handle = WLAN_HDD_GET_HAL_CTX(sta_adapter);

    if (true ==  hdd_is_sta_connection_pending(hdd_ctx)) {
        MTRACE(vos_trace(VOS_MODULE_ID_HDD,
                         TRACE_CODE_HDD_ISSUE_JOIN_REQ,
                         sta_adapter->sessionId, roam_id));
        if (VOS_STATUS_SUCCESS !=
              sme_issue_stored_joinreq(hal_handle,
                                       &roam_id,
                                       sta_adapter->sessionId)) {
            /* change back to NotAssociated */
            hdd_connSetConnectionState(sta_adapter,
                                       eConnectionState_NotConnected);
        }
        hdd_change_sta_conn_pending_status(hdd_ctx, false);
    }
}

/**
 * hdd_update_chandef() - Function to update channel width and center freq
 * @hostapd_adapter:	hostapd adapter
 * @chandef:		cfg80211 chan def
 * @cb_mode:		chan offset
 *
 * This function will be called to update channel width and center freq
 *
 * Return: None
 */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0)) || defined(WITH_BACKPORTS)
static inline void
hdd_update_chandef(hdd_adapter_t *hostapd_adapter,
		struct cfg80211_chan_def *chandef,
		ePhyChanBondState cb_mode)
{
	uint16_t   ch_width;
	hdd_ap_ctx_t *phdd_ap_ctx;
	uint8_t  center_chan, chan;

	phdd_ap_ctx = WLAN_HDD_GET_AP_CTX_PTR(hostapd_adapter);
	ch_width = phdd_ap_ctx->sapConfig.acs_cfg.ch_width;

	switch (ch_width) {
	case eHT_CHANNEL_WIDTH_20MHZ:
	case eHT_CHANNEL_WIDTH_40MHZ:
		VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_INFO,
			"ch_width %d, won't update", ch_width);
		break;
	case eHT_CHANNEL_WIDTH_80MHZ:
		chan = vos_freq_to_chan(chandef->chan->center_freq);
		chandef->width = NL80211_CHAN_WIDTH_80;

		switch (cb_mode) {
		case PHY_QUADRUPLE_CHANNEL_20MHZ_LOW_40MHZ_CENTERED:
		case PHY_QUADRUPLE_CHANNEL_20MHZ_HIGH_40MHZ_LOW:
			center_chan = chan + 2;
			break;
		case PHY_QUADRUPLE_CHANNEL_20MHZ_LOW_40MHZ_LOW:
			center_chan = chan + 6;
			break;
		case PHY_QUADRUPLE_CHANNEL_20MHZ_LOW_40MHZ_HIGH:
		case PHY_QUADRUPLE_CHANNEL_20MHZ_HIGH_40MHZ_CENTERED:
			center_chan = chan - 2;
			break;
		case PHY_QUADRUPLE_CHANNEL_20MHZ_HIGH_40MHZ_HIGH:
			center_chan = chan - 6;
			break;
		default:
			center_chan = chan;
			break;
		}

		chandef->center_freq1 = vos_chan_to_freq(center_chan);
		break;
	case eHT_CHANNEL_WIDTH_160MHZ:
	default:
		/* Todo, please add related codes if support 160MHZ or others */
		VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
			"unsupport ch_width %d", ch_width);
		break;
	}

}
#else
static inline void
hdd_update_chandef(hdd_adapter_t *hostapd_adapter,
		struct cfg80211_chan_def *chandef,
		ePhyChanBondState cb_mode)
{
}
#endif

/**
 * hdd_chan_change_notify() - Function to notify hostapd about channel change
 * @hostapd_adapter	hostapd adapter
 * @dev:		Net device structure
 * @oper_chan:		New operating channel
 *
 * This function is used to notify hostapd about the channel change
 *
 * Return: Success on intimating userspace
 *
 */
VOS_STATUS hdd_chan_change_notify(hdd_adapter_t *hostapd_adapter,
		struct net_device *dev,
		uint8_t oper_chan)
{
	struct ieee80211_channel *chan;
	struct cfg80211_chan_def chandef;
	enum nl80211_channel_type channel_type;
	eCsrPhyMode phy_mode;
	ePhyChanBondState cb_mode;
	uint32_t freq;
	tHalHandle  hal = WLAN_HDD_GET_HAL_CTX(hostapd_adapter);

	if (NULL == hal) {
		VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
				"%s: hal is NULL", __func__);
		return VOS_STATUS_E_FAILURE;
	}

	freq = vos_chan_to_freq(oper_chan);

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0))
	chan = ieee80211_get_channel(hostapd_adapter->wdev.wiphy, freq);
#else
	chan = __ieee80211_get_channel(hostapd_adapter->wdev.wiphy, freq);
#endif
	if (!chan) {
		VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
				"%s: Invalid input frequency for channel conversion",
				 __func__);
		return VOS_STATUS_E_FAILURE;
	}

#ifdef WLAN_FEATURE_MBSSID
	phy_mode = wlansap_get_phymode(WLAN_HDD_GET_SAP_CTX_PTR(hostapd_adapter));
#else
	phy_mode = wlansap_get_phymode(
			(WLAN_HDD_GET_CTX(hostapd_adapter))->pvosContext);
#endif

	if (oper_chan <= 14)
		cb_mode = sme_GetCBPhyStateFromCBIniValue(
				sme_GetChannelBondingMode24G(hal));
	else
		cb_mode = sme_GetCBPhyStateFromCBIniValue(
				sme_GetChannelBondingMode5G(hal));

	switch (phy_mode) {
	case eCSR_DOT11_MODE_11n:
	case eCSR_DOT11_MODE_11n_ONLY:
	case eCSR_DOT11_MODE_11ac:
	case eCSR_DOT11_MODE_11ac_ONLY:
		if (cb_mode == PHY_SINGLE_CHANNEL_CENTERED)
			channel_type = NL80211_CHAN_HT20;
		else if (cb_mode == PHY_DOUBLE_CHANNEL_HIGH_PRIMARY)
			channel_type = NL80211_CHAN_HT40MINUS;
		else if (cb_mode == PHY_DOUBLE_CHANNEL_LOW_PRIMARY)
			channel_type = NL80211_CHAN_HT40PLUS;
		else
			channel_type = NL80211_CHAN_HT40PLUS;
		break;
	default:
		channel_type = NL80211_CHAN_NO_HT;
		break;
	}

	VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_INFO,
			"%s: phy_mode %d cb_mode %d chann_type %d oper_chan %d",
			__func__, phy_mode, cb_mode, channel_type, oper_chan);

	cfg80211_chandef_create(&chandef, chan, channel_type);

	if ((phy_mode == eCSR_DOT11_MODE_11ac) ||
	    (phy_mode == eCSR_DOT11_MODE_11ac_ONLY))
		hdd_update_chandef(hostapd_adapter, &chandef, cb_mode);

	cfg80211_ch_switch_notify(dev, &chandef);

	return VOS_STATUS_SUCCESS;
}

#ifdef WLAN_FEATURE_SAP_TO_FOLLOW_STA_CHAN
VOS_STATUS hdd_send_sap_event(struct net_device *dev,
                sta_sap_notifications event,
                struct wlan_sap_csa_info csa_info,
                struct wireless_dev *wdev)
{
    uint32_t freq = 0, ret;

    hdd_wlan_get_freq(csa_info.sta_channel, &freq);

    hddLog(LOG1, FL(" Set Freq %d Chan= %d"), freq, csa_info.sta_channel );

    vos_ssr_protect(__func__);
    ret = hdd_softap_set_channel_change(dev, csa_info.sta_channel);
    vos_ssr_unprotect(__func__);

    return ret;
}

VOS_STATUS hdd_sta_state_sap_notify(hdd_context_t *hdd_context,
                                sta_sap_notifications event,
                                struct wlan_sap_csa_info csa_info)
{
    /* Get the HostApd Adapter. If present proceed further.
     * Check the current state of SAP. If its in active state, get the channel in which it is running.
     * Verify the channel and band. Based on the event type, take a decision.
     * If it is a disconnection event and SAP is running in 2.4 band channel, no action should be taken.
     * If its a connection event and SAP needs to do a CSA to the HomeAP channel.
     */

    hdd_adapter_t *pHostapdAdapter = NULL;
    hdd_ap_ctx_t *pHddApCtx = NULL;
    hdd_hostapd_state_t *pHostapdState = NULL;
    VOS_STATUS status = VOS_STATUS_SUCCESS;
    tsap_Config_t *sap_config;
    uint32_t ret = 0;

    hddLog(LOGE, FL("%s Entry event = %d channel = %d"),
                        __func__, event, csa_info.sta_channel);

    if (!hdd_context) {
        hddLog(LOGE, FL("HDD context is NULL"));
                return VOS_STATUS_E_FAILURE;
    }

    ret = wlan_hdd_validate_context(hdd_context);

    if (ret != 0) {

        hddLog(LOGE, FL("%s Failed in hdd_validate_context ret=%d"), __func__, ret);
        return ret;
    }

    /*Get the Adapter of SAP*/
    pHostapdAdapter = hdd_get_adapter(hdd_context, WLAN_HDD_SOFTAP);

    if(!pHostapdAdapter)
    {
        hddLog(LOGE, FL("Hostapd adapter context is NULL"));
        return VOS_STATUS_E_FAILURE;
    }

    pHddApCtx = WLAN_HDD_GET_AP_CTX_PTR(pHostapdAdapter);

    pHostapdState = WLAN_HDD_GET_HOSTAP_STATE_PTR(pHostapdAdapter);

    /*Verify the state*/
    if(pHostapdState->vosStatus != VOS_STATUS_SUCCESS ||
            pHostapdState->bssState != BSS_START)
    {
        hddLog(LOGE, FL("Invalid HostApd State vosStatus=%d bssState=%d"),
                    pHostapdState->vosStatus, pHostapdState->bssState);
        return VOS_STATUS_E_FAILURE;
    }

    switch(event)
    {
        case STA_NOTIFY_DISCONNECTED:
            {
                /* check for the operating channel
                 * If operating in 2.4, just ignore and return
                 * else start ACS & find the strongest signal channel and do initiate CSA to that channel.
                 */
                if((pHddApCtx->operatingChannel >= 1 && pHddApCtx->operatingChannel <= 14))
                {
                    hddLog(LOGE, FL("Hostapd is operating in 2.4Band Channel=%d, Avoid channel switch"),
                                        pHddApCtx->operatingChannel);
                }
                else
                {
		    sap_config = &((WLAN_HDD_GET_AP_CTX_PTR(pHostapdAdapter))->sapConfig);
		    if (VOS_IS_DFS_CH(pHddApCtx->operatingChannel) &&
				( VOS_IS_DFS_CH(sap_config->channel) ||
					(sap_config->channel == AUTO_CHANNEL_SELECT) )){
			hddLog(LOGE, FL("SAP CUR CH %d(DFS) Hostapd Conf CH=%d(%s) Switch to CH %d"),
					pHddApCtx->operatingChannel, pHddApCtx->operatingChannel,
				        (sap_config->channel == AUTO_CHANNEL_SELECT)?"AUTO":"DFS", 36);
                        hdd_context->ch_switch_ctx.def_csa_channel_on_disc = 36;
		    }else if (VOS_IS_DFS_CH(pHddApCtx->operatingChannel) &&
				!VOS_IS_DFS_CH(sap_config->channel)){
			hddLog(LOGE, FL("SAP CUR CH %d(DFS) Hostapd Conf CH=%d(Non-DFS) Switch to %d"),
					pHddApCtx->operatingChannel, sap_config->channel, sap_config->channel);
			hdd_context->ch_switch_ctx.def_csa_channel_on_disc = sap_config->channel; //channel from the hostapd
		    }else{
			    hddLog(LOGE, FL("SAP is operating in 5Ghz Band Non DFS Channel=%d, Avoid channel switch"),
					    pHddApCtx->operatingChannel);
			    return status;
		    }
		    mutex_lock(&hdd_context->ch_switch_ctx.sap_ch_sw_lock);
		    hdd_context->ch_switch_ctx.sap_chan_sw_pending = 1;
		    mutex_unlock(&hdd_context->ch_switch_ctx.sap_ch_sw_lock);

		    //Set the timer to initiate channel switch
		    if(hdd_context->ch_switch_ctx.chan_sw_timer_initialized == VOS_TRUE)
		    {
			    status = vos_timer_start(&hdd_context->ch_switch_ctx.hdd_ap_chan_switch_timer, 10000);
			    if(!VOS_IS_STATUS_SUCCESS(status))
			    {
				    hddLog(LOGE, FL("Failed to start AP channel switch timer!!"));
				    break;
			    }
		    }
		}
	    }
	    break;
	case STA_NOTIFY_CONNECTED:
	    {
		    //stop the channel switch timer first
		    if (hdd_context->ch_switch_ctx.hdd_ap_chan_switch_timer.state == VOS_TIMER_STATE_RUNNING)
		    {
			    status = vos_timer_stop(&hdd_context->ch_switch_ctx.hdd_ap_chan_switch_timer);
			    if(!VOS_IS_STATUS_SUCCESS(status))
			    {
				    hddLog(LOGE, FL("Failed to stop AP channel switch timer!!"));
				    break;
			    }
		    }
		    if(pHddApCtx->operatingChannel != csa_info.sta_channel)
		    {
			    mutex_lock(&hdd_context->ch_switch_ctx.sap_ch_sw_lock);
			    hddLog(LOGE, FL("Switching Hostapd to Station channel %d"), csa_info.sta_channel);
			    status = hdd_send_sap_event(pHostapdAdapter->dev,
					    event,
					    csa_info,
					    &pHostapdAdapter->wdev);
			    if(!VOS_IS_STATUS_SUCCESS(status))
			    {
				    hddLog(LOGE, FL("Failed to send channel switch event!!"));
				    mutex_unlock(&hdd_context->ch_switch_ctx.sap_ch_sw_lock);
				    break;
			    }
			    hdd_context->ch_switch_ctx.sap_chan_sw_pending = 0;
			    mutex_unlock(&hdd_context->ch_switch_ctx.sap_ch_sw_lock);
		    }
		    else
		    {
			    hddLog(LOGE, FL("Hostapd and Sta are operating in same channel : %d\n"),
					    pHddApCtx->operatingChannel);
		    }
	    }
	    break;
	case STA_NOTIFY_CSA:
	    {
		    mutex_lock(&hdd_context->ch_switch_ctx.sap_ch_sw_lock);
		    if(pHddApCtx->operatingChannel != csa_info.sta_channel)
		    {
			    if(!(hdd_context->ch_switch_ctx.is_ch_sw_through_sta_csa &&
						    hdd_context->ch_switch_ctx.csa_to_channel == csa_info.sta_channel))
			    {
				    hdd_context->ch_switch_ctx.is_ch_sw_through_sta_csa = VOS_TRUE;

				    hddLog(LOGE, FL("Switching Hostapd to Station channel %d"), csa_info.sta_channel);
				    status = hdd_send_sap_event(pHostapdAdapter->dev,
						    event,
						    csa_info,
						    &pHostapdAdapter->wdev);
				    if(!VOS_IS_STATUS_SUCCESS(status))
				    {
					    hddLog(LOGE, FL("Failed to send channel switch event!!"));
					    mutex_unlock(&hdd_context->ch_switch_ctx.sap_ch_sw_lock);
					    break;
				    }

				    hdd_context->ch_switch_ctx.csa_to_channel = csa_info.sta_channel;
			    }
			    else
			    {
				    VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_INFO,
						    "%s : CSA Sta interface for Channel %d is already notified",
						    __func__, csa_info.sta_channel);
			    }
		    }
		    else
		    {
			    hddLog(LOGE, FL("Hostapd and Sta are operating in same channel : %d\n"),
					    pHddApCtx->operatingChannel);
		    }
		    mutex_unlock(&hdd_context->ch_switch_ctx.sap_ch_sw_lock);
	    }
	    break;
	default:
	    {
		    hddLog(LOGE, FL("%s Invalid event %d"), __func__, event);
	    }
	    break;
    }

    hddLog(LOGE, FL("%s Exit ret = %d"), __func__, status);

    return status;
}
#endif //#ifdef WLAN_FEATURE_SAP_TO_FOLLOW_STA_CHAN
/**
 * hdd_send_radar_event() - Function to send radar events to user space
 * @hdd_context:	HDD context
 * @event:		Type of radar event
 * @dfs_info:		Structure containing DFS channel and country
 * @wdev:		Wireless device structure
 *
 * This function is used to send radar events such as CAC start, CAC
 * end etc., to userspace
 *
 * Return: Success on sending notifying userspace
 *
 */
VOS_STATUS hdd_send_radar_event(hdd_context_t *hdd_context,
				eSapHddEvent event,
				struct wlan_dfs_info dfs_info,
				struct wireless_dev *wdev)
{

	struct sk_buff *vendor_event;
	enum qca_nl80211_vendor_subcmds_index index;
	uint32_t freq, ret;
	uint32_t data_size;

	if (!hdd_context) {
		hddLog(LOGE, FL("HDD context is NULL"));
                return VOS_STATUS_E_FAILURE;
	}

	freq = vos_chan_to_freq(dfs_info.channel);

	switch (event) {
	case eSAP_DFS_CAC_START:
	    index =
		QCA_NL80211_VENDOR_SUBCMD_DFS_OFFLOAD_CAC_STARTED_INDEX;
	    data_size = sizeof(uint32_t);
	    break;
	case eSAP_DFS_CAC_END:
	    index =
		QCA_NL80211_VENDOR_SUBCMD_DFS_OFFLOAD_CAC_FINISHED_INDEX;
	    data_size = sizeof(uint32_t);
	    break;
	case eSAP_DFS_RADAR_DETECT:
	    index =
		QCA_NL80211_VENDOR_SUBCMD_DFS_OFFLOAD_RADAR_DETECTED_INDEX;
	    data_size = sizeof(uint32_t);
	    break;
	default:
	    return VOS_STATUS_E_FAILURE;
	}

	vendor_event = cfg80211_vendor_event_alloc(hdd_context->wiphy,
				wdev,
				data_size + NLMSG_HDRLEN,
				index,
				GFP_KERNEL);
	if (!vendor_event) {
		hddLog(LOGE,
		       FL("cfg80211_vendor_event_alloc failed for %d"), index);
		return VOS_STATUS_E_FAILURE;
	}

	ret = nla_put_u32(vendor_event, NL80211_ATTR_WIPHY_FREQ, freq);

	if (ret) {
		hddLog(LOGE, FL("NL80211_ATTR_WIPHY_FREQ put fail"));
		kfree_skb(vendor_event);
		return VOS_STATUS_E_FAILURE;
	}

	cfg80211_vendor_event(vendor_event, GFP_KERNEL);
	return VOS_STATUS_SUCCESS;
}

#ifdef CONFIG_CNSS
static VOS_STATUS hdd_wlan_get_dfs_nol(void *pdfs_list, u16 sdfs_list)
{
	int ret;

	/* get the dfs nol from cnss */
	ret = vos_wlan_get_dfs_nol(pdfs_list, sdfs_list);
	if (ret > 0) {
		hddLog(VOS_TRACE_LEVEL_INFO_HIGH,
			"%s: Get %d bytes of dfs nol from cnss",
			__func__, ret);
		return VOS_STATUS_SUCCESS;
	} else {
		hddLog(VOS_TRACE_LEVEL_INFO_HIGH,
			"%s: No dfs nol entry in CNSS, ret: %d",
			__func__, ret);
		return VOS_STATUS_E_FAULT;
	}
}
#else
static VOS_STATUS hdd_wlan_get_dfs_nol(void *pdfs_list, u16 sdfs_list)
{
	return VOS_STATUS_E_FAILURE;
}
#endif

#ifdef CONFIG_CNSS
static VOS_STATUS hdd_wlan_set_dfs_nol(const void *pdfs_list, u16 sdfs_list)
{
	int ret;

	/* set the dfs nol from cnss */
	ret = vos_wlan_set_dfs_nol(pdfs_list, sdfs_list);
	if (ret) {
		hddLog(VOS_TRACE_LEVEL_INFO_HIGH,
			"%s: Failed to set dfs nol - ret: %d",
			__func__, ret);
	} else {
		hddLog(VOS_TRACE_LEVEL_INFO_HIGH,
			"%s: Set %d bytes dfs nol to cnss",
			__func__, sdfs_list);
	}

	return VOS_STATUS_SUCCESS;
}
#else
static VOS_STATUS hdd_wlan_set_dfs_nol(const void *pdfs_list, u16 sdfs_list)
{
	return VOS_STATUS_E_FAILURE;
}
#endif

#ifdef FEATURE_WLAN_AP_AP_ACS_OPTIMIZE
/**
 * hdd_handle_acs_scan_event() - handle acs scan event for SAP
 * @sap_event: tpSap_Event
 * @adapter: hdd_adapter_t for SAP
 *
 * The function is to handle the eSAP_ACS_SCAN_SUCCESS_EVENT event.
 * It will update scan result to cfg80211 and start a timer to flush the
 * cached acs scan result.
 *
 * Return: VOS_STATUS_SUCCESS on success,
          other value on failure
 */
static VOS_STATUS hdd_handle_acs_scan_event(tpSap_Event sap_event,
		hdd_adapter_t *adapter)
{
	hdd_context_t *hdd_ctx;
	struct tsap_acs_scan_complete_event *comp_evt;
	VOS_STATUS vos_status;
	int chan_list_size;

	hdd_ctx = (hdd_context_t*)(adapter->pHddCtx);
	if (!hdd_ctx) {
		hddLog(VOS_TRACE_LEVEL_ERROR, FL("HDD context is null"));
		return VOS_STATUS_E_FAILURE;
	}
	comp_evt = &sap_event->sapevt.sap_acs_scan_comp;
	hdd_ctx->skip_acs_scan_status = eSAP_SKIP_ACS_SCAN;
	spin_lock(&hdd_ctx->acs_skip_lock);
	vos_mem_free(hdd_ctx->last_acs_channel_list);
	hdd_ctx->last_acs_channel_list = NULL;
	hdd_ctx->num_of_channels = 0;
	/* cache the previous ACS scan channel list .
	 * If the following OBSS scan chan list is covered by ACS chan list,
	 * we can skip OBSS Scan to save SAP starting total time.
	 */
	if (comp_evt->num_of_channels && comp_evt->channellist) {
		chan_list_size = comp_evt->num_of_channels *
			sizeof(comp_evt->channellist[0]);
		hdd_ctx->last_acs_channel_list = vos_mem_malloc(
			chan_list_size);
		if (hdd_ctx->last_acs_channel_list) {
			vos_mem_copy(hdd_ctx->last_acs_channel_list,
				comp_evt->channellist,
				chan_list_size);
			hdd_ctx->num_of_channels = comp_evt->num_of_channels;
		}
	}
	spin_unlock(&hdd_ctx->acs_skip_lock);
	/* Update ACS scan result to cfg80211. Then OBSS scan can reuse the
	 * scan result.
	 */
	if (wlan_hdd_cfg80211_update_bss(hdd_ctx->wiphy, adapter))
		hddLog(VOS_TRACE_LEVEL_INFO, FL("NO SCAN result"));

	hddLog(LOG1, FL("Reusing Last ACS scan result for %d sec"),
		ACS_SCAN_EXPIRY_TIMEOUT_S);
	vos_timer_stop( &hdd_ctx->skip_acs_scan_timer);
	vos_status = vos_timer_start( &hdd_ctx->skip_acs_scan_timer,
			ACS_SCAN_EXPIRY_TIMEOUT_S * 1000);
	if (!VOS_IS_STATUS_SUCCESS(vos_status))
		hddLog(LOGE, FL("Failed to start ACS scan expiry timer"));
	return VOS_STATUS_SUCCESS;
}
#else
static VOS_STATUS hdd_handle_acs_scan_event(tpSap_Event sap_event,
		hdd_adapter_t *adapter)
{
	return VOS_STATUS_SUCCESS;
}
#endif

/**
 * get_max_rate_vht() - calculate max rate for VHT mode
 * @nss: num of streams
 * @ch_width: channel width
 * @sgi: short gi
 * @vht_mcs_map: vht mcs map
 *
 * This function calculate max rate for VHT mode
 *
 * Return: max rate
 */
#ifdef WLAN_FEATURE_11AC
static int get_max_rate_vht(int nss, int ch_width, int sgi, int vht_mcs_map)
{
	struct index_vht_data_rate_type *supported_vht_mcs_rate;
	enum data_rate_11ac_max_mcs vht_max_mcs;
	int maxrate = 0;
	int maxidx = 0;

	if (nss == 1) {
		supported_vht_mcs_rate = supported_vht_mcs_rate_nss1;
	} else if (nss == 2) {
		supported_vht_mcs_rate = supported_vht_mcs_rate_nss2;
	} else {
		/* Not Supported */
		hddLog(VOS_TRACE_LEVEL_ERROR,
				"%s: nss %d not supported\n",
				__func__, nss);
		return maxrate;
	}

	vht_max_mcs =
		(enum data_rate_11ac_max_mcs)
		vht_mcs_map & DATA_RATE_11AC_MCS_MASK;

	if (vht_max_mcs == DATA_RATE_11AC_MAX_MCS_7) {
		maxidx = 7;
	} else if (vht_max_mcs == DATA_RATE_11AC_MAX_MCS_8) {
		maxidx = 8;
	} else if (vht_max_mcs == DATA_RATE_11AC_MAX_MCS_9) {
		if (ch_width == eHT_CHANNEL_WIDTH_20MHZ)
			/* MCS9 is not valid for VHT20 when nss=1,2 */
			maxidx = 8;
		else
			maxidx = 9;
	} else {
		hddLog(VOS_TRACE_LEVEL_ERROR,
				"%s: vht mcs map %x not supported\n",
				__func__,
				vht_mcs_map & DATA_RATE_11AC_MCS_MASK);
		return maxrate;
	}

	if (ch_width == eHT_CHANNEL_WIDTH_20MHZ) {
		maxrate =
		supported_vht_mcs_rate[maxidx].supported_VHT20_rate[sgi];
	} else if (ch_width == eHT_CHANNEL_WIDTH_40MHZ) {
		maxrate =
		supported_vht_mcs_rate[maxidx].supported_VHT40_rate[sgi];
	} else if (ch_width == eHT_CHANNEL_WIDTH_80MHZ) {
		maxrate =
		supported_vht_mcs_rate[maxidx].supported_VHT80_rate[sgi];
	} else {
		hddLog(VOS_TRACE_LEVEL_ERROR,
				"%s: ch_width %d not supported\n",
				__func__, ch_width);
		return maxrate;
	}

	return maxrate;
}
#else
static int get_max_rate_vht(int nss, int ch_width, int sgi, int vht_mcs_map)
{
	return 0;
}
#endif

/**
 * calculate_max_phy_rate() - calcuate maximum phy rate (kbps)
 * @mode: phymode: Legacy, 11a/b/g, HT, VHT
 * @nss: num of stream (maximum num is 2)
 * @ch_width: channel width
 * @sgi: short gi enabled or not
 * @supp_idx: max supported idx
 * @ext_idx: max extended idx
 * @ht_mcs_idx: max mcs index for HT
 * @vht_mcs_map: mcs map for VHT
 *
 * return: maximum phy rate in kbps
 * */
static int calcuate_max_phy_rate(int mode, int nss, int ch_width,
		int sgi, int supp_idx, int ext_idx, int ht_mcs_idx,
		int vht_mcs_map)
{
	struct index_data_rate_type *supported_mcs_rate;
	int maxidx = 12; /*default 6M mode*/
	int maxrate = 0, tmprate = 0;
	int i;

	/* check supported rates */
	if (supp_idx != 0xff && maxidx < supp_idx)
		maxidx = supp_idx;

	/* check extended rates */
	if (ext_idx != 0xff && maxidx < ext_idx)
		maxidx = ext_idx;

	for (i = 0;
		i < sizeof(supported_data_rate)/sizeof(supported_data_rate[0]);
		i++) {
		if (supported_data_rate[i].beacon_rate_index == maxidx)
			maxrate = supported_data_rate[i].supported_rate[0];
	}

	if (mode == SIR_SME_PHY_MODE_HT) {
		/* check for HT Mode */
		maxidx = ht_mcs_idx;
		if (nss == 1) {
			supported_mcs_rate = supported_mcs_rate_nss1;
		} else if (nss == 2) {
			supported_mcs_rate = supported_mcs_rate_nss2;
		} else {
			/* Not Supported */
			hddLog(VOS_TRACE_LEVEL_ERROR,
					"%s: nss %d not supported\n",
					__func__, nss);
			return maxrate;
		}

		if (ch_width == eHT_CHANNEL_WIDTH_20MHZ) {
			tmprate =
				supported_mcs_rate[maxidx].supported_rate[0];
			if (sgi)
			    tmprate =
				supported_mcs_rate[maxidx].supported_rate[2];
		} else if (ch_width == eHT_CHANNEL_WIDTH_40MHZ) {
			tmprate =
				supported_mcs_rate[maxidx].supported_rate[1];
			if (sgi)
			    tmprate =
				supported_mcs_rate[maxidx].supported_rate[3];
		} else {
			hddLog(VOS_TRACE_LEVEL_ERROR,
					"%s: invalid mode %d ch_width %d\n",
					__func__, mode, ch_width);
			return maxrate;
		}

		if (maxrate < tmprate)
			maxrate = tmprate;
	}

	if (mode == SIR_SME_PHY_MODE_VHT) {
		/* check for VHT Mode */
		tmprate = get_max_rate_vht(nss, ch_width, sgi, vht_mcs_map);
		if (maxrate < tmprate)
			maxrate = tmprate;
	}

	return maxrate;
}

static void hdd_fill_station_info(hdd_station_info_t *stainfo,
		tSap_StationAssocReassocCompleteEvent *event)
{
	stainfo->nss = event->chan_info.nss;
	stainfo->rate_flags = event->chan_info.rate_flags;
	stainfo->sub20_dynamic_channelwidth =
		event->chan_info.sub20_channelwidth;
	stainfo->ampdu = event->ampdu;
	stainfo->sgi_enable = event->sgi_enable;
	stainfo->tx_stbc = event->tx_stbc;
	stainfo->rx_stbc = event->rx_stbc;
	stainfo->ch_width = event->ch_width;
	stainfo->mode = event->mode;
	stainfo->max_supp_idx = event->max_supp_idx;
	stainfo->max_ext_idx = event->max_ext_idx;
	stainfo->max_mcs_idx = event->max_mcs_idx;
	stainfo->rx_mcs_map = event->rx_mcs_map;
	stainfo->tx_mcs_map = event->tx_mcs_map;
	stainfo->assoc_ts = vos_system_ticks();
	stainfo->max_phy_rate =
		calcuate_max_phy_rate(stainfo->mode,
				stainfo->nss,
				stainfo->ch_width,
				stainfo->sgi_enable,
				stainfo->max_supp_idx,
				stainfo->max_ext_idx,
				stainfo->max_mcs_idx,
				stainfo->rx_mcs_map);
	/* expect max_phy_rate report in kbps */
	stainfo->max_phy_rate *= 100;
	hddLog(VOS_TRACE_LEVEL_INFO,
			FL("cap %d %d %d %d %d %d %d %d %d %x %d"),
			stainfo->ampdu,
			stainfo->sgi_enable,
			stainfo->tx_stbc,
			stainfo->rx_stbc,
			stainfo->isQosEnabled,
			stainfo->ch_width,
			stainfo->mode,
			event->wmmEnabled,
			event->chan_info.nss,
			event->chan_info.rate_flags,
			stainfo->max_phy_rate);
	hddLog(VOS_TRACE_LEVEL_INFO,
			FL("rate info %d %d %d %d %d"),
			stainfo->max_supp_idx,
			stainfo->max_ext_idx,
			stainfo->max_mcs_idx,
			stainfo->rx_mcs_map,
			stainfo->tx_mcs_map);
}

VOS_STATUS hdd_hostapd_SAPEventCB( tpSap_Event pSapEvent, v_PVOID_t usrDataForCallback)
{
    hdd_adapter_t *pHostapdAdapter;
    hdd_ap_ctx_t *pHddApCtx;
    hdd_hostapd_state_t *pHostapdState;
    struct net_device *dev;
    eSapHddEvent sapEvent;
    union iwreq_data wrqu;
    v_BYTE_t *we_custom_event_generic = NULL;
    int we_event = 0;
    int i = 0;
    v_U8_t staId;
    VOS_STATUS vos_status;
    v_BOOL_t bWPSState;
    v_BOOL_t bAuthRequired = TRUE;
    tpSap_AssocMacAddr pAssocStasArray = NULL;
    char unknownSTAEvent[IW_CUSTOM_MAX+1];
    char maxAssocExceededEvent[IW_CUSTOM_MAX+1];
    v_BYTE_t we_custom_start_event[64];
    char *startBssEvent;
    hdd_context_t *pHddCtx;
    hdd_scaninfo_t *pScanInfo  = NULL;
    struct iw_michaelmicfailure msg;
    v_U8_t ignoreCAC = 0;
    hdd_config_t *cfg = NULL;
    struct wlan_dfs_info dfs_info;
    v_U8_t cc_len = WLAN_SVC_COUNTRY_CODE_LEN;
    hdd_adapter_t *con_sap_adapter;
    tSap_StationAssocReassocCompleteEvent *event;
    VOS_STATUS status = VOS_STATUS_SUCCESS;

    dev = (struct net_device *)usrDataForCallback;
    if (!dev)
    {
        VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
                "%s: usrDataForCallback is null", __func__);
        return VOS_STATUS_E_FAILURE;
    }

    pHostapdAdapter = netdev_priv(dev);

    if ((NULL == pHostapdAdapter) ||
        (WLAN_HDD_ADAPTER_MAGIC != pHostapdAdapter->magic))
    {
        VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_FATAL,
                "invalid adapter or adapter has invalid magic");
        return VOS_STATUS_E_FAILURE;
    }

    pHostapdState = WLAN_HDD_GET_HOSTAP_STATE_PTR(pHostapdAdapter);
    pHddApCtx = WLAN_HDD_GET_AP_CTX_PTR(pHostapdAdapter);

    if (!pSapEvent)
    {
        VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
                "%s: pSapEvent is null", __func__);
        return VOS_STATUS_E_FAILURE;
    }

    sapEvent = pSapEvent->sapHddEventCode;
    memset(&wrqu, '\0', sizeof(wrqu));
    pHddCtx = (hdd_context_t*)(pHostapdAdapter->pHddCtx);

    if (!pHddCtx) {
        hddLog(VOS_TRACE_LEVEL_ERROR, FL("HDD context is null"));
        return VOS_STATUS_E_FAILURE;
    }

    cfg = pHddCtx->cfg_ini;

    if (!cfg) {
        hddLog(VOS_TRACE_LEVEL_ERROR, FL("HDD config is null"));
        return VOS_STATUS_E_FAILURE;
    }

    dfs_info.channel = pHddApCtx->operatingChannel;
    sme_GetCountryCode(pHddCtx->hHal, dfs_info.country_code, &cc_len);

    switch(sapEvent)
    {
        case eSAP_START_BSS_EVENT :
            hddLog(LOG1, FL("BSS configured status = %s, channel = %u, bc sta Id = %d"),
                            pSapEvent->sapevt.sapStartBssCompleteEvent.status ? "eSAP_STATUS_FAILURE" : "eSAP_STATUS_SUCCESS",
                            pSapEvent->sapevt.sapStartBssCompleteEvent.operatingChannel,
                              pSapEvent->sapevt.sapStartBssCompleteEvent.staId);

            pHostapdAdapter->sessionId =
                    pSapEvent->sapevt.sapStartBssCompleteEvent.sessionId;

            pHostapdState->vosStatus = pSapEvent->sapevt.sapStartBssCompleteEvent.status;
            vos_status = vos_event_set(&pHostapdState->vosEvent);

            if (!VOS_IS_STATUS_SUCCESS(vos_status) || pHostapdState->vosStatus)
            {
                VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR, ("ERROR: startbss event failed!!"));
                goto stopbss;
            }
            else
            {
#ifdef FEATURE_WLAN_CH_AVOID
                sme_ChAvoidUpdateReq(pHddCtx->hHal);
#endif /* FEATURE_WLAN_CH_AVOID */

                pHddApCtx->uBCStaId = pSapEvent->sapevt.sapStartBssCompleteEvent.staId;

#ifdef QCA_LL_TX_FLOW_CT
                if (pHostapdAdapter->tx_flow_timer_initialized == VOS_FALSE)
                {
                    vos_timer_init(&pHostapdAdapter->tx_flow_control_timer,
                                   VOS_TIMER_TYPE_SW,
                                   hdd_tx_resume_timer_expired_handler,
                                   pHostapdAdapter);
                    pHostapdAdapter->tx_flow_timer_initialized = VOS_TRUE;
                }
                WLANTL_RegisterTXFlowControl(pHddCtx->pvosContext,
                                             hdd_tx_resume_cb,
                                             pHostapdAdapter->sessionId,
                                             (void *)pHostapdAdapter);
#endif

                //@@@ need wep logic here to set privacy bit
                vos_status = hdd_softap_Register_BC_STA(pHostapdAdapter, pHddApCtx->uPrivacy);
                if (!VOS_IS_STATUS_SUCCESS(vos_status)) {
                    hddLog(LOGW, FL("Failed to register BC STA %d"), vos_status);
                    hdd_stop_bss_link(pHostapdAdapter, usrDataForCallback);
                }
            }
#ifdef IPA_OFFLOAD
            if (hdd_ipa_is_enabled(pHddCtx))
            {
                status = hdd_ipa_wlan_evt(pHostapdAdapter, pHddApCtx->uBCStaId,
                        WLAN_AP_CONNECT, pHostapdAdapter->dev->dev_addr);

                if (status)
                {
                    VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
                                     ("ERROR: WLAN_AP_CONNECT event failed!!"));
                    goto stopbss;
                }
            }
#endif

            if (0 != (WLAN_HDD_GET_CTX(pHostapdAdapter))->cfg_ini->nAPAutoShutOff)
            {
                // AP Inactivity timer init and start
                vos_status = vos_timer_init( &pHddApCtx->hdd_ap_inactivity_timer, VOS_TIMER_TYPE_SW,
                                            hdd_hostapd_inactivity_timer_cb, (v_PVOID_t)dev );
                if (!VOS_IS_STATUS_SUCCESS(vos_status))
                   hddLog(LOGE, FL("Failed to init AP inactivity timer"));

                vos_status = vos_timer_start( &pHddApCtx->hdd_ap_inactivity_timer, (WLAN_HDD_GET_CTX(pHostapdAdapter))->cfg_ini->nAPAutoShutOff * 1000);
                if (!VOS_IS_STATUS_SUCCESS(vos_status))
                   hddLog(LOGE, FL("Failed to init AP inactivity timer"));

            }
#ifdef FEATURE_WLAN_AUTO_SHUTDOWN
            wlan_hdd_auto_shutdown_enable(pHddCtx, VOS_TRUE);
#endif
            pHddApCtx->operatingChannel = pSapEvent->sapevt.sapStartBssCompleteEvent.operatingChannel;

            hdd_hostapd_channel_prevent_suspend(pHostapdAdapter,
                    pHddApCtx->operatingChannel);

            pHostapdState->bssState = BSS_START;

            /* Set default key index */
            VOS_TRACE( VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_INFO,
                    "%s: default key index %hu", __func__,
                    pHddApCtx->wep_def_key_idx);

            sme_roam_set_default_key_index(
                    WLAN_HDD_GET_HAL_CTX(pHostapdAdapter),
                    pHostapdAdapter->sessionId,
                    pHddApCtx->wep_def_key_idx);

            //Set group key / WEP key every time when BSS is restarted
            if( pHddApCtx->groupKey.keyLength )
            {
                 status = WLANSAP_SetKeySta(
#ifdef WLAN_FEATURE_MBSSID
                               WLAN_HDD_GET_SAP_CTX_PTR(pHostapdAdapter),
#else
                               (WLAN_HDD_GET_CTX(pHostapdAdapter))->pvosContext,
#endif
                               &pHddApCtx->groupKey);
                 if (!VOS_IS_STATUS_SUCCESS(status))
                 {
                      VOS_TRACE( VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
                             "%s: WLANSAP_SetKeySta failed", __func__);
                 }
            }
            else
            {
                for ( i = 0; i < CSR_MAX_NUM_KEY; i++ )
                {
                    if ( !pHddApCtx->wepKey[i].keyLength )
                          continue;

                    status = WLANSAP_SetKeySta(
#ifdef WLAN_FEATURE_MBSSID
                                WLAN_HDD_GET_SAP_CTX_PTR(pHostapdAdapter),
#else
                                (WLAN_HDD_GET_CTX(pHostapdAdapter))->pvosContext,
#endif
                                &pHddApCtx->wepKey[i]);
                    if (!VOS_IS_STATUS_SUCCESS(status))
                    {
                          VOS_TRACE( VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
                             "%s: WLANSAP_SetKeySta failed idx %d", __func__, i);
                    }
                }
           }

            spin_lock_bh(&pHddCtx->dfs_lock);
            pHddCtx->dfs_radar_found = VOS_FALSE;
            spin_unlock_bh(&pHddCtx->dfs_lock);
            WLANSAP_Get_Dfs_Ignore_CAC(pHddCtx->hHal, &ignoreCAC);
            if ((NV_CHANNEL_DFS !=
                vos_nv_getChannelEnabledState(pHddApCtx->operatingChannel))
                || ignoreCAC
                || pHddCtx->dev_dfs_cac_status == DFS_CAC_ALREADY_DONE)
            {
                pHddApCtx->dfs_cac_block_tx = VOS_FALSE;
            } else {
                /*
                 * DFS requirement: Do not transmit during CAC.
                 * This flag will be reset when BSS starts
                 * (if not in a DFS channel) or CAC ends.
                 */
                pHddApCtx->dfs_cac_block_tx = VOS_TRUE;
            }

            VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_INFO_MED,
                      "The value of dfs_cac_block_tx[%d] for ApCtx[%pK]",
                      pHddApCtx->dfs_cac_block_tx, pHddApCtx);

            if ((NV_CHANNEL_DFS ==
                vos_nv_getChannelEnabledState(pHddApCtx->operatingChannel)) &&
                (pHddCtx->cfg_ini->IsSapDfsChSifsBurstEnabled == 0))
            {

                VOS_TRACE( VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_INFO,
                           "%s: Setting SIFS Burst disable for DFS channel %d",
                            __func__, pHddApCtx->operatingChannel);

                if (process_wma_set_command((int)pHostapdAdapter->sessionId,
                                            (int)WMI_PDEV_PARAM_BURST_ENABLE,
                                             0, PDEV_CMD))
                {
                    VOS_TRACE( VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
                               "%s: Failed to Set SIFS Burst for DFS channel %d",
                                __func__, pHddApCtx->operatingChannel);
                }
            }

            //Fill the params for sending IWEVCUSTOM Event with SOFTAP.enabled
            startBssEvent = "SOFTAP.enabled";
            memset(&we_custom_start_event, '\0', sizeof(we_custom_start_event));
            memcpy(&we_custom_start_event, startBssEvent, strlen(startBssEvent));
            memset(&wrqu, 0, sizeof(wrqu));
            wrqu.data.length = strlen(startBssEvent);
            we_event = IWEVCUSTOM;
            we_custom_event_generic = we_custom_start_event;
            hdd_dump_concurrency_info(pHddCtx);
#ifdef WLAN_FEATURE_SAP_TO_FOLLOW_STA_CHAN
            if(pHostapdAdapter->device_mode == WLAN_HDD_SOFTAP)
	    {
		    mutex_lock(&pHddCtx->ch_switch_ctx.sap_ch_sw_lock);
		    if(pHddCtx->ch_switch_ctx.is_ch_sw_through_sta_csa &&
				    (pHddApCtx->operatingChannel == pHddCtx->ch_switch_ctx.csa_to_channel)){
			    hddLog(LOG1, FL("Successfully Channel Switch is done to CH = %d"),
					    pHddApCtx->operatingChannel);
			    pHddCtx->ch_switch_ctx.is_ch_sw_through_sta_csa = VOS_FALSE;
		    }
		    mutex_unlock(&pHddCtx->ch_switch_ctx.sap_ch_sw_lock);
	    }
#endif//#ifdef WLAN_FEATURE_SAP_TO_FOLLOW_STA_CHAN
            break; //Event will be sent after Switch-Case stmt

        case eSAP_STOP_BSS_EVENT:
            hddLog(LOG1, FL("BSS stop status = %s"),pSapEvent->sapevt.sapStopBssCompleteEvent.status ?
                             "eSAP_STATUS_FAILURE" : "eSAP_STATUS_SUCCESS");

            hdd_set_sap_auth_offload(pHostapdAdapter, FALSE);

            hdd_hostapd_channel_allow_suspend(pHostapdAdapter,
                    pHddApCtx->operatingChannel);

            //Free up Channel List incase if it is set

            sapCleanupChannelList(WLAN_HDD_GET_SAP_CTX_PTR(pHostapdAdapter));

            pHddApCtx->operatingChannel = 0; //Invalidate the channel info.
#ifdef IPA_OFFLOAD
            if (hdd_ipa_is_enabled(pHddCtx))
            {
                status = hdd_ipa_wlan_evt(pHostapdAdapter, pHddApCtx->uBCStaId,
                        WLAN_AP_DISCONNECT, pHostapdAdapter->dev->dev_addr);

                if (status)
                {
                    VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
                                  ("ERROR: WLAN_AP_DISCONNECT event failed!!"));
                    goto stopbss;
                }
            }
#endif
            /* reset the dfs_cac_status and dfs_cac_block_tx flag only when
             * the last BSS is stopped
             */
            con_sap_adapter = hdd_get_con_sap_adapter(pHostapdAdapter, true);
            if (!con_sap_adapter) {
                pHddApCtx->dfs_cac_block_tx = TRUE;
                pHddCtx->dev_dfs_cac_status = DFS_CAC_NEVER_DONE;
            }
            if (pHddCtx->cfg_ini->conc_custom_rule2 &&
                (WLAN_HDD_P2P_GO == pHostapdAdapter->device_mode)) {

                hdd_adapter_t *sta_adapter = hdd_get_adapter(pHddCtx,
                                                WLAN_HDD_INFRA_STATION);
                hddLog(VOS_TRACE_LEVEL_INFO_HIGH,
                       FL("P2PGO is going down now"));
                hdd_issue_stored_joinreq(sta_adapter, pHddCtx);
            }

            hddLog(VOS_TRACE_LEVEL_INFO,
                FL("bss_stop_reason=%d"), pHddApCtx->bss_stop_reason);
            if (pHddApCtx->bss_stop_reason != BSS_STOP_DUE_TO_MCC_SCC_SWITCH){
                /* when MCC to SCC switching happens, key storage should not be
                 * cleared due to hostapd will not repopulate the original keys
                 */
                pHddApCtx->groupKey.keyLength = 0;
                for (i = 0; i < CSR_MAX_NUM_KEY; i++)
                    pHddApCtx->wepKey[i].keyLength = 0;
            }

            /* clear the reason code in case BSS is stopped
             * in another place
             */
            pHddApCtx->bss_stop_reason = BSS_STOP_REASON_INVALID;
            goto stopbss;

        case eSAP_DFS_CAC_START:
            wlan_hdd_send_svc_nlink_msg(pHddCtx->radio_index,
                                        WLAN_SVC_DFS_CAC_START_IND,
                                        &dfs_info,
                                        sizeof(dfs_info));
            pHddCtx->dev_dfs_cac_status = DFS_CAC_IN_PROGRESS;
            if (VOS_STATUS_SUCCESS !=
                      hdd_send_radar_event(pHddCtx, eSAP_DFS_CAC_START,
                                           dfs_info, &pHostapdAdapter->wdev)) {
                      hddLog(LOGE, FL("Unable to indicate CAC start NL event"));
            } else {
                hddLog(VOS_TRACE_LEVEL_INFO,
                       FL("Sent CAC start to user space"));
            }
            pHddCtx->dfs_radar_found = VOS_FALSE;
            break;

        case eSAP_DFS_CAC_INTERRUPTED:
            /*
             * The CAC timer did not run completely and a radar was detected
             * during the CAC time. This new state will keep the tx path
             * blocked since we do not want any transmission on the DFS
             * channel. CAC end will only be reported here since the user
             * space applications are waiting on CAC end for their state
             * management.
             */
            if (VOS_STATUS_SUCCESS !=
                      hdd_send_radar_event(pHddCtx, eSAP_DFS_CAC_END,
                                           dfs_info, &pHostapdAdapter->wdev)) {
                      hddLog(LOGE,
                          FL("Unable to indicate CAC end (interrupted) event"));
            } else {
                hddLog(VOS_TRACE_LEVEL_INFO,
                    FL("Sent CAC end (interrupted) to user space"));
            }
            break;

        case eSAP_DFS_CAC_END:
            wlan_hdd_send_svc_nlink_msg(pHddCtx->radio_index,
                                        WLAN_SVC_DFS_CAC_END_IND,
                                        &dfs_info,
                                        sizeof(dfs_info));
            pHddApCtx->dfs_cac_block_tx = VOS_FALSE;
            pHddCtx->dev_dfs_cac_status = DFS_CAC_ALREADY_DONE;
            if (VOS_STATUS_SUCCESS !=
                      hdd_send_radar_event(pHddCtx, eSAP_DFS_CAC_END,
                                           dfs_info, &pHostapdAdapter->wdev)) {
                      hddLog(LOGE, FL("Unable to indicate CAC end NL event"));
            } else {
                hddLog(VOS_TRACE_LEVEL_INFO,
                       FL("Sent CAC end to user space"));
            }
            break;

        case eSAP_DFS_RADAR_DETECT:
            wlan_hdd_send_svc_nlink_msg(pHddCtx->radio_index,
                                        WLAN_SVC_DFS_RADAR_DETECT_IND,
                                        &dfs_info,
                                        sizeof(dfs_info));
            pHddCtx->dev_dfs_cac_status = DFS_CAC_NEVER_DONE;
            if (VOS_STATUS_SUCCESS !=
                      hdd_send_radar_event(pHddCtx, eSAP_DFS_RADAR_DETECT,
                                           dfs_info, &pHostapdAdapter->wdev)) {
                      hddLog(LOGE, FL("Unable to indicate Radar detect NL event"));
            } else {
                hddLog(VOS_TRACE_LEVEL_INFO,
                       FL("Sent radar detected to user space"));
            }
            break;

        case eSAP_DFS_NO_AVAILABLE_CHANNEL:
            wlan_hdd_send_svc_nlink_msg(pHddCtx->radio_index,
                                        WLAN_SVC_DFS_ALL_CHANNEL_UNAVAIL_IND,
                                        &dfs_info,
                                        sizeof(dfs_info));
            break;

        case eSAP_STA_SET_KEY_EVENT:
            /* TODO: forward the message to hostapd once implementation
               is done for now just print */
            hddLog(LOG1, FL("SET Key: configured status = %s"),pSapEvent->sapevt.sapStationSetKeyCompleteEvent.status ?
                            "eSAP_STATUS_FAILURE" : "eSAP_STATUS_SUCCESS");
            return VOS_STATUS_SUCCESS;
        case eSAP_STA_DEL_KEY_EVENT:
           /* TODO: forward the message to hostapd once implementation
              is done for now just print */
           hddLog(LOG1, FL("Event received %s"),"eSAP_STA_DEL_KEY_EVENT");
           return VOS_STATUS_SUCCESS;
        case eSAP_STA_MIC_FAILURE_EVENT:
        {
            memset(&msg, '\0', sizeof(msg));
            msg.src_addr.sa_family = ARPHRD_ETHER;
            memcpy(msg.src_addr.sa_data, &pSapEvent->sapevt.sapStationMICFailureEvent.staMac, sizeof(v_MACADDR_t));
            hddLog(LOG1, "MIC MAC "MAC_ADDRESS_STR, MAC_ADDR_ARRAY(msg.src_addr.sa_data));
            if(pSapEvent->sapevt.sapStationMICFailureEvent.multicast == eSAP_TRUE)
             msg.flags = IW_MICFAILURE_GROUP;
            else
             msg.flags = IW_MICFAILURE_PAIRWISE;
            memset(&wrqu, 0, sizeof(wrqu));
            wrqu.data.length = sizeof(msg);
            we_event = IWEVMICHAELMICFAILURE;
            we_custom_event_generic = (v_BYTE_t *)&msg;
        }
      /* inform mic failure to nl80211 */
        cfg80211_michael_mic_failure(dev,
                                     pSapEvent->sapevt.
                                     sapStationMICFailureEvent.staMac.bytes,
                                     ((pSapEvent->sapevt.sapStationMICFailureEvent.multicast == eSAP_TRUE) ?
                                      NL80211_KEYTYPE_GROUP :
                                      NL80211_KEYTYPE_PAIRWISE),
                                     pSapEvent->sapevt.sapStationMICFailureEvent.keyId,
                                     pSapEvent->sapevt.sapStationMICFailureEvent.TSC,
                                     GFP_KERNEL);
            break;

        case eSAP_STA_ASSOC_EVENT:
        case eSAP_STA_REASSOC_EVENT:
            event = &pSapEvent->sapevt.sapStationAssocReassocCompleteEvent;
            wrqu.addr.sa_family = ARPHRD_ETHER;
            memcpy(wrqu.addr.sa_data, &event->staMac,
                sizeof(v_MACADDR_t));
            hddLog(LOG1, " associated "MAC_ADDRESS_STR, MAC_ADDR_ARRAY(wrqu.addr.sa_data));
            we_event = IWEVREGISTERED;

#ifdef WLAN_FEATURE_MBSSID
            WLANSAP_Get_WPS_State(WLAN_HDD_GET_SAP_CTX_PTR(pHostapdAdapter), &bWPSState);
#else
            WLANSAP_Get_WPS_State((WLAN_HDD_GET_CTX(pHostapdAdapter))->pvosContext, &bWPSState);
#endif

            if ( (eCSR_ENCRYPT_TYPE_NONE == pHddApCtx->ucEncryptType) ||
                 ( eCSR_ENCRYPT_TYPE_WEP40_STATICKEY == pHddApCtx->ucEncryptType ) ||
                 ( eCSR_ENCRYPT_TYPE_WEP104_STATICKEY == pHddApCtx->ucEncryptType ) )
            {
                bAuthRequired = FALSE;
            }

            if (bAuthRequired || bWPSState == eANI_BOOLEAN_TRUE )
            {
                vos_status = hdd_softap_RegisterSTA(pHostapdAdapter,
                                                    TRUE,
                                                    pHddApCtx->uPrivacy,
                                                    event->staId,
                                                    0,
                                                    0,
                                                    (v_MACADDR_t *)wrqu.addr.sa_data,
                                                    event->wmmEnabled);
                if (!VOS_IS_STATUS_SUCCESS(vos_status))
                     hddLog(LOGW, FL("Failed to register STA %d "MAC_ADDRESS_STR""),
                                     vos_status, MAC_ADDR_ARRAY(wrqu.addr.sa_data));
            }
            else
            {
                vos_status = hdd_softap_RegisterSTA(pHostapdAdapter,
                                                    FALSE,
                                                    pHddApCtx->uPrivacy,
                                                    event->staId,
                                                    0,
                                                    0,
                                                    (v_MACADDR_t *)wrqu.addr.sa_data,
                                                    event->wmmEnabled);
                if (!VOS_IS_STATUS_SUCCESS(vos_status))
                    hddLog(LOGW, FL("Failed to register STA %d "MAC_ADDRESS_STR""),
                           vos_status, MAC_ADDR_ARRAY(wrqu.addr.sa_data));
            }

            if (VOS_IS_STATUS_SUCCESS(vos_status)) {
                staId = event->staId;
                hdd_fill_station_info(&pHostapdAdapter->aStaInfo[staId],
                                      event);
                pHostapdAdapter->aStaInfo[staId].ecsa_capable =
                    pSapEvent->
                    sapevt.sapStationAssocReassocCompleteEvent.ecsa_capable;
            }

#ifdef IPA_OFFLOAD
            if (hdd_ipa_is_enabled(pHddCtx))
            {
                status = hdd_ipa_wlan_evt(pHostapdAdapter,
                   event->staId,
                   WLAN_CLIENT_CONNECT_EX,
                   event->staMac.bytes);
                if (status)
                {
                   VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR, ("ERROR: WLAN_CLIENT_CONNECT_EX event failed!!"));
                   goto stopbss;
                }
            }
#endif
#ifdef QCA_PKT_PROTO_TRACE
            /* Peer associated, update into trace buffer */
            if (pHddCtx->cfg_ini->gEnableDebugLog)
            {
               vos_pkt_trace_buf_update("HA:ASSOC");
            }
#endif /* QCA_PKT_PROTO_TRACE */

            DPTRACE(adf_dp_trace_mgmt_pkt(ADF_DP_TRACE_MGMT_PACKET_RECORD,
                           pHostapdAdapter->sessionId,
                           ADF_PROTO_TYPE_MGMT, ADF_PROTO_MGMT_ASSOC));

#ifdef FEATURE_BUS_BANDWIDTH
            /* start timer in sap/p2p_go */
            if (pHddApCtx->bApActive == VOS_FALSE)
            {
                spin_lock_bh(&pHddCtx->bus_bw_lock);
                pHostapdAdapter->prev_tx_packets = pHostapdAdapter->stats.tx_packets;
                pHostapdAdapter->prev_rx_packets = pHostapdAdapter->stats.rx_packets;
                tlshim_get_intra_bss_fwd_pkts_count(
                       pHostapdAdapter->sessionId,
                       &pHostapdAdapter->prev_fwd_tx_packets,
                       &pHostapdAdapter->prev_fwd_rx_packets);
                pHostapdAdapter->prev_tx_bytes =
                        pHostapdAdapter->stats.tx_bytes;
                spin_unlock_bh(&pHddCtx->bus_bw_lock);
                hdd_start_bus_bw_compute_timer(pHostapdAdapter);
            }
#endif
            pHddApCtx->bApActive = VOS_TRUE;
            // Stop AP inactivity timer
            if (pHddApCtx->hdd_ap_inactivity_timer.state == VOS_TIMER_STATE_RUNNING)
            {
                vos_status = vos_timer_stop(&pHddApCtx->hdd_ap_inactivity_timer);
                if (!VOS_IS_STATUS_SUCCESS(vos_status))
                   hddLog(LOGE, FL("Failed to start AP inactivity timer"));
            }
#ifdef FEATURE_WLAN_AUTO_SHUTDOWN
            wlan_hdd_auto_shutdown_enable(pHddCtx, VOS_FALSE);
#endif
            vos_wake_lock_timeout_acquire(&pHddCtx->sap_wake_lock,
                                          HDD_SAP_WAKE_LOCK_DURATION,
                                          WIFI_POWER_EVENT_WAKELOCK_SAP);
            {
               v_U16_t iesLen =  event->iesLen;

               if (iesLen <= MAX_ASSOC_IND_IE_LEN )
               {
                  struct station_info *stainfo;
                  stainfo = vos_mem_malloc(sizeof(*stainfo));
                  if (stainfo == NULL) {
                      hddLog(LOGE, FL("alloc station_info failed"));
                      return VOS_STATUS_E_NOMEM;
                  }
                  memset(stainfo, 0, sizeof(*stainfo));
                  stainfo->assoc_req_ies =
                     (const u8 *)&event->ies[0];
                  stainfo->assoc_req_ies_len = iesLen;
#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 0, 0))
                  /*
                   * After Kernel 4.0, it's no longer need to set
                   * STATION_INFO_ASSOC_REQ_IES flag, as it
                   * changed to use assoc_req_ies_len length to
                   * check the existance of request IE.
                   */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,0,31)) || defined(WITH_BACKPORTS)
                  stainfo->filled |= STATION_INFO_ASSOC_REQ_IES;
#endif
#endif
                  cfg80211_new_sta(dev,
                        (const u8 *)&event->staMac.bytes[0],
                        stainfo, GFP_KERNEL);
                  vos_mem_free(stainfo);
               }
               else
               {
                  hddLog(LOGE, FL(" Assoc Ie length is too long"));
               }
            }

            pScanInfo =  &pHostapdAdapter->scan_info;
            // Lets do abort scan to ensure smooth authentication for client
            if ((pScanInfo != NULL) && pScanInfo->mScanPending)
            {
                hdd_abort_mac_scan(pHddCtx, pHostapdAdapter->sessionId,
                                   eCSR_SCAN_ABORT_DEFAULT);
            }
            if (pHostapdAdapter->device_mode == WLAN_HDD_P2P_GO)
            {
                /* send peer status indication to oem app */
                hdd_SendPeerStatusIndToOemApp(
                    &event->staMac,
                    ePeerConnected,
                    event->timingMeasCap,
                    pHostapdAdapter->sessionId,
                    &event->chan_info,
                    pHostapdAdapter->device_mode);
            }

            hdd_wlan_green_ap_add_sta(pHddCtx);

            if (pHostapdAdapter->device_mode == WLAN_HDD_SOFTAP &&
                !bAuthRequired  && bWPSState == eANI_BOOLEAN_FALSE) {
                    uint32_t sub20_channelwidth;

                    if (hdd_hostapd_sub20_channelwidth_can_switch(
                         pHostapdAdapter, &sub20_channelwidth))
                            WLANSAP_set_sub20_channelwidth_with_csa(
                                WLAN_HDD_GET_SAP_CTX_PTR(pHostapdAdapter),
                                sub20_channelwidth);
            }
            break;
        case eSAP_STA_DISASSOC_EVENT:
            memcpy(wrqu.addr.sa_data, &pSapEvent->sapevt.sapStationDisassocCompleteEvent.staMac,
                   sizeof(v_MACADDR_t));
            hddLog(LOG1, " disassociated "MAC_ADDRESS_STR, MAC_ADDR_ARRAY(wrqu.addr.sa_data));

            vos_status = vos_event_set(&pHostapdState->sta_disassoc_event);
            if (!VOS_IS_STATUS_SUCCESS(vos_status))
                hddLog(VOS_TRACE_LEVEL_ERROR,
                        "ERROR: Station deauth event reporting failed!!");

            if (pSapEvent->sapevt.sapStationDisassocCompleteEvent.reason == eSAP_USR_INITATED_DISASSOC)
                hddLog(LOG1," User initiated disassociation");
            else
                hddLog(LOG1," MAC initiated disassociation");
            we_event = IWEVEXPIRED;
            vos_status = hdd_softap_GetStaId(pHostapdAdapter, &pSapEvent->sapevt.sapStationDisassocCompleteEvent.staMac, &staId);
            if (!VOS_IS_STATUS_SUCCESS(vos_status))
            {
                VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR, FL("ERROR: HDD Failed to find sta id!!"));
                return VOS_STATUS_E_FAILURE;
            }
#ifdef IPA_OFFLOAD
            if (!pHddCtx->isLogpInProgress && hdd_ipa_is_enabled(pHddCtx))
            {
                status = hdd_ipa_wlan_evt(pHostapdAdapter, staId, WLAN_CLIENT_DISCONNECT,
                pSapEvent->sapevt.sapStationDisassocCompleteEvent.staMac.bytes);

                if (status)
                {
                    VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
                        ("ERROR: WLAN_CLIENT_DISCONNECT event failed!!"));
                    goto stopbss;
                }
            }
#endif
#ifdef QCA_PKT_PROTO_TRACE
            /* Peer dis-associated, update into trace buffer */
            if (pHddCtx->cfg_ini->gEnableDebugLog)
            {
               vos_pkt_trace_buf_update("HA:DISASC");
            }
#endif /* QCA_PKT_PROTO_TRACE */

            DPTRACE(adf_dp_trace_mgmt_pkt(ADF_DP_TRACE_MGMT_PACKET_RECORD,
                           pHostapdAdapter->sessionId,
                           ADF_PROTO_TYPE_MGMT, ADF_PROTO_MGMT_DISASSOC));

            hdd_softap_DeregisterSTA(pHostapdAdapter, staId);

            pHddApCtx->bApActive = VOS_FALSE;
            spin_lock_bh( &pHostapdAdapter->staInfo_lock );
            for (i = 0; i < WLAN_MAX_STA_COUNT; i++)
            {
                if (pHostapdAdapter->aStaInfo[i].isUsed && i != (WLAN_HDD_GET_AP_CTX_PTR(pHostapdAdapter))->uBCStaId)
                {
                    pHddApCtx->bApActive = VOS_TRUE;
                    break;
                }
            }
            spin_unlock_bh( &pHostapdAdapter->staInfo_lock );

            // Start AP inactivity timer if no stations associated with it
            if ((0 != (WLAN_HDD_GET_CTX(pHostapdAdapter))->cfg_ini->nAPAutoShutOff))
            {
                if (pHddApCtx->bApActive == FALSE)
                {
                    if (pHddApCtx->hdd_ap_inactivity_timer.state == VOS_TIMER_STATE_STOPPED)
                    {
                        vos_status = vos_timer_start(&pHddApCtx->hdd_ap_inactivity_timer, (WLAN_HDD_GET_CTX(pHostapdAdapter))->cfg_ini->nAPAutoShutOff * 1000);
                        if (!VOS_IS_STATUS_SUCCESS(vos_status))
                            hddLog(LOGE, FL("Failed to init AP inactivity timer"));
                    }
                    else
                        VOS_ASSERT(vos_timer_getCurrentState(&pHddApCtx->hdd_ap_inactivity_timer) == VOS_TIMER_STATE_STOPPED);
                }
            }
#ifdef FEATURE_WLAN_AUTO_SHUTDOWN
            wlan_hdd_auto_shutdown_enable(pHddCtx, VOS_TRUE);
#endif

            if (pSapEvent->sapevt.sapStationDisassocCompleteEvent.statusCode ==
                             eSIR_SME_SAP_AUTH_OFFLOAD_PEER_UPDATE_STATUS) {
                /** eSIR_SME_SAP_AUTH_OFFLOAD_PEER_UPDATE_STATUS indicates:
                 * The existing sta connection needs to be updated instead
                 * of clean up the sta. This condition could only happens
                 * when Host SAP sleep with WOW and SAP Auth offload enabled.
                 */

                hddLog(LOG1,"SAP peer update sta:Id=%d, Mac="MAC_ADDRESS_STR,
                    pSapEvent->sapevt.sapStationDisassocCompleteEvent.staId,
                    MAC_ADDR_ARRAY(pSapEvent->sapevt.
                    sapStationDisassocCompleteEvent.staMac.bytes));
            } else {
                hddLog(LOG1,"SAP del sta: staId=%d, staMac="MAC_ADDRESS_STR,
                    pSapEvent->sapevt.sapStationDisassocCompleteEvent.staId,
                    MAC_ADDR_ARRAY(pSapEvent->sapevt.
                    sapStationDisassocCompleteEvent.staMac.bytes));

                cfg80211_del_sta(dev,
                    (const u8 *)&pSapEvent->sapevt.sapStationDisassocCompleteEvent.staMac.bytes[0],
                    GFP_KERNEL);
            }

            //Update the beacon Interval if it is P2P GO
            vos_status = hdd_change_mcc_go_beacon_interval(pHostapdAdapter);
            if (VOS_STATUS_SUCCESS != vos_status)
            {
                hddLog(LOGE, "%s: failed to update Beacon interval %d",
                       __func__, vos_status);
            }
            if (pHostapdAdapter->device_mode == WLAN_HDD_P2P_GO)
            {
                /* send peer status indication to oem app */
                hdd_SendPeerStatusIndToOemApp(
                  &pSapEvent->sapevt.sapStationDisassocCompleteEvent.staMac,
                  ePeerDisconnected, 0,
                  pHostapdAdapter->sessionId, NULL,
                  pHostapdAdapter->device_mode);
            }

#ifdef FEATURE_BUS_BANDWIDTH
            /*stop timer in sap/p2p_go */
            if (pHddApCtx->bApActive == FALSE)
            {
                spin_lock_bh(&pHddCtx->bus_bw_lock);
                pHostapdAdapter->prev_tx_packets = 0;
                pHostapdAdapter->prev_rx_packets = 0;
                pHostapdAdapter->prev_fwd_tx_packets = 0;
                pHostapdAdapter->prev_fwd_rx_packets = 0;
                pHostapdAdapter->prev_tx_bytes = 0;
                spin_unlock_bh(&pHddCtx->bus_bw_lock);
                hdd_stop_bus_bw_compute_timer(pHostapdAdapter);
            }
#endif

            hdd_wlan_green_ap_del_sta(pHddCtx);

            break;
        case eSAP_WPS_PBC_PROBE_REQ_EVENT:
        {
                static const char * message ="MLMEWPSPBCPROBEREQ.indication";
                union iwreq_data wreq;

                down(&pHddApCtx->semWpsPBCOverlapInd);
                pHddApCtx->WPSPBCProbeReq.probeReqIELen = pSapEvent->sapevt.sapPBCProbeReqEvent.WPSPBCProbeReq.probeReqIELen;

                vos_mem_copy(pHddApCtx->WPSPBCProbeReq.probeReqIE, pSapEvent->sapevt.sapPBCProbeReqEvent.WPSPBCProbeReq.probeReqIE,
                    pHddApCtx->WPSPBCProbeReq.probeReqIELen);

                vos_mem_copy(pHddApCtx->WPSPBCProbeReq.peerMacAddr, pSapEvent->sapevt.sapPBCProbeReqEvent.WPSPBCProbeReq.peerMacAddr, sizeof(v_MACADDR_t));
                hddLog(LOG1, "WPS PBC probe req "MAC_ADDRESS_STR, MAC_ADDR_ARRAY(pHddApCtx->WPSPBCProbeReq.peerMacAddr));
                memset(&wreq, 0, sizeof(wreq));
                wreq.data.length = strlen(message); // This is length of message
                wireless_send_event(dev, IWEVCUSTOM, &wreq, (char *)message);

                return VOS_STATUS_SUCCESS;
        }
        case eSAP_ASSOC_STA_CALLBACK_EVENT:
            pAssocStasArray = pSapEvent->sapevt.sapAssocStaListEvent.pAssocStas;
            if (pSapEvent->sapevt.sapAssocStaListEvent.noOfAssocSta != 0)
            {   // List of associated stations
                for (i = 0; i < pSapEvent->sapevt.sapAssocStaListEvent.noOfAssocSta; i++)
                {
                    hddLog(LOG1,"Associated Sta Num %d:assocId=%d, staId=%d, staMac="MAC_ADDRESS_STR,
                        i+1,
                        pAssocStasArray->assocId,
                        pAssocStasArray->staId,
                                    MAC_ADDR_ARRAY(pAssocStasArray->staMac.bytes));
                        pAssocStasArray++;
            }
            }
            vos_mem_free(pSapEvent->sapevt.sapAssocStaListEvent.pAssocStas);// Release caller allocated memory here
            pSapEvent->sapevt.sapAssocStaListEvent.pAssocStas = NULL;
            return VOS_STATUS_SUCCESS;
        case eSAP_REMAIN_CHAN_READY:
           hdd_remainChanReadyHandler( pHostapdAdapter );
           return VOS_STATUS_SUCCESS;
        case eSAP_UNKNOWN_STA_JOIN:
            snprintf(unknownSTAEvent, IW_CUSTOM_MAX, "JOIN_UNKNOWN_STA-%02x:%02x:%02x:%02x:%02x:%02x",
                pSapEvent->sapevt.sapUnknownSTAJoin.macaddr.bytes[0],
                pSapEvent->sapevt.sapUnknownSTAJoin.macaddr.bytes[1],
                pSapEvent->sapevt.sapUnknownSTAJoin.macaddr.bytes[2],
                pSapEvent->sapevt.sapUnknownSTAJoin.macaddr.bytes[3],
                pSapEvent->sapevt.sapUnknownSTAJoin.macaddr.bytes[4],
                pSapEvent->sapevt.sapUnknownSTAJoin.macaddr.bytes[5]);
            we_event = IWEVCUSTOM; /* Discovered a new node (AP mode). */
            wrqu.data.pointer = unknownSTAEvent;
            wrqu.data.length = strlen(unknownSTAEvent);
            we_custom_event_generic = (v_BYTE_t *)unknownSTAEvent;
            hddLog(LOGE,"%s", unknownSTAEvent);
            break;

        case eSAP_MAX_ASSOC_EXCEEDED:
            snprintf(maxAssocExceededEvent, IW_CUSTOM_MAX, "Peer %02x:%02x:%02x:%02x:%02x:%02x denied"
                    " assoc due to Maximum Mobile Hotspot connections reached. Please disconnect"
                    " one or more devices to enable the new device connection",
                    pSapEvent->sapevt.sapMaxAssocExceeded.macaddr.bytes[0],
                    pSapEvent->sapevt.sapMaxAssocExceeded.macaddr.bytes[1],
                    pSapEvent->sapevt.sapMaxAssocExceeded.macaddr.bytes[2],
                    pSapEvent->sapevt.sapMaxAssocExceeded.macaddr.bytes[3],
                    pSapEvent->sapevt.sapMaxAssocExceeded.macaddr.bytes[4],
                    pSapEvent->sapevt.sapMaxAssocExceeded.macaddr.bytes[5]);
            we_event = IWEVCUSTOM; /* Discovered a new node (AP mode). */
            wrqu.data.pointer = maxAssocExceededEvent;
            wrqu.data.length = strlen(maxAssocExceededEvent);
            we_custom_event_generic = (v_BYTE_t *)maxAssocExceededEvent;
            hddLog(LOG1,"%s", maxAssocExceededEvent);
            break;
        case eSAP_STA_ASSOC_IND:
            return VOS_STATUS_SUCCESS;

        case eSAP_DISCONNECT_ALL_P2P_CLIENT:
            hddLog(LOG1, FL(" Disconnecting all the P2P Clients...."));
            hdd_clear_all_sta(pHostapdAdapter, usrDataForCallback);
            return VOS_STATUS_SUCCESS;

        case eSAP_MAC_TRIG_STOP_BSS_EVENT :
            vos_status = hdd_stop_bss_link(pHostapdAdapter, usrDataForCallback);
            if (!VOS_IS_STATUS_SUCCESS(vos_status))
                hddLog(LOGW, FL("hdd_stop_bss_link failed %d"), vos_status);

            return VOS_STATUS_SUCCESS;

        case eSAP_CHANNEL_CHANGE_EVENT:
            hddLog(LOG1, FL("Received eSAP_CHANNEL_CHANGE_EVENT event"));
            if (pHostapdState->bssState != BSS_STOP) {
                /* Prevent suspend for new channel */
                hdd_hostapd_channel_prevent_suspend(pHostapdAdapter,
                        pSapEvent->sapevt.sapChSelected.pri_ch);
                /* Allow suspend for old channel */
                hdd_hostapd_channel_allow_suspend(pHostapdAdapter,
                        pHddApCtx->operatingChannel);
            }
            /* SME/PE is already updated for new operation channel. So update
             * HDD layer also here. This resolves issue in AP-AP mode where
             * AP1 channel is changed due to RADAR then CAC is going on and
             * START_BSS on new channel has not come to HDD. At this case if
             * AP2 is start it needs current operation channel for MCC DFS
             * restiction
             */
            pHddApCtx->operatingChannel =
                 pSapEvent->sapevt.sapChSelected.pri_ch;
            pHddApCtx->sapConfig.acs_cfg.pri_ch =
                 pSapEvent->sapevt.sapChSelected.pri_ch;
            pHddApCtx->sapConfig.acs_cfg.ht_sec_ch =
                 pSapEvent->sapevt.sapChSelected.ht_sec_ch;
            pHddApCtx->sapConfig.acs_cfg.vht_seg0_center_ch =
                 pSapEvent->sapevt.sapChSelected.vht_seg0_center_ch;
            pHddApCtx->sapConfig.acs_cfg.vht_seg1_center_ch =
                 pSapEvent->sapevt.sapChSelected.vht_seg1_center_ch;
            pHddApCtx->sapConfig.acs_cfg.ch_width =
                 pSapEvent->sapevt.sapChSelected.ch_width;

            /* Indicate operating channel change to hostapd
             * only for non driver override acs
             */
            if (pHostapdAdapter->device_mode == WLAN_HDD_SOFTAP &&
                                               pHddCtx->cfg_ini->force_sap_acs)
                return VOS_STATUS_SUCCESS;
            else
                return hdd_chan_change_notify(pHostapdAdapter, dev,
                           pSapEvent->sapevt.sapChSelected.pri_ch);
        case eSAP_ACS_SCAN_SUCCESS_EVENT:
            return hdd_handle_acs_scan_event(pSapEvent, pHostapdAdapter);
        case eSAP_DFS_NOL_GET:
            hddLog(VOS_TRACE_LEVEL_INFO,
                    FL("Received eSAP_DFS_NOL_GET event"));
            /* get the dfs nol from cnss */
            return hdd_wlan_get_dfs_nol(
                      pSapEvent->sapevt.sapDfsNolInfo.pDfsList,
                      pSapEvent->sapevt.sapDfsNolInfo.sDfsList);
        case eSAP_DFS_NOL_SET:
            hddLog(VOS_TRACE_LEVEL_INFO, FL("Received eSAP_DFS_NOL_SET event"));
            /* set the dfs nol to cnss */
            return hdd_wlan_set_dfs_nol(
                    pSapEvent->sapevt.sapDfsNolInfo.pDfsList,
                    pSapEvent->sapevt.sapDfsNolInfo.sDfsList);
        case eSAP_ACS_CHANNEL_SELECTED:
            hddLog(LOG1, FL("ACS Completed for wlan%d"),
                                              pHostapdAdapter->dev->ifindex);
            clear_bit(ACS_PENDING, &pHostapdAdapter->event_flags);
            clear_bit(ACS_IN_PROGRESS, &pHddCtx->g_event_flags);
            pHddApCtx->sapConfig.acs_cfg.pri_ch =
                 pSapEvent->sapevt.sapChSelected.pri_ch;
            pHddApCtx->sapConfig.acs_cfg.ht_sec_ch =
                 pSapEvent->sapevt.sapChSelected.ht_sec_ch;
            pHddApCtx->sapConfig.acs_cfg.vht_seg0_center_ch =
                 pSapEvent->sapevt.sapChSelected.vht_seg0_center_ch;
            pHddApCtx->sapConfig.acs_cfg.vht_seg1_center_ch =
                 pSapEvent->sapevt.sapChSelected.vht_seg1_center_ch;
            pHddApCtx->sapConfig.acs_cfg.ch_width =
                 pSapEvent->sapevt.sapChSelected.ch_width;
            /* send vendor event to hostapd only for hostapd based acs */
            if (!test_bit(SOFTAP_BSS_STARTED, &pHostapdAdapter->event_flags)) {
                if (!pHddCtx->cfg_ini->force_sap_acs)
                    wlan_hdd_cfg80211_acs_ch_select_evt(pHostapdAdapter);
            } else {
                pHddApCtx->sapConfig.channel =
                    pHddApCtx->sapConfig.backup_channel;
            }

            return VOS_STATUS_SUCCESS;
        case eSAP_ECSA_CHANGE_CHAN_IND:
            hddLog(LOG1,
              FL("Channel change indication from peer for channel %d"),
                            pSapEvent->sapevt.sap_chan_cng_ind.new_chan);
            if (hdd_softap_set_channel_change(dev,
                 pSapEvent->sapevt.sap_chan_cng_ind.new_chan))
                return VOS_STATUS_E_FAILURE;
            else
                return VOS_STATUS_SUCCESS;
        default:
            hddLog(LOG1,"SAP message is not handled");
            goto stopbss;
            return VOS_STATUS_SUCCESS;
    }
    wireless_send_event(dev, we_event, &wrqu, (char *)we_custom_event_generic);
    return VOS_STATUS_SUCCESS;

stopbss :
    {
        v_BYTE_t we_custom_event[64];
        char *stopBssEvent = "STOP-BSS.response";//17
        int event_len = strlen(stopBssEvent);

        hddLog(LOG1, FL("BSS stop status = %s"),
               pSapEvent->sapevt.sapStopBssCompleteEvent.status ?
                            "eSAP_STATUS_FAILURE" : "eSAP_STATUS_SUCCESS");

        /* Change the BSS state now since, as we are shutting things down,
         * we don't want interfaces to become re-enabled */
        pHostapdState->bssState = BSS_STOP;

        if (0 != (WLAN_HDD_GET_CTX(pHostapdAdapter))->cfg_ini->nAPAutoShutOff)
        {
            if (VOS_TIMER_STATE_RUNNING == pHddApCtx->hdd_ap_inactivity_timer.state)
            {
                vos_status = vos_timer_stop(&pHddApCtx->hdd_ap_inactivity_timer);
                if (!VOS_IS_STATUS_SUCCESS(vos_status))
                    hddLog(LOGE, FL("Failed to stop AP inactivity timer"));
            }

            vos_status = vos_timer_destroy(&pHddApCtx->hdd_ap_inactivity_timer);
            if (!VOS_IS_STATUS_SUCCESS(vos_status))
                hddLog(LOGE, FL("Failed to Destroy AP inactivity timer"));
        }
#ifdef FEATURE_WLAN_AUTO_SHUTDOWN
        wlan_hdd_auto_shutdown_enable(pHddCtx, VOS_TRUE);
#endif

        /* Stop the pkts from n/w stack as we are going to free all of
         * the TX WMM queues for all STAID's */

       /*
        * If channel avoidance is in progress means driver is performing SAP
        * restart. So don't do carrier off, which may lead framework to do
        * driver reload.
        */
        hddLog(LOG1, FL("ch avoid in progress: %d"),
                        pHddCtx->is_ch_avoid_in_progress);
        if (pHddCtx->is_ch_avoid_in_progress)
            wlan_hdd_netif_queue_control(pHostapdAdapter, WLAN_NETIF_TX_DISABLE,
                                         WLAN_CONTROL_PATH);
        else
            hdd_hostapd_stop(dev);

        /* reclaim all resources allocated to the BSS */
        vos_status = hdd_softap_stop_bss(pHostapdAdapter);
        if (!VOS_IS_STATUS_SUCCESS(vos_status))
            hddLog(LOGW, FL("hdd_softap_stop_bss failed %d"), vos_status);

        /* once the event is set, structure dev/pHostapdAdapter should
         * not be touched since they are now subject to being deleted
         * by another thread */
        if (eSAP_STOP_BSS_EVENT == sapEvent)
            vos_event_set(&pHostapdState->stop_bss_event);

        /* Notify user space that the BSS has stopped */
        memset(&we_custom_event, '\0', sizeof(we_custom_event));
        memcpy(&we_custom_event, stopBssEvent, event_len);
        memset(&wrqu, 0, sizeof(wrqu));
        wrqu.data.length = event_len;
        we_event = IWEVCUSTOM;
        we_custom_event_generic = we_custom_event;
        wireless_send_event(dev, we_event, &wrqu, (char *)we_custom_event_generic);
        hdd_dump_concurrency_info(pHddCtx);
    }
    return VOS_STATUS_SUCCESS;
}

int hdd_softap_unpackIE(
                tHalHandle halHandle,
                eCsrEncryptionType *pEncryptType,
                eCsrEncryptionType *mcEncryptType,
                eCsrAuthType *pAuthType,
                v_BOOL_t *pMFPCapable,
                v_BOOL_t *pMFPRequired,
                u_int16_t gen_ie_len,
                u_int8_t *gen_ie )
{
    tDot11fIERSN dot11RSNIE;
    tDot11fIEWPA dot11WPAIE;

    tANI_U8 *pRsnIe;
    tANI_U16 RSNIeLen;
    tANI_U32 status;

    if (NULL == halHandle)
    {
        hddLog(LOGE, FL("Error haHandle returned NULL"));
        return VOS_STATUS_E_FAILURE;
    }

    // Validity checks
    if ((gen_ie_len < VOS_MIN(DOT11F_IE_RSN_MIN_LEN, DOT11F_IE_WPA_MIN_LEN)) ||
        (gen_ie_len > VOS_MAX(DOT11F_IE_RSN_MAX_LEN, DOT11F_IE_WPA_MAX_LEN)) )
        return VOS_STATUS_E_FAILURE;
    // Type check
    if ( gen_ie[0] ==  DOT11F_EID_RSN)
    {
        // Validity checks
        if ((gen_ie_len < DOT11F_IE_RSN_MIN_LEN ) ||
            (gen_ie_len > DOT11F_IE_RSN_MAX_LEN) )
        {
            return VOS_STATUS_E_FAILURE;
        }
        // Skip past the EID byte and length byte
        pRsnIe = gen_ie + 2;
        RSNIeLen = gen_ie_len - 2;
        // Unpack the RSN IE
        memset(&dot11RSNIE, 0, sizeof(tDot11fIERSN));

        status = sme_unpack_rsn_ie(halHandle,
                                   pRsnIe,
                                   RSNIeLen,
                                   &dot11RSNIE);
        if (DOT11F_FAILED(status))
        {
            hddLog(LOGE,
            FL("unpack failed for RSN IE status:(0x%08x)"),
               status);
            return VOS_STATUS_E_FAILURE;
        }

        // Copy out the encryption and authentication types
        hddLog(LOG1, FL("%s: pairwise cipher suite count: %d"),
                __func__, dot11RSNIE.pwise_cipher_suite_count );
        hddLog(LOG1, FL("%s: authentication suite count: %d"),
                __func__, dot11RSNIE.akm_suite_cnt);
        /*Here we have followed the apple base code,
          but probably I suspect we can do something different*/
        //dot11RSNIE.akm_suite_cnt
        // Just translate the FIRST one
        *pAuthType =  hdd_TranslateRSNToCsrAuthType(dot11RSNIE.akm_suite[0]);
        //dot11RSNIE.pwise_cipher_suite_count
        *pEncryptType = hdd_TranslateRSNToCsrEncryptionType(dot11RSNIE.pwise_cipher_suites[0]);
        //dot11RSNIE.gp_cipher_suite_count
        *mcEncryptType = hdd_TranslateRSNToCsrEncryptionType(dot11RSNIE.gp_cipher_suite);
        // Set the PMKSA ID Cache for this interface
        *pMFPCapable = 0 != (dot11RSNIE.RSN_Cap[0] & 0x80);
        *pMFPRequired = 0 != (dot11RSNIE.RSN_Cap[0] & 0x40);
        // Calling csrRoamSetPMKIDCache to configure the PMKIDs into the cache
    } else
    if (gen_ie[0] == DOT11F_EID_WPA)
    {
        // Validity checks
        if ((gen_ie_len < DOT11F_IE_WPA_MIN_LEN ) ||
            (gen_ie_len > DOT11F_IE_WPA_MAX_LEN))
        {
            return VOS_STATUS_E_FAILURE;
        }
        // Skip past the EID byte and length byte - and four byte WiFi OUI
        pRsnIe = gen_ie + 2 + 4;
        RSNIeLen = gen_ie_len - (2 + 4);
        // Unpack the WPA IE
        memset(&dot11WPAIE, 0, sizeof(tDot11fIEWPA));
        status = dot11fUnpackIeWPA((tpAniSirGlobal) halHandle,
                            pRsnIe,
                            RSNIeLen,
                            &dot11WPAIE);
        if (DOT11F_FAILED(status))
        {
             hddLog(LOGE,
                    FL("unpack failed for WPA IE status:(0x%08x)"),
                    status);
             return VOS_STATUS_E_FAILURE;
        }

       // Copy out the encryption and authentication types
        hddLog(LOG1, FL("%s: WPA unicast cipher suite count: %d"),
                __func__, dot11WPAIE.unicast_cipher_count );
        hddLog(LOG1, FL("%s: WPA authentication suite count: %d"),
                __func__, dot11WPAIE.auth_suite_count);
        //dot11WPAIE.auth_suite_count
        // Just translate the FIRST one
        *pAuthType =  hdd_TranslateWPAToCsrAuthType(dot11WPAIE.auth_suites[0]);
        //dot11WPAIE.unicast_cipher_count
        *pEncryptType = hdd_TranslateWPAToCsrEncryptionType(dot11WPAIE.unicast_ciphers[0]);
        //dot11WPAIE.unicast_cipher_count
        *mcEncryptType = hdd_TranslateWPAToCsrEncryptionType(dot11WPAIE.multicast_cipher);
        *pMFPCapable = VOS_FALSE;
        *pMFPRequired = VOS_FALSE;
    }
    else
    {
        hddLog(LOGW, FL("%s: gen_ie[0]: %d"), __func__, gen_ie[0]);
        return VOS_STATUS_E_FAILURE;
    }
    return VOS_STATUS_SUCCESS;
}

 /**---------------------------------------------------------------------------

  \brief hdd_softap_set_channel_change() -
   This function to support SAP channel change with CSA IE
   set in the beacons.

  \param  - dev - Pointer to the net device.
          - target_channel - target channel number.
  \return - 0 for success, non zero for failure

  --------------------------------------------------------------------------*/

int hdd_softap_set_channel_change(struct net_device *dev, int target_channel)
{
    VOS_STATUS status;
    int ret = 0;
    hdd_adapter_t *pHostapdAdapter = (netdev_priv(dev));
    hdd_context_t *pHddCtx = NULL;
    hdd_adapter_t *sta_adapter = NULL;
    hdd_station_ctx_t *sta_ctx;

#ifndef WLAN_FEATURE_MBSSID
    v_CONTEXT_t pVosContext = (WLAN_HDD_GET_CTX(pHostapdAdapter))->pvosContext;
#endif

    pHddCtx = WLAN_HDD_GET_CTX(pHostapdAdapter);
    ret = wlan_hdd_validate_context(pHddCtx);
    if (ret)
        return ret;

    sta_adapter = hdd_get_adapter(pHddCtx, WLAN_HDD_INFRA_STATION);
    /*
     * conc_custom_rule1:
     * Force SCC for SAP + STA
     * if STA is already connected then we shouldn't allow
     * channel switch in SAP interface
     */
    if (sta_adapter && pHddCtx->cfg_ini->conc_custom_rule1)
    {
        sta_ctx = WLAN_HDD_GET_STATION_CTX_PTR(sta_adapter);
        if (hdd_connIsConnected(sta_ctx))
        {
            hddLog(LOGE, FL("Channel switch not allowed after STA connection with conc_custom_rule1 enabled"));
            return -EBUSY;
        }
    }

    spin_lock_bh(&pHddCtx->dfs_lock);
    if (pHddCtx->dfs_radar_found == VOS_TRUE)
    {
        spin_unlock_bh(&pHddCtx->dfs_lock);
        hddLog(VOS_TRACE_LEVEL_ERROR, "%s: Channel switch in progress!!",
               __func__);
        ret = -EBUSY;
        return ret;
    }
    /*
     * Set the dfs_radar_found flag to mimic channel change
     * when a radar is found. This will enable synchronizing
     * SAP and HDD states similar to that of radar indication.
     * Suspend the netif queues to stop queuing Tx frames
     * from upper layers.  netif queues will be resumed
     * once the channel change is completed and SAP will
     * post eSAP_START_BSS_EVENT success event to HDD.
     */
    pHddCtx->dfs_radar_found = VOS_TRUE;

    spin_unlock_bh(&pHddCtx->dfs_lock);
    /*
     * Post the Channel Change request to SAP.
     */
    status = WLANSAP_SetChannelChangeWithCsa(
#ifdef WLAN_FEATURE_MBSSID
                WLAN_HDD_GET_SAP_CTX_PTR(pHostapdAdapter),
#else
                pVosContext,
#endif
                (v_U32_t) target_channel);

    if (VOS_STATUS_SUCCESS != status)
    {
         hddLog(VOS_TRACE_LEVEL_ERROR,
                "%s: SAP set channel failed for channel = %d",
                 __func__, target_channel);
        /*
         * If channel change command fails then clear the
         * radar found flag and also restart the netif
         * queues.
         */

        spin_lock_bh(&pHddCtx->dfs_lock);
        pHddCtx->dfs_radar_found = VOS_FALSE;
        spin_unlock_bh(&pHddCtx->dfs_lock);

        ret = -EINVAL;
    }

    return ret;
}

#ifdef FEATURE_WLAN_SUB_20_MHZ
/**
 * hdd_softap_set_channel_sub20_chanwidth_change() -This
 * function to support SAP channel change with CSA IE
 * set in the beacons.
 * @dev: Pointer to the net device
 * @chan_width: new sub20 channel width
 *
 * Return:  true or false
 */
int hdd_softap_set_channel_sub20_chanwidth_change(struct net_device *dev,
						  uint32_t chan_width)
{
	VOS_STATUS status;
	int ret;
	hdd_adapter_t *hostapd_adapter = (netdev_priv(dev));
	hdd_context_t *hdd_ctx_ptr;
	hdd_adapter_t *sta_adapter;
	hdd_station_ctx_t *sta_ctx;
	uint32_t sub20_chan_width;
	bool sub20_operate_permission;
	void *vos_ctx_ptr;

	hdd_ctx_ptr = WLAN_HDD_GET_CTX(hostapd_adapter);
	ret = wlan_hdd_validate_context(hdd_ctx_ptr);
	if (ret)
		return ret;

	sta_adapter = hdd_get_adapter(hdd_ctx_ptr, WLAN_HDD_INFRA_STATION);
	/*
	 * conc_custom_rule1:
	 * Force SCC for SAP + STA
	 * if STA is already connected then we shouldn't allow
	 * channel switch in SAP interface
	 */
	if (sta_adapter && hdd_ctx_ptr->cfg_ini->conc_custom_rule1) {
		sta_ctx = WLAN_HDD_GET_STATION_CTX_PTR(sta_adapter);
		if (hdd_connIsConnected(sta_ctx)) {
			hddLog(LOGE,
			       FL("sub20 chan switch not allowed"));
			return -EBUSY;
		}
	}

	switch (chan_width) {
	case NL80211_CHAN_WIDTH_5:
		sub20_chan_width = SUB20_MODE_5MHZ;
		break;
	case NL80211_CHAN_WIDTH_10:
		sub20_chan_width = SUB20_MODE_10MHZ;
		break;
	case NL80211_CHAN_WIDTH_20_NOHT:
		sub20_chan_width = SUB20_MODE_NONE;
		break;
	default:
		hddLog(LOGE, FL("invalid param %d"), chan_width);
		return -EINVAL;
	}

	sub20_operate_permission =
		hdd_sub20_channelwidth_can_set(hostapd_adapter,
					       sub20_chan_width);
	if (!sub20_operate_permission) {
		hddLog(LOGE, FL("can't set sub20_chan_width in curr chan"));
		return -EINVAL;
	}

	spin_lock_bh(&hdd_ctx_ptr->dfs_lock);
	if (hdd_ctx_ptr->dfs_radar_found == VOS_TRUE) {
		spin_unlock_bh(&hdd_ctx_ptr->dfs_lock);
		hddLog(LOGE,
		       FL("sub20 chan width switch in progress!!"));
		return -EBUSY;
	}
	/*
	 * Set the dfs_radar_found flag to mimic channel change
	 * when a radar is found. This will enable synchronizing
	 * SAP and HDD states similar to that of radar indication.
	 * Suspend the netif queues to stop queuing Tx frames
	 * from upper layers.  netif queues will be resumed
	 * once the channel change is completed and SAP will
	 * post eSAP_START_BSS_EVENT success event to HDD.
	 */
	hdd_ctx_ptr->dfs_radar_found = VOS_TRUE;
	spin_unlock_bh(&hdd_ctx_ptr->dfs_lock);

	vos_ctx_ptr = WLAN_HDD_GET_SAP_CTX_PTR(hostapd_adapter);
	status = WLANSAP_set_sub20_channelwidth_with_csa(vos_ctx_ptr,
							 sub20_chan_width);
	if (VOS_STATUS_SUCCESS != status) {
		hddLog(LOGE,
		       FL("sub20 chan width %d switch failed"),
		       sub20_chan_width);
		/*
		 * If channel change command fails then clear the
		 * radar found flag and also restart the netif
		 * queues.
		 */
		spin_lock_bh(&hdd_ctx_ptr->dfs_lock);
		hdd_ctx_ptr->dfs_radar_found = VOS_FALSE;
		spin_unlock_bh(&hdd_ctx_ptr->dfs_lock);

		ret = -EINVAL;
	}

	return ret;
}
#endif

/**
 * hdd_sap_get_chan_width() - get channel width of sap
 * @adapter: adapter being queried
 * @value: where to store the value
 *
 * Return: 0 on success, negative errno on failure
 */
static int hdd_sap_get_chan_width(hdd_adapter_t *adapter, int *value)
{
	void *pvosctx;
	uint32_t vht_channel_width = 0;
	hdd_context_t *hdd_ctx;
	hdd_hostapd_state_t *phostapdstate;

	ENTER();
	hdd_ctx = WLAN_HDD_GET_CTX(adapter);
	phostapdstate = WLAN_HDD_GET_HOSTAP_STATE_PTR(adapter);

	if (phostapdstate->bssState != BSS_START) {
		*value = -EINVAL;
		return -EINVAL;
	}

#ifdef WLAN_FEATURE_MBSSID
	pvosctx = WLAN_HDD_GET_SAP_CTX_PTR(adapter);
#else
	pvosctx = hdd_ctx->pvosContext;
#endif

	wlansap_get_chan_width(pvosctx, &vht_channel_width);
	*value = vht_channel_width;
	hddLog(LOGW, FL("chan_width = %d"), vht_channel_width);

	return 0;
}

int
static __iw_softap_set_ini_cfg(struct net_device *dev,
                          struct iw_request_info *info,
                          union iwreq_data *wrqu, char *extra)
{
    VOS_STATUS vstatus;
    int errno;
    hdd_adapter_t *adapter;
    hdd_context_t *hdd_ctx;
    char *value;
    size_t len;

    ENTER();

    adapter = netdev_priv(dev);
    if (adapter == NULL)
    {
        VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
                                        "%s: adapter is NULL!", __func__);
        return -EINVAL;
    }

    hdd_ctx = WLAN_HDD_GET_CTX(adapter);
    errno = wlan_hdd_validate_context(hdd_ctx);
    if (errno != 0)
        return errno;

    /* ensure null termination */
    len = min_t(size_t, wrqu->data.length, QCSAP_IOCTL_MAX_STR_LEN);
    value = vos_mem_malloc(len + 1);
    if (!value)
        return -ENOMEM;

    vos_mem_copy(value, extra, len);
    value[len] = '\0';
    VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_INFO,
              "%s: Received data %s", __func__, value);

    vstatus = hdd_execute_global_config_command(hdd_ctx, value);
#ifdef WLAN_FEATURE_MBSSID
    if (vstatus == VOS_STATUS_E_PERM) {
        vstatus = hdd_execute_sap_dyn_config_command(adapter, value);
        if (vstatus == VOS_STATUS_SUCCESS)
            VOS_TRACE( VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_INFO,
                   "%s: Stored in Dynamic SAP ini config", __func__);
    }
#endif
    vos_mem_free(value);

    EXIT();

    return vos_status_to_os_return(vstatus);
}

int
static iw_softap_set_ini_cfg(struct net_device *dev,
                             struct iw_request_info *info,
                             union iwreq_data *wrqu, char *extra)
{
	int ret;

	vos_ssr_protect(__func__);
	ret = __iw_softap_set_ini_cfg(dev, info, wrqu, extra);
	vos_ssr_unprotect(__func__);

	return ret;
}

int
static __iw_softap_get_ini_cfg(struct net_device *dev,
                             struct iw_request_info *info,
                             union iwreq_data *wrqu, char *extra)
{
    hdd_adapter_t *pAdapter = WLAN_HDD_GET_PRIV_PTR(dev);
    hdd_context_t *pHddCtx;
    int ret = 0;

    pHddCtx = WLAN_HDD_GET_CTX(pAdapter);
    ret = wlan_hdd_validate_context(pHddCtx);
    if (ret != 0)
        return ret;

#ifdef WLAN_FEATURE_MBSSID
    VOS_TRACE( VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_INFO,
                   "%s: Printing Adapter MBSSID SAP Dyn INI Config", __func__);
    hdd_cfg_get_sap_dyn_config(pAdapter, extra, QCSAP_IOCTL_MAX_STR_LEN);
    /* Overwrite extra buffer with global ini config if need to return in buf */
#endif
    VOS_TRACE( VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_INFO,
                   "%s: Printing CLD global INI Config", __func__);
    hdd_cfg_get_global_config(pHddCtx, extra, QCSAP_IOCTL_MAX_STR_LEN);
    wrqu->data.length = strlen(extra)+1;

    return 0;
}

int
static iw_softap_get_ini_cfg(struct net_device *dev,
                             struct iw_request_info *info,
                             union iwreq_data *wrqu, char *extra)
{
	int ret;

	vos_ssr_protect(__func__);
	ret = __iw_softap_get_ini_cfg(dev, info, wrqu, extra);
	vos_ssr_unprotect(__func__);

	return ret;
}

static int __iw_softap_set_two_ints_getnone(struct net_device *dev,
                                            struct iw_request_info *info,
                                            union iwreq_data *wrqu, char *extra)
{
    hdd_adapter_t *pAdapter = WLAN_HDD_GET_PRIV_PTR(dev);
    hdd_context_t *pHddCtx;
    int *value = (int *)extra;
    int sub_cmd = value[0];
    int ret = 0;

    pHddCtx = WLAN_HDD_GET_CTX(pAdapter);
    ret = wlan_hdd_validate_context(pHddCtx);
    if (ret != 0)
        goto out;

    switch(sub_cmd) {
#ifdef WLAN_DEBUG
    case QCSAP_IOCTL_SET_FW_CRASH_INJECT:
        hddLog(LOGE, "WE_SET_FW_CRASH_INJECT: %d %d", value[1], value[2]);
        if (!pHddCtx->cfg_ini->crash_inject_enabled) {
            hddLog(LOGE, "Crash Inject ini disabled, Ignore Crash Inject");
            return 0;
        }
        ret = process_wma_set_command_twoargs((int) pAdapter->sessionId,
                                           (int) GEN_PARAM_CRASH_INJECT,
                                           value[1], value[2], GEN_CMD);
        if (!ret)
            pHddCtx->isLogpInProgress = true;
        break;
#endif
    case QCSAP_IOCTL_DUMP_DP_TRACE_LEVEL:
        hddLog(LOG1, "WE_DUMP_DP_TRACE: %d %d",
            value[1], value[2]);
        if (value[1] == DUMP_DP_TRACE)
            adf_dp_trace_dump_all(value[2]);
        else if (value[1] == ENABLE_DP_TRACE_LIVE_MODE)
            adf_dp_trace_enable_live_mode();
        else if (value[1] == CLEAR_DP_TRACE_BUFFER)
            adf_dp_trace_clear_buffer();
        else
            hddLog(LOGE, "unexpected value for dump_dp_trace");
        break;
    default:
        hddLog(LOGE, "%s: Invalid IOCTL command %d", __func__, sub_cmd);
        break;
    }

out:
    return ret;
}

static int iw_softap_set_two_ints_getnone(struct net_device *dev,
                                          struct iw_request_info *info,
                                          union iwreq_data *wrqu, char *extra)
{
	int ret;

	vos_ssr_protect(__func__);
	ret = __iw_softap_set_two_ints_getnone(dev, info, wrqu, extra);
	vos_ssr_unprotect(__func__);

	return ret;
}

int
static __iw_softap_wowl_config_pattern(struct net_device *dev,
                                       struct iw_request_info *info,
                                       union iwreq_data *wrqu, char *extra)
{
    int sub_cmd;
    int ret = 0; /* success */
    char *pBuffer = NULL;
    hdd_adapter_t *pAdapter = (netdev_priv(dev));
    struct iw_point s_priv_data;

    if (!capable(CAP_NET_ADMIN)) {
        VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
                  FL("permission check failed"));
        return -EPERM;
    }

    if ((WLAN_HDD_GET_CTX(pAdapter))->isLogpInProgress)
    {
        VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_FATAL,
                  "%s:LOGP in Progress. Ignore!!!", __func__);
        return -EBUSY;
    }

    /* helper function to get iwreq_data with compat handling. */
    if (hdd_priv_get_data(&s_priv_data, wrqu)) {
        return -EINVAL;
    }

    /* make sure all params are correctly passed to function */
    if ((NULL == s_priv_data.pointer) || (0 == s_priv_data.length)) {
        return -EINVAL;
    }

    sub_cmd = s_priv_data.flags;

    /* ODD number is used for set, copy data using copy_from_user */
    pBuffer = mem_alloc_copy_from_user_helper(s_priv_data.pointer,
                                              s_priv_data.length);
    if (NULL == pBuffer)
    {
        VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
                  "mem_alloc_copy_from_user_helper fail");
        return -ENOMEM;
    }

    VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_INFO,
              "%s: Received length %d", __func__, s_priv_data.length);
    VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_INFO,
              "%s: Received data %s", __func__, pBuffer);

    switch(sub_cmd)
    {
    case WE_WOWL_ADD_PTRN:
        VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_INFO, "ADD_PTRN");
        hdd_add_wowl_ptrn(pAdapter, pBuffer);
        break;
    case WE_WOWL_DEL_PTRN:
        VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_INFO, "DEL_PTRN");
        hdd_del_wowl_ptrn(pAdapter, pBuffer);
        break;
    default:
        hddLog(LOGE, "%s: Invalid sub command %d", __func__, sub_cmd);
        ret = -EINVAL;
        break;
    }
    vos_mem_free(pBuffer);
    return ret;
}

int
static iw_softap_wowl_config_pattern(struct net_device *dev,
                                     struct iw_request_info *info,
                                     union iwreq_data *wrqu, char *extra)
{
	int ret;

	vos_ssr_protect(__func__);
	ret = __iw_softap_wowl_config_pattern(dev, info, wrqu, extra);
	vos_ssr_unprotect(__func__);

	return ret;
}

static void print_mac_list(v_MACADDR_t *macList, v_U8_t size)
{
    int i;
    v_BYTE_t *macArray;

    for (i = 0; i < size; i++) {
        macArray = (macList + i)->bytes;
        pr_info("** ACL entry %i - %02x:%02x:%02x:%02x:%02x:%02x \n",
                                          i, MAC_ADDR_ARRAY(macArray));
    }
    return;
}

static VOS_STATUS hdd_print_acl(hdd_adapter_t *pHostapdAdapter)
{
    eSapMacAddrACL acl_mode;
    v_MACADDR_t MacList[MAX_ACL_MAC_ADDRESS];
    v_U8_t listnum;
    v_PVOID_t pvosGCtx = NULL;

#ifdef WLAN_FEATURE_MBSSID
    pvosGCtx = WLAN_HDD_GET_SAP_CTX_PTR(pHostapdAdapter);
#else
    pvosGCtx = (WLAN_HDD_GET_CTX(pHostapdAdapter))->pvosContext;
#endif
    vos_mem_zero(&MacList[0], sizeof(MacList));
    if (VOS_STATUS_SUCCESS == WLANSAP_GetACLMode(pvosGCtx, &acl_mode)) {
        pr_info("******** ACL MODE *********\n");
        switch (acl_mode) {
        case eSAP_ACCEPT_UNLESS_DENIED:
            pr_info("ACL Mode = ACCEPT_UNLESS_DENIED\n");
            break;
        case eSAP_DENY_UNLESS_ACCEPTED:
            pr_info("ACL Mode = DENY_UNLESS_ACCEPTED\n");
            break;
        case eSAP_SUPPORT_ACCEPT_AND_DENY:
            pr_info("ACL Mode = ACCEPT_AND_DENY\n");
            break;
        case eSAP_ALLOW_ALL:
            pr_info("ACL Mode = ALLOW_ALL\n");
            break;
        default:
            pr_info("Invalid SAP ACL Mode = %d\n", acl_mode);
            return VOS_STATUS_E_FAILURE;
        }
    } else {
        return VOS_STATUS_E_FAILURE;
    }

    if (VOS_STATUS_SUCCESS == WLANSAP_GetACLAcceptList(pvosGCtx,
                                                       &MacList[0], &listnum)) {
        pr_info("******* WHITE LIST ***********\n");
        if (listnum <= MAX_ACL_MAC_ADDRESS)
            print_mac_list(&MacList[0], listnum);
    } else {
        return VOS_STATUS_E_FAILURE;
    }

    if (VOS_STATUS_SUCCESS == WLANSAP_GetACLDenyList(pvosGCtx,
                                                     &MacList[0], &listnum)) {
        pr_info("******* BLACK LIST ***********\n");
        if (listnum <= MAX_ACL_MAC_ADDRESS)
            print_mac_list(&MacList[0], listnum);
    } else {
        return VOS_STATUS_E_FAILURE;
    }
    return VOS_STATUS_SUCCESS;
}

int
static __iw_softap_setparam(struct net_device *dev,
                            struct iw_request_info *info,
                            union iwreq_data *wrqu, char *extra)
{
    hdd_adapter_t *pHostapdAdapter = (netdev_priv(dev));
    tHalHandle hHal;
    int *value = (int *)extra;
    int sub_cmd = value[0];
    int set_value = value[1];
    eHalStatus status;
    int ret = 0; /* success */
    v_CONTEXT_t pVosContext;
    hdd_context_t *pHddCtx = NULL;

    ENTER();

    if (NULL == pHostapdAdapter) {
       hddLog(LOGE, FL("hostapd Adapter is null"));
       return -EINVAL;
    }

    pHddCtx = WLAN_HDD_GET_CTX(pHostapdAdapter);
    ret = wlan_hdd_validate_context(pHddCtx);
    if (0 != ret)
        return -EINVAL;

    hHal = WLAN_HDD_GET_HAL_CTX(pHostapdAdapter);
    if (!hHal) {
       hddLog(LOGE, FL("Hal ctx is null"));
       return -EINVAL;
    }

    if (VOS_STATUS_SUCCESS != sme_is_session_valid(hHal,
                               pHostapdAdapter->sessionId)) {
       hddLog(LOGE, FL("session id is not valid %d"),
                   pHostapdAdapter->sessionId);
       return -EINVAL;
    }
    pVosContext = pHddCtx->pvosContext;
    if (!pVosContext) {
       hddLog(LOGE, FL("Vos ctx is null"));
       return -EINVAL;
    }

    switch(sub_cmd)
    {
        case QCASAP_SET_RADAR_DBG:
            hddLog(LOG1, FL("QCASAP_SET_RADAR_DBG called with: value: %d"),
                   set_value);
            sme_enable_phy_error_logs(hHal, (bool) set_value);
            break;

        case QCSAP_PARAM_CLR_ACL:
            if (VOS_STATUS_SUCCESS != WLANSAP_ClearACL(
#ifdef WLAN_FEATURE_MBSSID
                 WLAN_HDD_GET_SAP_CTX_PTR(pHostapdAdapter)
#else
                 pVosContext
#endif
            ))
            {
               ret = -EIO;
            }
            break;

        case QCSAP_PARAM_ACL_MODE:
            if ((eSAP_ALLOW_ALL < (eSapMacAddrACL)set_value) ||
                (eSAP_ACCEPT_UNLESS_DENIED > (eSapMacAddrACL)set_value))
            {
                hddLog(LOGE, FL("Invalid ACL Mode value %d"), set_value);
                ret = -EINVAL;
            }
            else
            {
#ifdef WLAN_FEATURE_MBSSID
                WLANSAP_SetMode(WLAN_HDD_GET_SAP_CTX_PTR(pHostapdAdapter), set_value);
#else
                WLANSAP_SetMode(pVosContext, set_value);
#endif

            }
            break;

        case QCSAP_PARAM_AUTO_CHANNEL:
            if (set_value == 0 || set_value == 1)
                (WLAN_HDD_GET_CTX(
                           pHostapdAdapter))->cfg_ini->force_sap_acs =
                                                                     set_value;
            else
                ret = -EINVAL;
            break;

        case QCSAP_PARAM_SET_CHANNEL_CHANGE:
		if ((WLAN_HDD_SOFTAP == pHostapdAdapter->device_mode)||
		   (WLAN_HDD_P2P_GO == pHostapdAdapter->device_mode)) {
			hddLog(LOG1, "SET Channel Change to new channel= %d",
					set_value);
			ret = hdd_softap_set_channel_change(dev, set_value);
		} else {
			hddLog(LOGE,
			  FL("Channel Change Failed, Device in test mode"));
			ret = -EINVAL;
		}
		break;

        case QCSAP_PARAM_MAX_ASSOC:
            if (WNI_CFG_ASSOC_STA_LIMIT_STAMIN > set_value)
            {
                hddLog(LOGE, FL("Invalid setMaxAssoc value %d"), set_value);
                ret = -EINVAL;
            }
            else
            {
                if (WNI_CFG_ASSOC_STA_LIMIT_STAMAX < set_value)
                {
                    hddLog(LOGW, FL("setMaxAssoc value %d higher than max allowed %d."
                                "Setting it to max allowed and continuing"),
                                set_value, WNI_CFG_ASSOC_STA_LIMIT_STAMAX);
                    set_value = WNI_CFG_ASSOC_STA_LIMIT_STAMAX;
                }
                status = ccmCfgSetInt(hHal, WNI_CFG_ASSOC_STA_LIMIT,
                                      set_value, NULL, eANI_BOOLEAN_FALSE);
                if ( status != eHAL_STATUS_SUCCESS )
                {
                    hddLog(LOGE, FL("setMaxAssoc failure, status %d"),
                            status);
                    ret = -EIO;
                }
            }
            break;

        case QCSAP_PARAM_HIDE_SSID:
            {
                eHalStatus status = eHAL_STATUS_SUCCESS;
                status = sme_HideSSID(hHal, pHostapdAdapter->sessionId, set_value);
                if(eHAL_STATUS_SUCCESS != status)
                {
                    hddLog(VOS_TRACE_LEVEL_ERROR,
                            "%s: QCSAP_PARAM_HIDE_SSID failed",
                            __func__);
                    return status;
                }
                break;
            }
        case QCSAP_PARAM_SET_MC_RATE:
            {
                tSirRateUpdateInd rateUpdate = {0};
                hdd_config_t *pConfig = pHddCtx->cfg_ini;

                hddLog(VOS_TRACE_LEVEL_INFO, "MC Target rate %d", set_value);
                memcpy(rateUpdate.bssid,
                       pHostapdAdapter->macAddressCurrent.bytes,
                       sizeof(tSirMacAddr));
                rateUpdate.nss = (pConfig->enable2x2 == 0) ? 0 : 1;
                rateUpdate.dev_mode = pHostapdAdapter->device_mode;
                rateUpdate.mcastDataRate24GHz = set_value;
                rateUpdate.mcastDataRate24GHzTxFlag = 1;
                rateUpdate.mcastDataRate5GHz = set_value;
                rateUpdate.bcastDataRate = -1;
                if (sme_SendRateUpdateInd(hHal, &rateUpdate) !=
                                                     eHAL_STATUS_SUCCESS) {
                    hddLog(VOS_TRACE_LEVEL_ERROR, "%s: SET_MC_RATE failed",
                                                                  __func__);
                    ret = -1;
                }
                break;
            }

         case QCSAP_PARAM_SET_TXRX_FW_STATS:
             {
                  hddLog(LOG1, "QCSAP_PARAM_SET_TXRX_FW_STATS val %d", set_value);
                  ret = process_wma_set_command((int)pHostapdAdapter->sessionId,
                                               (int)WMA_VDEV_TXRX_FWSTATS_ENABLE_CMDID,
                                               set_value, VDEV_CMD);
                  break;
             }
         /* Firmware debug log */
         case QCSAP_DBGLOG_LOG_LEVEL:
             {
                  hddLog(LOG1, "QCSAP_DBGLOG_LOG_LEVEL val %d", set_value);
                  ret = process_wma_set_command((int)pHostapdAdapter->sessionId,
                                               (int)WMI_DBGLOG_LOG_LEVEL,
                                               set_value, DBG_CMD);
                  break;
             }

         case QCSAP_DBGLOG_VAP_ENABLE:
             {
                  hddLog(LOG1, "QCSAP_DBGLOG_VAP_ENABLE val %d", set_value);
                  ret = process_wma_set_command((int)pHostapdAdapter->sessionId,
                                               (int)WMI_DBGLOG_VAP_ENABLE,
                                               set_value, DBG_CMD);
                  break;
             }

         case QCSAP_DBGLOG_VAP_DISABLE:
             {
                  hddLog(LOG1, "QCSAP_DBGLOG_VAP_DISABLE val %d", set_value);
                  ret = process_wma_set_command((int)pHostapdAdapter->sessionId,
                                               (int)WMI_DBGLOG_VAP_DISABLE,
                                               set_value, DBG_CMD);
                  break;
             }

         case QCSAP_DBGLOG_MODULE_ENABLE:
             {
                  hddLog(LOG1, "QCSAP_DBGLOG_MODULE_ENABLE val %d", set_value);
                  ret = process_wma_set_command((int)pHostapdAdapter->sessionId,
                                               (int)WMI_DBGLOG_MODULE_ENABLE,
                                               set_value, DBG_CMD);
                  break;
             }

         case QCSAP_DBGLOG_MODULE_DISABLE:
             {
                  hddLog(LOG1, "QCSAP_DBGLOG_MODULE_DISABLE val %d", set_value);
                  ret = process_wma_set_command((int)pHostapdAdapter->sessionId,
                                               (int)WMI_DBGLOG_MODULE_DISABLE,
                                               set_value, DBG_CMD);
                  break;
             }

         case QCSAP_DBGLOG_MOD_LOG_LEVEL:
             {
                  hddLog(LOG1, "QCSAP_DBGLOG_MOD_LOG_LEVEL val %d", set_value);
                  ret = process_wma_set_command((int)pHostapdAdapter->sessionId,
                                               (int)WMI_DBGLOG_MOD_LOG_LEVEL,
                                               set_value, DBG_CMD);
                  break;
             }

         case QCSAP_DBGLOG_TYPE:
             {
                  hddLog(LOG1, "QCSAP_DBGLOG_TYPE val %d", set_value);
                  ret = process_wma_set_command((int)pHostapdAdapter->sessionId,
                                               (int)WMI_DBGLOG_TYPE,
                                               set_value, DBG_CMD);
                  break;
             }
         case QCSAP_DBGLOG_REPORT_ENABLE:
             {
                  hddLog(LOG1, "QCSAP_DBGLOG_REPORT_ENABLE val %d", set_value);
                  ret = process_wma_set_command((int)pHostapdAdapter->sessionId,
                                               (int)WMI_DBGLOG_REPORT_ENABLE,
                                               set_value, DBG_CMD);
                  break;
             }
         case QCSAP_PARAM_SET_MCC_CHANNEL_LATENCY:
             {
                  tVOS_CONCURRENCY_MODE concurrent_state = 0;
                  v_U8_t first_adapter_operating_channel = 0;
                  int ret = 0; /* success */
                  hddLog(LOG1, "%s: iwpriv cmd to set MCC latency with val: "
                          "%dms", __func__, set_value);
                  concurrent_state = hdd_get_concurrency_mode();
                  /**
                   * Check if concurrency mode is active.
                   * Need to modify this code to support MCC modes other than
                   * STA/P2P GO
                   */
                  if (concurrent_state == (VOS_STA | VOS_P2P_GO))
                  {
                      hddLog(LOG1, "%s: STA & P2P are both enabled", __func__);
                      /**
                       * The channel number and latency are formatted in
                       * a bit vector then passed on to WMA layer.
                       +**********************************************+
                       | bits 31-16 | bits 15-8         |  bits 7-0   |
                       | Unused     | latency - Chan. 1 |  channel no.|
                       +**********************************************+
                       */

                      /* Get the operating channel of the designated vdev */
                      first_adapter_operating_channel =
                                    hdd_get_operating_channel
                                    (
                                    pHostapdAdapter->pHddCtx,
                                    pHostapdAdapter->device_mode
                                    );
                      /* Move the time latency for the adapter to bits 15-8 */
                      set_value = set_value << 8;
                      /* Store the channel number at bits 7-0 of the bit vector
                       * as per the bit format above.
                       */
                      set_value = set_value | first_adapter_operating_channel;
                      /* Send command to WMA */
                      ret = process_wma_set_command
                                        (
                                        (int)pHostapdAdapter->sessionId,
                                        (int)WMA_VDEV_MCC_SET_TIME_LATENCY,
                                        set_value, VDEV_CMD
                                        );
                  }
                  else
                  {
                      hddLog(LOG1, "%s: MCC is not active. Exit w/o setting"
                              " latency", __func__);
                  }
                  break;
             }

         case QCSAP_PARAM_SET_MCC_CHANNEL_QUOTA:
             {
                 hddLog(LOG1, "%s: iwpriv cmd to set MCC quota value %dms",
                         __func__, set_value);
                 ret = hdd_wlan_go_set_mcc_p2p_quota(pHostapdAdapter,
                                                     set_value);
                 break;
             }

         case QCASAP_TXRX_FWSTATS_RESET:
             {
                  hddLog(LOG1, "WE_TXRX_FWSTATS_RESET val %d", set_value);
                  if (set_value != WMA_FW_TXRX_FWSTATS_RESET) {
                      hddLog(LOGE, "Invalid arg %d in FWSTATS_RESET IOCTL",
                             set_value);
                      return -EINVAL;
                  }
                  ret = process_wma_set_command((int)pHostapdAdapter->sessionId,
                                                (int)WMA_VDEV_TXRX_FWSTATS_RESET_CMDID,
                                                set_value, VDEV_CMD);
                  break;
             }

         case QCSAP_PARAM_RTSCTS:
            {
                ret = process_wma_set_command((int)pHostapdAdapter->sessionId,
                                (int)WMI_VDEV_PARAM_ENABLE_RTSCTS,
                                set_value, VDEV_CMD);
                if (ret) {
                    hddLog(LOGE, "FAILED TO SET RTSCTS at SAP");
                    ret = -EIO;
                }
                break;
            }
        case QCASAP_SET_11N_RATE:
            {
                u_int8_t preamble = 0, nss = 0, rix = 0;
                tsap_Config_t *pConfig =
                        &pHostapdAdapter->sessionCtx.ap.sapConfig;

                hddLog(LOG1, "WMI_VDEV_PARAM_FIXED_RATE val %d", set_value);

                if (set_value != 0xff) {
                    rix = RC_2_RATE_IDX(set_value);
                    if (set_value & 0x80) {
                        if (pConfig->SapHw_mode == eCSR_DOT11_MODE_11b ||
                            pConfig->SapHw_mode == eCSR_DOT11_MODE_11b_ONLY ||
                            pConfig->SapHw_mode == eCSR_DOT11_MODE_11g ||
                            pConfig->SapHw_mode == eCSR_DOT11_MODE_11g_ONLY ||
                            pConfig->SapHw_mode == eCSR_DOT11_MODE_abg ||
                            pConfig->SapHw_mode == eCSR_DOT11_MODE_11a) {
                            hddLog(LOGE, "Not valid mode for HT");
                            ret = -EIO;
                            break;
                        }
                        preamble = WMI_RATE_PREAMBLE_HT;
                        nss = HT_RC_2_STREAMS(set_value) - 1;
                    } else if (set_value & 0x10) {
                        if (pConfig->SapHw_mode == eCSR_DOT11_MODE_11a) {
                            hddLog(VOS_TRACE_LEVEL_ERROR, "Not valid for cck");
                            ret = -EIO;
                            break;
                        }
                        preamble = WMI_RATE_PREAMBLE_CCK;
                        /* Enable Short preamble always for CCK except 1mbps */
                        if (rix != 0x3)
                            rix |= 0x4;
                    } else {
                        if (pConfig->SapHw_mode == eCSR_DOT11_MODE_11b ||
                            pConfig->SapHw_mode == eCSR_DOT11_MODE_11b_ONLY) {
                            hddLog(VOS_TRACE_LEVEL_ERROR, "Not valid for OFDM");
                            ret = -EIO;
                            break;
                        }
                        preamble = WMI_RATE_PREAMBLE_OFDM;
                    }

                    set_value = (preamble << 6) | (nss << 4) | rix;
                }
                hddLog(LOG1, "WMI_VDEV_PARAM_FIXED_RATE val %d rix %d "
                    "preamble %x nss %d", set_value, rix, preamble, nss);
                ret = process_wma_set_command((int)pHostapdAdapter->sessionId,
                                              (int)WMI_VDEV_PARAM_FIXED_RATE,
                                              set_value, VDEV_CMD);
                break;
            }

        case QCASAP_SET_VHT_RATE:
            {
                u_int8_t preamble = 0, nss = 0, rix = 0;
                tsap_Config_t *pConfig =
                    &pHostapdAdapter->sessionCtx.ap.sapConfig;

                if (pConfig->SapHw_mode != eCSR_DOT11_MODE_11ac &&
                    pConfig->SapHw_mode != eCSR_DOT11_MODE_11ac_ONLY) {
                    hddLog(VOS_TRACE_LEVEL_ERROR,
                        "%s: SET_VHT_RATE error: SapHw_mode= 0x%x, ch = %d",
                        __func__, pConfig->SapHw_mode, pConfig->channel);
                    ret = -EIO;
                    break;
                }

                if (set_value != 0xff) {
                    rix = RC_2_RATE_IDX_11AC(set_value);
                    preamble = WMI_RATE_PREAMBLE_VHT;
                    nss = HT_RC_2_STREAMS_11AC(set_value) - 1;

                    set_value = (preamble << 6) | (nss << 4) | rix;
                }
                hddLog(LOG1, "WMI_VDEV_PARAM_FIXED_RATE val %d rix %d "
                    "preamble %x nss %d", set_value, rix, preamble, nss);

                ret = process_wma_set_command((int)pHostapdAdapter->sessionId,
                                              (int)WMI_VDEV_PARAM_FIXED_RATE,
                                              set_value, VDEV_CMD);
                break;
            }

         case QCASAP_SHORT_GI:
             {
                  hddLog(LOG1, "QCASAP_SET_SHORT_GI val %d", set_value);

                  ret = sme_UpdateHTConfig(hHal, pHostapdAdapter->sessionId,
                                           WNI_CFG_HT_CAP_INFO_SHORT_GI_20MHZ, /* same as 40MHZ */
                                           set_value);
                  if (ret)
                      hddLog(LOGE, "Failed to set ShortGI value ret(%d)", ret);
                  break;
             }

         case QCSAP_SET_AMPDU:
             {
                  hddLog(LOG1, "QCSAP_SET_AMPDU val %d", set_value);
                  ret = process_wma_set_command((int)pHostapdAdapter->sessionId,
                                               (int)GEN_VDEV_PARAM_AMPDU,
                                               set_value, GEN_CMD);
                  break;
             }

         case QCSAP_SET_AMSDU:
             {
                  hddLog(LOG1, "QCSAP_SET_AMSDU val %d", set_value);
                  ret = process_wma_set_command((int)pHostapdAdapter->sessionId,
                                               (int)GEN_VDEV_PARAM_AMSDU,
                                               set_value, GEN_CMD);
                  break;
             }
        case QCSAP_GTX_HT_MCS:
             {
                  hddLog(LOG1, "WMI_VDEV_PARAM_GTX_HT_MCS %d", set_value);
                  ret = process_wma_set_command((int)pHostapdAdapter->sessionId,
                                         (int)WMI_VDEV_PARAM_GTX_HT_MCS,
                                         set_value, GTX_CMD);
                  break;
             }

        case QCSAP_GTX_VHT_MCS:
             {
                  hddLog(LOG1, "WMI_VDEV_PARAM_GTX_VHT_MCS %d", set_value);
                  ret = process_wma_set_command((int)pHostapdAdapter->sessionId,
                                         (int)WMI_VDEV_PARAM_GTX_VHT_MCS,
                                         set_value, GTX_CMD);
                  break;
             }

       case QCSAP_GTX_USRCFG:
             {
                  hddLog(LOG1, "WMI_VDEV_PARAM_GTX_USR_CFG %d", set_value);
                  ret = process_wma_set_command((int)pHostapdAdapter->sessionId,
                                         (int)WMI_VDEV_PARAM_GTX_USR_CFG,
                                         set_value, GTX_CMD);
                  break;
             }

        case QCSAP_GTX_THRE:
             {
                  hddLog(LOG1, "WMI_VDEV_PARAM_GTX_THRE %d", set_value);
                  ret = process_wma_set_command((int)pHostapdAdapter->sessionId,
                                         (int)WMI_VDEV_PARAM_GTX_THRE,
                                         set_value, GTX_CMD);
                  break;
             }

        case QCSAP_GTX_MARGIN:
             {
                  hddLog(LOG1, "WMI_VDEV_PARAM_GTX_MARGIN %d", set_value);
                  ret = process_wma_set_command((int)pHostapdAdapter->sessionId,
                                         (int)WMI_VDEV_PARAM_GTX_MARGIN,
                                         set_value, GTX_CMD);
                  break;
             }

        case QCSAP_GTX_STEP:
             {
                  hddLog(LOG1, "WMI_VDEV_PARAM_GTX_STEP %d", set_value);
                  ret = process_wma_set_command((int)pHostapdAdapter->sessionId,
                                         (int)WMI_VDEV_PARAM_GTX_STEP,
                                         set_value, GTX_CMD);
                  break;
             }

        case QCSAP_GTX_MINTPC:
             {
                  hddLog(LOG1, "WMI_VDEV_PARAM_GTX_MINTPC %d", set_value);
                  ret = process_wma_set_command((int)pHostapdAdapter->sessionId,
                                         (int)WMI_VDEV_PARAM_GTX_MINTPC,
                                         set_value, GTX_CMD);
                  break;
             }

        case QCSAP_GTX_BWMASK:
             {
                  hddLog(LOG1, "WMI_VDEV_PARAM_GTX_BWMASK %d", set_value);
                  ret = process_wma_set_command((int)pHostapdAdapter->sessionId,
                                         (int)WMI_VDEV_PARAM_GTX_BW_MASK,
                                         set_value, GTX_CMD);
                  break;
             }



#ifdef QCA_PKT_PROTO_TRACE
         case QCASAP_SET_DEBUG_LOG:
             {
                  hdd_context_t *pHddCtx = WLAN_HDD_GET_CTX(pHostapdAdapter);

                  hddLog(LOG1, "QCASAP_SET_DEBUG_LOG val %d", set_value);
                  /* Trace buffer dump only */
                  if (VOS_PKT_TRAC_DUMP_CMD == set_value)
                  {
                      vos_pkt_trace_buf_dump();
                      break;
                  }
                  pHddCtx->cfg_ini->gEnableDebugLog = set_value;
                  break;
             }
#endif /* QCA_PKT_PROTO_TRACE */

        case QCASAP_SET_TM_LEVEL:
             {
                  hddLog(VOS_TRACE_LEVEL_INFO, "Set Thermal Mitigation Level %d",
                            set_value);
                  (void)sme_SetThermalLevel(hHal, set_value);
                  break;
             }


        case QCASAP_SET_DFS_IGNORE_CAC:
             {
                  hddLog(VOS_TRACE_LEVEL_INFO, "Set Dfs ignore CAC  %d",
                            set_value);

                  if (pHostapdAdapter->device_mode != WLAN_HDD_SOFTAP)
                       return -EINVAL;

                  ret = WLANSAP_Set_Dfs_Ignore_CAC(hHal, set_value);
                  break;
             }

        case QCASAP_SET_DFS_TARGET_CHNL:
             {
                  hddLog(VOS_TRACE_LEVEL_INFO, "Set Dfs target channel  %d",
                            set_value);

                  if (pHostapdAdapter->device_mode != WLAN_HDD_SOFTAP)
                       return -EINVAL;

                  ret = WLANSAP_Set_Dfs_Target_Chnl(hHal, set_value);
                  break;
             }


        case QCASAP_SET_DFS_NOL:
             WLANSAP_Set_DfsNol(
#ifdef WLAN_FEATURE_MBSSID
                     WLAN_HDD_GET_SAP_CTX_PTR(pHostapdAdapter),
#else
                     pVosContext,
#endif
                     (eSapDfsNolType)set_value
                     );
             break;

        case QCASAP_SET_RADAR_CMD:
            {
                hdd_context_t *pHddCtx =
                    WLAN_HDD_GET_CTX(pHostapdAdapter);
                v_U8_t ch =
                    (WLAN_HDD_GET_AP_CTX_PTR(pHostapdAdapter))->operatingChannel;
                v_BOOL_t isDfsch;

                isDfsch = (NV_CHANNEL_DFS ==
                                vos_nv_getChannelEnabledState(ch));

                hddLog(VOS_TRACE_LEVEL_INFO,
                       FL("Set QCASAP_SET_RADAR_CMD val %d"), set_value);

                if (!pHddCtx->dfs_radar_found && isDfsch) {
                    ret = process_wma_set_command(
                            (int)pHostapdAdapter->sessionId,
                            (int)WMA_VDEV_DFS_CONTROL_CMDID,
                            set_value, VDEV_CMD);
                } else {
                    hddLog(VOS_TRACE_LEVEL_ERROR,
                        FL("Ignore command due to "
                            "dfs_radar_found: %d, is_dfs_channel: %d"),
                        pHddCtx->dfs_radar_found, isDfsch);
                }
                break;
            }
        case QCASAP_TX_CHAINMASK_CMD:
            {
                hddLog(LOG1, "QCASAP_TX_CHAINMASK_CMD val %d", set_value);
                ret = process_wma_set_command((int)pHostapdAdapter->sessionId,
                        (int)WMI_PDEV_PARAM_TX_CHAIN_MASK,
                        set_value, PDEV_CMD);
                break;
            }

        case QCASAP_RX_CHAINMASK_CMD:
            {
                hddLog(LOG1, "QCASAP_RX_CHAINMASK_CMD val %d", set_value);
                ret = process_wma_set_command((int)pHostapdAdapter->sessionId,
                        (int)WMI_PDEV_PARAM_RX_CHAIN_MASK,
                        set_value, PDEV_CMD);
                break;
            }

        case QCASAP_NSS_CMD:
            {
                hddLog(LOG1, "QCASAP_NSS_CMD val %d", set_value);
                ret = process_wma_set_command((int)pHostapdAdapter->sessionId,
                        (int)WMI_VDEV_PARAM_NSS,
                        set_value, VDEV_CMD);
                break;
            }
#ifdef IPA_UC_OFFLOAD
	case QCSAP_IPA_UC_STAT:
	    {
		    /* If input value is non-zero get stats */
		    if (1 == set_value) {
			    hdd_ipa_uc_stat_request(pHostapdAdapter, set_value);
		    } else if (3 == set_value) {
			    hdd_ipa_uc_rt_debug_host_dump(pHddCtx);
		    } else if (4 == set_value) {
			    hdd_ipa_dump_info(pHddCtx);
		    } else {
			    /* place holder for stats clean up
			     * Stats clean not implemented yet on FW and IPA
			     */
		    }

		    return ret;
	    }
#endif /* IPA_UC_OFFLOAD */
        case QCASAP_SET_PHYMODE:
            {
                hdd_context_t *phddctx = WLAN_HDD_GET_CTX(pHostapdAdapter);

                ret = wlan_hdd_update_phymode(dev, hHal, set_value, phddctx);
                break;
            }
        case QCASAP_DUMP_STATS:
            {
                hddLog(LOG1, "QCASAP_DUMP_STATS val %d", set_value);
                hdd_wlan_dump_stats(pHostapdAdapter, set_value);
                break;
            }
        case QCASAP_CLEAR_STATS:
            {
                hdd_context_t *hdd_ctx = WLAN_HDD_GET_CTX(pHostapdAdapter);

                hddLog(LOG1, FL("QCASAP_CLEAR_STATS val %d"), set_value);

                switch (set_value) {
                case WLAN_HDD_STATS:
                    memset(&pHostapdAdapter->stats, 0,
                                 sizeof(pHostapdAdapter->stats));
                    memset(&pHostapdAdapter->hdd_stats, 0,
                                 sizeof(pHostapdAdapter->hdd_stats));
                    break;
                case WLAN_HDD_NETIF_OPER_HISTORY:
                    wlan_hdd_clear_netif_queue_history(hdd_ctx);
                    break;
                default:
                    WLANTL_clear_datapath_stats(hdd_ctx->pvosContext,
                                                             set_value);
                }
                break;
            }

        case QCASAP_PARAM_LDPC:
            ret = hdd_set_ldpc(pHostapdAdapter, set_value);
            break;

        case QCASAP_PARAM_TX_STBC:
            ret = hdd_set_tx_stbc(pHostapdAdapter, set_value);
            break;

        case QCASAP_PARAM_RX_STBC:
            ret = hdd_set_rx_stbc(pHostapdAdapter, set_value);
            break;

        case QCSAP_SET_DEFAULT_AMPDU:
            hddLog(LOG1, "QCSAP_SET_DEFAULT_AMPDU val %d", set_value);
            ret = process_wma_set_command((int)pHostapdAdapter->sessionId,
                                         (int)WMI_PDEV_PARAM_MAX_MPDUS_IN_AMPDU,
                                         set_value, PDEV_CMD);
            break;

        case QCSAP_ENABLE_RTS_BURSTING:
            hddLog(LOG1, "QCSAP_ENABLE_RTS_BURSTING val %d", set_value);
            ret = process_wma_set_command((int)pHostapdAdapter->sessionId,
                                   (int)WMI_PDEV_PARAM_ENABLE_RTS_SIFS_BURSTING,
                                   set_value, PDEV_CMD);
            break;

        case QCSAP_ENABLE_DYNAMIC_BW:
           hddLog(LOG1, "QCSAP_ENABLE_DYNAMIC_BW val %d", set_value);
           ret = process_wma_set_command((int)pHostapdAdapter->sessionId,
                                         (int)WMI_PDEV_PARAM_DYNAMIC_BW,
                                         set_value, PDEV_CMD);
            break;

        default:
            hddLog(LOGE, FL("Invalid setparam command %d value %d"),
                    sub_cmd, set_value);
            ret = -EINVAL;
            break;
    }
    EXIT();
    return ret;
}

/**
 * __iw_softap_get_three() - return three value to upper layer.
 *
 * @dev: pointer of net_device of this wireless card
 * @info: meta data about Request sent
 * @wrqu: include request info
 * @extra: buf used for in/out
 *
 * Return: execute result
 */
static int __iw_softap_get_three(struct net_device *dev,
			struct iw_request_info *info,
			union iwreq_data *wrqu, char *extra)
{
	uint32_t *value = (uint32_t *)extra;
	uint32_t sub_cmd = value[0];
	int ret = 0; /* success */

	hdd_adapter_t *padapter = WLAN_HDD_GET_PRIV_PTR(dev);

	switch (sub_cmd) {
	case QCSAP_GET_TSF:
		ret = hdd_indicate_tsf(padapter, value, 3);
		break;
	default:
		hddLog(LOGE, FL("Invalid getparam command %d"), sub_cmd);
		break;
	}
	return ret;
}


/**
 * iw_softap_get_three() - return three value to upper layer.
 *
 * @dev: pointer of net_device of this wireless card
 * @info: meta data about Request sent
 * @wrqu: include request info
 * @extra: buf used for in/Output
 *
 * Return: execute result
 */
static int iw_softap_get_three(struct net_device *dev,
			struct iw_request_info *info,
			union iwreq_data *wrqu, char *extra)
{
	int ret;

	vos_ssr_protect(__func__);
	ret = __iw_softap_get_three(dev, info, wrqu, extra);
	vos_ssr_unprotect(__func__);

	return ret;
}


int
static iw_softap_setparam(struct net_device *dev,
                          struct iw_request_info *info,
                          union iwreq_data *wrqu, char *extra)
{
	int ret;

	vos_ssr_protect(__func__);
	ret = __iw_softap_setparam(dev, info, wrqu, extra);
	vos_ssr_unprotect(__func__);

	return ret;
}

int
static __iw_softap_getparam(struct net_device *dev,
                            struct iw_request_info *info,
                            union iwreq_data *wrqu, char *extra)
{
    hdd_adapter_t *pHostapdAdapter = (netdev_priv(dev));
    tHalHandle hHal = WLAN_HDD_GET_HAL_CTX(pHostapdAdapter);
    int *value = (int *)extra;
    int sub_cmd = value[0];
    eHalStatus status;
    int ret;
    hdd_context_t *pHddCtx;

    ENTER();

    pHddCtx = WLAN_HDD_GET_CTX(pHostapdAdapter);
    ret = wlan_hdd_validate_context(pHddCtx);
    if (0 != ret)
        return ret;

    if (VOS_STATUS_SUCCESS != sme_is_session_valid(hHal,
                               pHostapdAdapter->sessionId)) {
       hddLog(LOGE, FL("session id is not valid %d"),
                   pHostapdAdapter->sessionId);
       return -EINVAL;
    }
    switch (sub_cmd)
    {
    case QCSAP_PARAM_MAX_ASSOC:
        status = ccmCfgGetInt(hHal, WNI_CFG_ASSOC_STA_LIMIT, (tANI_U32 *)value);
        if (eHAL_STATUS_SUCCESS != status)
        {
            VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
                      FL("failed to get WNI_CFG_ASSOC_STA_LIMIT from cfg %d"),status);
            ret = -EIO;
        }
        break;

    case QCSAP_PARAM_GET_WLAN_DBG:
        {
            vos_trace_display();
            *value = 0;
            break;
        }

    case QCSAP_PARAM_AUTO_CHANNEL:
        {
            *value = (WLAN_HDD_GET_CTX
                      (pHostapdAdapter))->cfg_ini->force_sap_acs;
            break;
        }

    case QCSAP_PARAM_RTSCTS:
        {
            *value = wma_cli_get_command(pHddCtx->pvosContext,
                                             (int)pHostapdAdapter->sessionId,
                                             (int)WMI_VDEV_PARAM_ENABLE_RTSCTS,
                                             VDEV_CMD);
            break;
        }

    case QCASAP_SHORT_GI:
        {
            *value = (int)sme_GetHTConfig(hHal,
                                          pHostapdAdapter->sessionId,
                                          WNI_CFG_HT_CAP_INFO_SHORT_GI_20MHZ);
            break;
        }

    case QCSAP_GTX_HT_MCS:
        {
            hddLog(LOG1, "GET WMI_VDEV_PARAM_GTX_HT_MCS");
            *value = wma_cli_get_command(pHddCtx->pvosContext,
                                        (int)pHostapdAdapter->sessionId,
                                        (int)WMI_VDEV_PARAM_GTX_HT_MCS,
                                        GTX_CMD);
            break;
        }

    case QCSAP_GTX_VHT_MCS:
        {
            hddLog(LOG1, "GET WMI_VDEV_PARAM_GTX_VHT_MCS");
            *value = wma_cli_get_command(pHddCtx->pvosContext,
                                        (int)pHostapdAdapter->sessionId,
                                        (int)WMI_VDEV_PARAM_GTX_VHT_MCS,
                                        GTX_CMD);
            break;
        }

    case QCSAP_GTX_USRCFG:
        {
            hddLog(LOG1, "GET WMI_VDEV_PARAM_GTX_USR_CFG");
            *value = wma_cli_get_command(pHddCtx->pvosContext,
                                        (int)pHostapdAdapter->sessionId,
                                        (int)WMI_VDEV_PARAM_GTX_USR_CFG,
                                        GTX_CMD);
            break;
        }

    case QCSAP_GTX_THRE:
        {
            hddLog(LOG1, "GET WMI_VDEV_PARAM_GTX_THRE");
            *value = wma_cli_get_command(pHddCtx->pvosContext,
                                        (int)pHostapdAdapter->sessionId,
                                        (int)WMI_VDEV_PARAM_GTX_THRE,
                                        GTX_CMD);
            break;
        }

    case QCSAP_GTX_MARGIN:
        {
            hddLog(LOG1, "GET WMI_VDEV_PARAM_GTX_MARGIN");
            *value = wma_cli_get_command(pHddCtx->pvosContext,
                                        (int)pHostapdAdapter->sessionId,
                                        (int)WMI_VDEV_PARAM_GTX_MARGIN,
                                        GTX_CMD);
            break;
        }

    case QCSAP_GTX_STEP:
        {
            hddLog(LOG1, "GET WMI_VDEV_PARAM_GTX_STEP");
            *value = wma_cli_get_command(pHddCtx->pvosContext,
                                        (int)pHostapdAdapter->sessionId,
                                        (int)WMI_VDEV_PARAM_GTX_STEP,
                                        GTX_CMD);
            break;
        }

    case QCSAP_GTX_MINTPC:
        {
            hddLog(LOG1, "GET WMI_VDEV_PARAM_GTX_MINTPC");
            *value = wma_cli_get_command(pHddCtx->pvosContext,
                                        (int)pHostapdAdapter->sessionId,
                                        (int)WMI_VDEV_PARAM_GTX_MINTPC,
                                        GTX_CMD);
            break;
        }

    case QCSAP_GTX_BWMASK:
        {
            hddLog(LOG1, "GET WMI_VDEV_PARAM_GTX_BW_MASK");
            *value = wma_cli_get_command(pHddCtx->pvosContext,
                                        (int)pHostapdAdapter->sessionId,
                                        (int)WMI_VDEV_PARAM_GTX_BW_MASK,
                                        GTX_CMD);
            break;
        }

    case QCASAP_GET_DFS_NOL:
        {
            WLANSAP_Get_DfsNol(
#ifdef WLAN_FEATURE_MBSSID
                    WLAN_HDD_GET_SAP_CTX_PTR(pHostapdAdapter)
#else
                    pHddCtx->pvosContext
#endif
                    );
        }
        break;

    case QCSAP_GET_ACL:
        {
            hddLog(LOG1, FL("QCSAP_GET_ACL"));
            if (hdd_print_acl(pHostapdAdapter) != VOS_STATUS_SUCCESS) {
                hddLog(LOGE, FL("QCSAP_GET_ACL returned Error: not completed"));
            }
            *value = 0;
            break;
        }

    case QCASAP_TX_CHAINMASK_CMD:
        {
            hddLog(LOG1, "QCASAP_TX_CHAINMASK_CMD");
            *value = wma_cli_get_command(pHddCtx->pvosContext,
                    (int)pHostapdAdapter->sessionId,
                    (int)WMI_PDEV_PARAM_TX_CHAIN_MASK,
                    PDEV_CMD);
            break;
        }

    case QCASAP_RX_CHAINMASK_CMD:
        {
            hddLog(LOG1, "QCASAP_RX_CHAINMASK_CMD");
            *value = wma_cli_get_command(pHddCtx->pvosContext,
                    (int)pHostapdAdapter->sessionId,
                    (int)WMI_PDEV_PARAM_RX_CHAIN_MASK,
                    PDEV_CMD);
            break;
        }

    case QCASAP_NSS_CMD:
        {
            hddLog(LOG1, "QCASAP_NSS_CMD");
            *value = wma_cli_get_command(pHddCtx->pvosContext,
                        (int)pHostapdAdapter->sessionId,
                        (int)WMI_VDEV_PARAM_NSS,
                        VDEV_CMD);
            break;
        }
    case QCSAP_GET_DYNAMIC_BW:
        {
            *value = wma_cli_get_command(pHddCtx->pvosContext,
                                        (int)pHostapdAdapter->sessionId,
                                        (int)WMI_PDEV_PARAM_DYNAMIC_BW,
                                        PDEV_CMD);
            break;
        }
    case QCASAP_GET_TEMP_CMD:
        {
            hddLog(VOS_TRACE_LEVEL_INFO, "QCASAP_GET_TEMP_CMD");
            ret = wlan_hdd_get_temperature(pHostapdAdapter, wrqu, extra);
            break;
        }
    case QCSAP_GET_FW_STATUS:
        {
            hddLog(LOG1, "QCSAP_GET_FW_STATUS");
            *value = wlan_hdd_get_fw_state(pHostapdAdapter);
            break;
        }
    case QCSAP_CAP_TSF:
        {
            ret = hdd_capture_tsf(pHostapdAdapter, (uint32_t *)value, 1);
            break;
        }
    case QCASAP_PARAM_LDPC:
        ret = hdd_get_ldpc(pHostapdAdapter, value);
        break;

    case QCASAP_PARAM_TX_STBC:
        ret = hdd_get_tx_stbc(pHostapdAdapter, value);
        break;

    case QCASAP_PARAM_RX_STBC:
        ret = hdd_get_rx_stbc(pHostapdAdapter, value);
        break;

    case QCSAP_PARAM_CHAN_WIDTH:
        ret = hdd_sap_get_chan_width(pHostapdAdapter, value);
        break;
    default:
        hddLog(LOGE, FL("Invalid getparam command %d"), sub_cmd);
        ret = -EINVAL;
        break;
    }
    EXIT();
    return ret;
}

int
static iw_softap_getparam(struct net_device *dev,
                          struct iw_request_info *info,
                          union iwreq_data *wrqu, char *extra)
{
	int ret;

	vos_ssr_protect(__func__);
	ret = __iw_softap_getparam(dev, info, wrqu, extra);
	vos_ssr_unprotect(__func__);

	return ret;
}

/* Usage:
    BLACK_LIST  = 0
    WHITE_LIST  = 1
    ADD MAC = 0
    REMOVE MAC  = 1

    mac addr will be accepted as a 6 octet mac address with each octet inputted in hex
    for e.g. 00:0a:f5:11:22:33 will be represented as 0x00 0x0a 0xf5 0x11 0x22 0x33
    while using this ioctl

    Syntax:
    iwpriv softap.0 modify_acl
    <6 octet mac addr> <list type> <cmd type>

    Examples:
    eg 1. to add a mac addr 00:0a:f5:89:89:90 to the black list
    iwpriv softap.0 modify_acl 0x00 0x0a 0xf5 0x89 0x89 0x90 0 0
    eg 2. to delete a mac addr 00:0a:f5:89:89:90 from white list
    iwpriv softap.0 modify_acl 0x00 0x0a 0xf5 0x89 0x89 0x90 1 1
*/
static
int __iw_softap_modify_acl(struct net_device *dev, struct iw_request_info *info,
        union iwreq_data *wrqu, char *extra)
{
    hdd_adapter_t *pHostapdAdapter = (netdev_priv(dev));
    hdd_context_t *hdd_ctx = WLAN_HDD_GET_CTX(pHostapdAdapter);
#ifndef WLAN_FEATURE_MBSSID
    v_CONTEXT_t pVosContext = hdd_ctx->pvosContext;
#endif
    v_BYTE_t *value = (v_BYTE_t*)extra;
    v_U8_t pPeerStaMac[VOS_MAC_ADDR_SIZE];
    int listType, cmd, i;
    int ret;
    VOS_STATUS vos_status = VOS_STATUS_SUCCESS;

    ENTER();

    ret = wlan_hdd_validate_context(hdd_ctx);
    if (0 != ret)
        return ret;

#ifndef WLAN_FEATURE_MBSSID
    if (NULL == pVosContext) {
        VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
                  "%s: Vos Context is NULL", __func__);
        return -EINVAL;
    }
#endif

    for (i=0; i<VOS_MAC_ADDR_SIZE; i++)
    {
        pPeerStaMac[i] = *(value+i);
    }
    listType = (int)(*(value+i));
    i++;
    cmd = (int)(*(value+i));

    hddLog(LOG1, "%s: SAP Modify ACL arg0 " MAC_ADDRESS_STR " arg1 %d arg2 %d",
            __func__, MAC_ADDR_ARRAY(pPeerStaMac), listType, cmd);

#ifdef WLAN_FEATURE_MBSSID
    vos_status = WLANSAP_ModifyACL(WLAN_HDD_GET_SAP_CTX_PTR(pHostapdAdapter), pPeerStaMac,(eSapACLType)listType,(eSapACLCmdType)cmd);
#else
    vos_status = WLANSAP_ModifyACL(pVosContext, pPeerStaMac,(eSapACLType)listType,(eSapACLCmdType)cmd);
#endif
    if (!VOS_IS_STATUS_SUCCESS(vos_status))
    {
        hddLog(LOGE, FL("Modify ACL failed"));
        ret = -EIO;
    }
    EXIT();
    return ret;
}

static
int iw_softap_modify_acl(struct net_device *dev,
                         struct iw_request_info *info,
                         union iwreq_data *wrqu, char *extra)
{
	int ret;

	vos_ssr_protect(__func__);
	ret = __iw_softap_modify_acl(dev, info, wrqu, extra);
	vos_ssr_unprotect(__func__);

	return ret;
}

int
static __iw_softap_getchannel(struct net_device *dev,
                              struct iw_request_info *info,
                              union iwreq_data *wrqu, char *extra)
{
    hdd_adapter_t *pHostapdAdapter = (netdev_priv(dev));
    hdd_context_t *hdd_ctx;
    int ret;
    int *value = (int *)extra;

    ENTER();

    hdd_ctx = WLAN_HDD_GET_CTX(pHostapdAdapter);
    ret = wlan_hdd_validate_context(hdd_ctx);
    if (0 != ret)
        return ret;

    *value = 0;
    if (test_bit(SOFTAP_BSS_STARTED, &pHostapdAdapter->event_flags))
        *value = (WLAN_HDD_GET_AP_CTX_PTR(pHostapdAdapter))->operatingChannel;
    EXIT();
    return 0;
}

int
static iw_softap_getchannel(struct net_device *dev,
                            struct iw_request_info *info,
                            union iwreq_data *wrqu, char *extra)
{
	int ret;

	vos_ssr_protect(__func__);
	ret = __iw_softap_getchannel(dev, info, wrqu, extra);
	vos_ssr_unprotect(__func__);

	return ret;
}

int
static __iw_softap_set_max_tx_power(struct net_device *dev,
                        struct iw_request_info *info,
                        union iwreq_data *wrqu, char *extra)
{
    hdd_adapter_t *pHostapdAdapter = (netdev_priv(dev));
    hdd_context_t *hdd_ctx;
    tHalHandle hHal = WLAN_HDD_GET_HAL_CTX(pHostapdAdapter);
    int *value = (int *)extra;
    int set_value;
    tSirMacAddr bssid = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    tSirMacAddr selfMac = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    int ret;

    ENTER();

    if (NULL == value)
        return -ENOMEM;

    hdd_ctx = WLAN_HDD_GET_CTX(pHostapdAdapter);
    ret = wlan_hdd_validate_context(hdd_ctx);
    if (0 != ret)
        return ret;

    /* Assign correct slef MAC address */
    vos_mem_copy(bssid, pHostapdAdapter->macAddressCurrent.bytes,
                 VOS_MAC_ADDR_SIZE);
    vos_mem_copy(selfMac, pHostapdAdapter->macAddressCurrent.bytes,
                 VOS_MAC_ADDR_SIZE);

    set_value = value[0];
    if (eHAL_STATUS_SUCCESS != sme_SetMaxTxPower(hHal, bssid, selfMac, set_value))
    {
        hddLog(VOS_TRACE_LEVEL_ERROR, "%s: Setting maximum tx power failed",
                __func__);
        return -EIO;
    }
    EXIT();
    return 0;
}

int
static iw_softap_set_max_tx_power(struct net_device *dev,
                                  struct iw_request_info *info,
                                  union iwreq_data *wrqu, char *extra)
{
	int ret;

	vos_ssr_protect(__func__);
	ret = __iw_softap_set_max_tx_power(dev, info, wrqu, extra);
	vos_ssr_unprotect(__func__);

	return ret;
}

int
static __iw_display_data_path_snapshot(struct net_device *dev,
                        struct iw_request_info *info,
                        union iwreq_data *wrqu, char *extra)
{

    /* Function initiating dumping states of
     *  HDD(WMM Tx Queues)
     *  TL State (with Per Client infor)
     *  DXE Snapshot (Called at the end of TL Snapshot)
     */
    hdd_adapter_t *pHostapdAdapter = (netdev_priv(dev));

    ENTER();

    hdd_wmm_tx_snapshot(pHostapdAdapter);
    WLANTL_TLDebugMessage(VOS_TRUE);
    EXIT();
    return 0;
}

int
static iw_display_data_path_snapshot(struct net_device *dev,
                                     struct iw_request_info *info,
                                     union iwreq_data *wrqu, char *extra)
{
	int ret;

	vos_ssr_protect(__func__);
	ret = __iw_display_data_path_snapshot(dev, info, wrqu, extra);
	vos_ssr_unprotect(__func__);

	return ret;
}

int
static __iw_softap_set_tx_power(struct net_device *dev,
                        struct iw_request_info *info,
                        union iwreq_data *wrqu, char *extra)
{
    hdd_adapter_t *pHostapdAdapter = (netdev_priv(dev));
    tHalHandle hHal = WLAN_HDD_GET_HAL_CTX(pHostapdAdapter);
    hdd_context_t *hdd_ctx;
    int *value = (int *)extra;
    int set_value;
    tSirMacAddr bssid;
    int ret;

    ENTER();

    hdd_ctx = WLAN_HDD_GET_CTX(pHostapdAdapter);
    ret = wlan_hdd_validate_context(hdd_ctx);
    if (0 != ret)
        return ret;

    if (NULL == value)
        return -ENOMEM;

    vos_mem_copy(bssid, pHostapdAdapter->macAddressCurrent.bytes,
           VOS_MAC_ADDR_SIZE);

    set_value = value[0];
    if (eHAL_STATUS_SUCCESS != sme_SetTxPower(hHal, pHostapdAdapter->sessionId, bssid,
                                              pHostapdAdapter->device_mode,
                                              set_value))
    {
        hddLog(VOS_TRACE_LEVEL_ERROR, "%s: Setting tx power failed",
                __func__);
        return -EIO;
    }
    EXIT();
    return 0;
}

int
static iw_softap_set_tx_power(struct net_device *dev,
                              struct iw_request_info *info,
                              union iwreq_data *wrqu, char *extra)
{
	int ret;

	vos_ssr_protect(__func__);
	ret = __iw_softap_set_tx_power(dev, info, wrqu, extra);
	vos_ssr_unprotect(__func__);

	return ret;
}

#define IS_BROADCAST_MAC(x) (((x[0] & x[1] & x[2] & x[3] & x[4] & x[5]) == 0xff) ? 1 : 0)

int
static __iw_softap_getassoc_stamacaddr(struct net_device *dev,
                        struct iw_request_info *info,
                        union iwreq_data *wrqu, char *extra)
{
    hdd_adapter_t *pHostapdAdapter = (netdev_priv(dev));
    hdd_station_info_t *pStaInfo = pHostapdAdapter->aStaInfo;
    hdd_context_t *hdd_ctx;
    char *buf;
    int cnt = 0;
    int left;
    int ret;
    /* maclist_index must be u32 to match user space */
    u32 maclist_index;

    ENTER();

    hdd_ctx = WLAN_HDD_GET_CTX(pHostapdAdapter);
    ret = wlan_hdd_validate_context(hdd_ctx);
    if (0 != ret)
        return ret;

    /*
     * NOTE WELL: this is a "get" ioctl but it uses an even ioctl
     * number, and even numbered iocts are supposed to have "set"
     * semantics.  Hence the wireless extensions support in the kernel
     * won't correctly copy the result to user space, so the ioctl
     * handler itself must copy the data.  Output format is 32-bit
     * record length, followed by 0 or more 6-byte STA MAC addresses.
     *
     * Further note that due to the incorrect semantics, the "iwpriv"
     * user space application is unable to correctly invoke this API,
     * hence it is not registered in the hostapd_private_args.  This
     * API can only be invoked by directly invoking the ioctl() system
     * call.
     */

    /* Make sure user space allocated a reasonable buffer size */
    if (wrqu->data.length < sizeof(maclist_index)) {
        hddLog(LOG1, "%s: invalid userspace buffer", __func__);
        return -EINVAL;
    }

    /* allocate local buffer to build the response */
    buf = vos_mem_malloc(wrqu->data.length);
    if (!buf) {
        hddLog(LOG1, "%s: failed to allocate response buffer", __func__);
        return -ENOMEM;
    }

    /* start indexing beyond where the record count will be written */
    maclist_index = sizeof(maclist_index);
    left = wrqu->data.length - maclist_index;

    spin_lock_bh(&pHostapdAdapter->staInfo_lock);
    while ((cnt < WLAN_MAX_STA_COUNT) && (left >= VOS_MAC_ADDR_SIZE)) {
        if ((pStaInfo[cnt].isUsed) &&
            (!IS_BROADCAST_MAC(pStaInfo[cnt].macAddrSTA.bytes))) {
            memcpy(&buf[maclist_index], &(pStaInfo[cnt].macAddrSTA),
                   VOS_MAC_ADDR_SIZE);
            maclist_index += VOS_MAC_ADDR_SIZE;
            left -= VOS_MAC_ADDR_SIZE;
        }
        cnt++;
    }
    spin_unlock_bh(&pHostapdAdapter->staInfo_lock);

    *((u32 *)buf) = maclist_index;
    wrqu->data.length = maclist_index;
    if (copy_to_user(wrqu->data.pointer, buf, maclist_index)) {
        hddLog(LOG1, "%s: failed to copy response to user buffer", __func__);
        ret = -EFAULT;
    }
    vos_mem_free(buf);
    EXIT();
    return ret;
}

int
static iw_softap_getassoc_stamacaddr(struct net_device *dev,
                                   struct iw_request_info *info,
                                   union iwreq_data *wrqu, char *extra)
{
	int ret;

	vos_ssr_protect(__func__);
	ret = __iw_softap_getassoc_stamacaddr(dev, info, wrqu, extra);
	vos_ssr_unprotect(__func__);

	return ret;
}

/* Usage:
    mac addr will be accepted as a 6 octet mac address with each octet inputted in hex
    for e.g. 00:0a:f5:11:22:33 will be represented as 0x00 0x0a 0xf5 0x11 0x22 0x33
    while using this ioctl

    Syntax:
    iwpriv softap.0 disassoc_sta <6 octet mac address>

    e.g.
    disassociate sta with mac addr 00:0a:f5:11:22:33 from softap
    iwpriv softap.0 disassoc_sta 0x00 0x0a 0xf5 0x11 0x22 0x33
*/

int
static __iw_softap_disassoc_sta(struct net_device *dev,
                        struct iw_request_info *info,
                        union iwreq_data *wrqu, char *extra)
{
    hdd_adapter_t *pHostapdAdapter = (netdev_priv(dev));
    hdd_context_t *hdd_ctx;
    v_U8_t *peerMacAddr;
    struct tagCsrDelStaParams delStaParams;
    int ret;

    ENTER();

    if (!capable(CAP_NET_ADMIN)) {
        VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
                 FL("permission check failed"));
        return -EPERM;
    }

    hdd_ctx = WLAN_HDD_GET_CTX(pHostapdAdapter);
    ret = wlan_hdd_validate_context(hdd_ctx);
    if (0 != ret)
        return ret;

    /* iwpriv tool or framework calls this ioctl with
     * data passed in extra (less than 16 octets);
     */
    peerMacAddr = (v_U8_t *)(extra);

    hddLog(LOG1, "%s data "  MAC_ADDRESS_STR,
           __func__, MAC_ADDR_ARRAY(peerMacAddr));


    WLANSAP_PopulateDelStaParams(peerMacAddr,
                   eSIR_MAC_DEAUTH_LEAVING_BSS_REASON,
                   (SIR_MAC_MGMT_DISASSOC >> 4),
                   &delStaParams);

    hdd_softap_sta_disassoc(pHostapdAdapter, &delStaParams);
    EXIT();
    return 0;
}

int
static iw_softap_disassoc_sta(struct net_device *dev,
                              struct iw_request_info *info,
                              union iwreq_data *wrqu, char *extra)
{
	int ret;

	vos_ssr_protect(__func__);
	ret = __iw_softap_disassoc_sta(dev, info, wrqu, extra);
	vos_ssr_unprotect(__func__);

	return ret;
}

int
static __iw_softap_ap_stats(struct net_device *dev,
                        struct iw_request_info *info,
                        union iwreq_data *wrqu, char *extra)
{
    hdd_adapter_t *pHostapdAdapter = (netdev_priv(dev));
    WLANTL_TRANSFER_STA_TYPE  statBuffer;
    char *pstatbuf;
    int len;

    ENTER();

    memset(&statBuffer, 0, sizeof(statBuffer));
    WLANSAP_GetStatistics((WLAN_HDD_GET_CTX(pHostapdAdapter))->pvosContext,
                           &statBuffer, (v_BOOL_t)wrqu->data.flags);

    pstatbuf = vos_mem_malloc(wrqu->data.length);
    if(NULL == pstatbuf) {
        hddLog(LOG1, "unable to allocate memory");
        return -ENOMEM;
    }
    len = snprintf(pstatbuf, wrqu->data.length,
                    "RUF=%d RMF=%d RBF=%d "
                    "RUB=%d RMB=%d RBB=%d "
                    "TUF=%d TMF=%d TBF=%d "
                    "TUB=%d TMB=%d TBB=%d",
                    (int)statBuffer.rxUCFcnt, (int)statBuffer.rxMCFcnt,
                    (int)statBuffer.rxBCFcnt, (int)statBuffer.rxUCBcnt,
                    (int)statBuffer.rxMCBcnt, (int)statBuffer.rxBCBcnt,
                    (int)statBuffer.txUCFcnt, (int)statBuffer.txMCFcnt,
                    (int)statBuffer.txBCFcnt, (int)statBuffer.txUCBcnt,
                    (int)statBuffer.txMCBcnt, (int)statBuffer.txBCBcnt);

    if (len >= wrqu->data.length) {
        hddLog(LOG1, "%s: Insufficient buffer:%d, %d",
            __func__, wrqu->data.length, len);
        vos_mem_free(pstatbuf);
        return -E2BIG;
    }
    if (copy_to_user((void *)wrqu->data.pointer, (void *)pstatbuf, len)) {
        hddLog(LOG1, "%s: failed to copy data to user buffer", __func__);
        vos_mem_free(pstatbuf);
        return -EFAULT;
    }
    wrqu->data.length = len;
    vos_mem_free(pstatbuf);
    EXIT();
    return 0;
}

int
static iw_softap_ap_stats(struct net_device *dev,
                          struct iw_request_info *info,
                          union iwreq_data *wrqu, char *extra)
{
	int ret;

	vos_ssr_protect(__func__);
	ret = __iw_softap_ap_stats(dev, info, wrqu, extra);
	vos_ssr_unprotect(__func__);

	return ret;
}

int
static __iw_get_char_setnone(struct net_device *dev,
                             struct iw_request_info *info,
                             union iwreq_data *wrqu, char *extra)
{
    hdd_adapter_t *pAdapter = WLAN_HDD_GET_PRIV_PTR(dev);
    int sub_cmd = wrqu->data.flags;
    ENTER();
    if (NULL == WLAN_HDD_GET_CTX(pAdapter))
    {
        VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
                                        "%s: HDD Context is NULL!", __func__);

        return -EINVAL;
    }

    if ((WLAN_HDD_GET_CTX(pAdapter))->isLogpInProgress)
    {
        VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_FATAL,
                                  "%s:LOGP in Progress. Ignore!!!", __func__);
        return -EBUSY;
    }
    switch(sub_cmd)
    {
        case QCSAP_GET_STATS:
        {
            return hdd_wlan_get_stats(pAdapter, &(wrqu->data.length),
                               extra, WE_MAX_STR_LEN);
        }
    }
    return 0;
}

static int iw_get_char_setnone(struct net_device *dev,
                               struct iw_request_info *info,
                               union iwreq_data *wrqu, char *extra)
{
	int ret;

	vos_ssr_protect(__func__);
	ret = __iw_get_char_setnone(dev, info, wrqu, extra);
	vos_ssr_unprotect(__func__);

	return ret;
}

static int wlan_hdd_set_force_acs_ch_range(struct net_device *dev,
                          struct iw_request_info *info,
                          union iwreq_data *wrqu, char *extra)
{
	hdd_adapter_t *adapter = (netdev_priv(dev));
	hdd_context_t *hdd_ctx = WLAN_HDD_GET_CTX(adapter);
	int *value = (int *)extra;

	if (!capable(CAP_NET_ADMIN)) {
		VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
			  FL("permission check failed"));
		return -EPERM;
	}

	if (wlan_hdd_validate_operation_channel(adapter, value[0]) !=
					 VOS_STATUS_SUCCESS ||
		wlan_hdd_validate_operation_channel(adapter, value[1]) !=
					 VOS_STATUS_SUCCESS) {
		return -EINVAL;
	} else {
		hdd_ctx->cfg_ini->force_sap_acs_st_ch = value[0];
		hdd_ctx->cfg_ini->force_sap_acs_end_ch = value[1];
	}

	return 0;
}

static int iw_softap_set_force_acs_ch_range(struct net_device *dev,
                                       struct iw_request_info *info,
                                       union iwreq_data *wrqu, char *extra)
{
	int ret;
	vos_ssr_protect(__func__);
	ret = wlan_hdd_set_force_acs_ch_range(dev, info, wrqu, extra);
	vos_ssr_unprotect(__func__);
	return ret;
}

static int __iw_get_channel_list(struct net_device *dev,
                          struct iw_request_info *info,
                          union iwreq_data *wrqu, char *extra)
{
    v_U32_t num_channels = 0;
    v_U8_t i = 0;
    v_U8_t bandStartChannel = RF_CHAN_1;
    v_U8_t bandEndChannel = RF_CHAN_184;
    hdd_adapter_t *pHostapdAdapter = (netdev_priv(dev));
    tHalHandle hHal = WLAN_HDD_GET_HAL_CTX(pHostapdAdapter);
    tpChannelListInfo channel_list = (tpChannelListInfo) extra;
    eCsrBand curBand = eCSR_BAND_ALL;
    hdd_context_t *hdd_ctx;
    int ret;
    int is_dfs_mode_enabled = 0;

    ENTER();

    hdd_ctx = WLAN_HDD_GET_CTX(pHostapdAdapter);
    ret = wlan_hdd_validate_context(hdd_ctx);
    if (0 != ret)
        return ret;

    if (eHAL_STATUS_SUCCESS != sme_GetFreqBand(hHal, &curBand))
    {
        hddLog(LOGE,FL("not able get the current frequency band"));
        return -EIO;
    }
    wrqu->data.length = sizeof(tChannelListInfo);
    ENTER();

    if (eCSR_BAND_24 == curBand)
    {
        bandStartChannel = RF_CHAN_1;
        bandEndChannel = RF_CHAN_14;
    }
    else if (eCSR_BAND_5G == curBand)
    {
        bandStartChannel = RF_CHAN_36;
        bandEndChannel = RF_CHAN_184;
    }
    if (curBand != eCSR_BAND_24) {
        if (hdd_ctx->cfg_ini->dot11p_mode) {
            bandEndChannel = RF_CHAN_184;
        } else {
            bandEndChannel = RF_CHAN_165;
        }
    }

    if (pHostapdAdapter->device_mode == WLAN_HDD_INFRA_STATION &&
            hdd_ctx->cfg_ini->enableDFSChnlScan) {
        is_dfs_mode_enabled = 1;
    } else if (pHostapdAdapter->device_mode == WLAN_HDD_SOFTAP &&
            hdd_ctx->cfg_ini->enableDFSMasterCap) {
        is_dfs_mode_enabled = 1;
    }
    hddLog(LOG1, FL("curBand = %d, bandStartChannel = %hu, "
                "bandEndChannel = %hu is_dfs_mode_enabled  = %d "), curBand,
                bandStartChannel, bandEndChannel, is_dfs_mode_enabled);

    for( i = bandStartChannel; i <= bandEndChannel; i++ )
    {
        if ((NV_CHANNEL_ENABLE == regChannels[i].enabled) ||
            (is_dfs_mode_enabled &&
              NV_CHANNEL_DFS == regChannels[i].enabled))

        {
            channel_list->channels[num_channels] = rfChannels[i].channelNum;
            num_channels++;
        }
    }

    hddLog(LOG1,FL(" number of channels %d"), num_channels);

    channel_list->num_channels = num_channels;
    EXIT();

    return 0;
}

int iw_get_channel_list(struct net_device *dev,
                               struct iw_request_info *info,
                               union iwreq_data *wrqu, char *extra)
{
	int ret;

	vos_ssr_protect(__func__);
	ret = __iw_get_channel_list(dev, info, wrqu, extra);
	vos_ssr_unprotect(__func__);

	return ret;
}

static
int __iw_get_genie(struct net_device *dev,
                        struct iw_request_info *info,
                        union iwreq_data *wrqu, char *extra)
{
    hdd_adapter_t *pHostapdAdapter = (netdev_priv(dev));
    hdd_context_t *hdd_ctx;
    int ret;
#ifndef WLAN_FEATURE_MBSSID
    v_CONTEXT_t pVosContext;
#endif
    VOS_STATUS status;
    v_U32_t length = DOT11F_IE_RSN_MAX_LEN;
    v_U8_t genIeBytes[DOT11F_IE_RSN_MAX_LEN];

    ENTER();

    hdd_ctx = WLAN_HDD_GET_CTX(pHostapdAdapter);
    ret = wlan_hdd_validate_context(hdd_ctx);
    if (0 != ret)
        return ret;

#ifndef WLAN_FEATURE_MBSSID
    pVosContext = hdd_ctx->pvosContext;
    if (NULL == pVosContext) {
        VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
                  "%s: vos context is not valid ", __func__);
        return -EINVAL;
    }
#endif

    // Actually retrieve the RSN IE from CSR.  (We previously sent it down in the CSR Roam Profile.)
    status = WLANSap_getstationIE_information(
#ifdef WLAN_FEATURE_MBSSID
                                   WLAN_HDD_GET_SAP_CTX_PTR(pHostapdAdapter),
#else
                                   pVosContext,
#endif
                                   &length,
                                   genIeBytes
                                   );
    if (VOS_STATUS_SUCCESS != status) {
        hddLog(LOGE, FL("failed to get sta ies"));
        return -EFAULT;
    }

    wrqu->data.length = length;
    if (length > DOT11F_IE_RSN_MAX_LEN) {
        hddLog(LOGE,
               FL("invalid buffer length length:%d"), length);
        return -E2BIG;
    }

    vos_mem_copy(extra, genIeBytes, length);

    hddLog(LOG1, FL("RSN IE of %d bytes returned"), wrqu->data.length);


    EXIT();
    return 0;
}

static
int iw_get_genie(struct net_device *dev,
                 struct iw_request_info *info,
                 union iwreq_data *wrqu, char *extra)
{
	int ret;

	vos_ssr_protect(__func__);
	ret = __iw_get_genie(dev, info, wrqu, extra);
	vos_ssr_unprotect(__func__);

	return ret;
}

static
int __iw_get_WPSPBCProbeReqIEs(struct net_device *dev,
                        struct iw_request_info *info,
                        union iwreq_data *wrqu, char *extra)
{
    hdd_adapter_t *pHostapdAdapter = (netdev_priv(dev));
    sQcSapreq_WPSPBCProbeReqIES_t WPSPBCProbeReqIEs;
    hdd_ap_ctx_t *pHddApCtx = WLAN_HDD_GET_AP_CTX_PTR(pHostapdAdapter);
    hdd_context_t *hdd_ctx;
    int ret;

    ENTER();

    hdd_ctx = WLAN_HDD_GET_CTX(pHostapdAdapter);
    ret = wlan_hdd_validate_context(hdd_ctx);
    if (0 != ret)
        return ret;

    memset((void*)&WPSPBCProbeReqIEs, 0, sizeof(WPSPBCProbeReqIEs));

    WPSPBCProbeReqIEs.probeReqIELen = pHddApCtx->WPSPBCProbeReq.probeReqIELen;
    vos_mem_copy(&WPSPBCProbeReqIEs.probeReqIE,
                 pHddApCtx->WPSPBCProbeReq.probeReqIE,
                 WPSPBCProbeReqIEs.probeReqIELen);
    vos_mem_copy(&WPSPBCProbeReqIEs.macaddr,
                 pHddApCtx->WPSPBCProbeReq.peerMacAddr,
                 sizeof(v_MACADDR_t));
    if (copy_to_user(wrqu->data.pointer,
                     (void *)&WPSPBCProbeReqIEs,
                      sizeof(WPSPBCProbeReqIEs)))
    {
         hddLog(LOG1, "%s: failed to copy data to user buffer", __func__);
         return -EFAULT;
    }
    wrqu->data.length = 12 + WPSPBCProbeReqIEs.probeReqIELen;
    hddLog(LOG1, FL("Macaddress : "MAC_ADDRESS_STR),
           MAC_ADDR_ARRAY(WPSPBCProbeReqIEs.macaddr));
    up(&pHddApCtx->semWpsPBCOverlapInd);
    EXIT();
    return 0;
}

static
int iw_get_WPSPBCProbeReqIEs(struct net_device *dev,
                             struct iw_request_info *info,
                             union iwreq_data *wrqu, char *extra)
{
	int ret;

	vos_ssr_protect(__func__);
	ret = __iw_get_WPSPBCProbeReqIEs(dev, info, wrqu, extra);
	vos_ssr_unprotect(__func__);

	return ret;
}

/**
 * __iw_set_auth_hostap() - This function sets the auth type received
 *			from the wpa_supplicant.
 *
 * @dev - Pointer to the net device.
 * @info - Pointer to the iw_request_info.
 * @wrqu - Pointer to the iwreq_data.
 * @extra - Pointer to the data.
 *
 * Return: 0 for success, non zero for failure.
 */
static int
__iw_set_auth_hostap(struct net_device *dev,
			struct iw_request_info *info,
			union iwreq_data *wrqu, char *extra)
{
	hdd_adapter_t *pAdapter = WLAN_HDD_GET_PRIV_PTR(dev);
	hdd_wext_state_t *pWextState = WLAN_HDD_GET_WEXT_STATE_PTR(pAdapter);
	hdd_context_t *hdd_ctx;
	int ret;

	ENTER();

	hdd_ctx = WLAN_HDD_GET_CTX(pAdapter);
	ret = wlan_hdd_validate_context(hdd_ctx);
	if (0 != ret)
		return ret;

	switch (wrqu->param.flags & IW_AUTH_INDEX) {
	case IW_AUTH_TKIP_COUNTERMEASURES:
		if (wrqu->param.value) {
			hddLog(LOG2,
				FL("Counter Measure started(%d)"),
				wrqu->param.value);
			pWextState->mTKIPCounterMeasures =
						TKIP_COUNTER_MEASURE_STARTED;
		} else {
			hddLog(LOG2,
				FL("Counter Measure stopped(%d)"),
				wrqu->param.value);
			pWextState->mTKIPCounterMeasures =
						TKIP_COUNTER_MEASURE_STOPED;
		}

		hdd_softap_tkip_mic_fail_counter_measure(pAdapter,
							 wrqu->param.value);
		break;

	default:
		hddLog(LOGW, FL("called with unsupported auth type %d"),
			wrqu->param.flags & IW_AUTH_INDEX);
		break;
	}

	EXIT();
	return 0;
}

/**
 * iw_set_auth_hostap() - Wrapper function to protect __iw_set_auth_hostap
 *			from the SSR.
 *
 * @dev - Pointer to the net device.
 * @info - Pointer to the iw_request_info.
 * @wrqu - Pointer to the iwreq_data.
 * @extra - Pointer to the data.
 *
 * Return: 0 for success, non zero for failure.
 */
static int
iw_set_auth_hostap(struct net_device *dev,
			struct iw_request_info *info,
			union iwreq_data *wrqu, char *extra)
{
	int ret;

	vos_ssr_protect(__func__);
	ret = __iw_set_auth_hostap(dev, info, wrqu, extra);
	vos_ssr_unprotect(__func__);

	return ret;
}

/**
 * __iw_set_ap_encodeext() - set ap encode
 *
 * @dev - Pointer to the net device.
 * @info - Pointer to the iw_request_info.
 * @wrqu - Pointer to the iwreq_data.
 * @extra - Pointer to the data.
 *
 * Return: 0 for success, non zero for failure.
 */
static int __iw_set_ap_encodeext(struct net_device *dev,
				 struct iw_request_info *info,
				 union iwreq_data *wrqu, char *extra)
{
    hdd_adapter_t *pHostapdAdapter = (netdev_priv(dev));
#ifndef WLAN_FEATURE_MBSSID
    v_CONTEXT_t pVosContext;
#endif
    hdd_ap_ctx_t *pHddApCtx = WLAN_HDD_GET_AP_CTX_PTR(pHostapdAdapter);
    hdd_context_t *hdd_ctx;
    int ret;
    VOS_STATUS vstatus;
    struct iw_encode_ext *ext = (struct iw_encode_ext*)extra;
    v_U8_t groupmacaddr[VOS_MAC_ADDR_SIZE] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    int key_index;
    struct iw_point *encoding = &wrqu->encoding;
    tCsrRoamSetKey  setKey;
//    tCsrRoamRemoveKey RemoveKey;
    int i;

    ENTER();

    hdd_ctx = WLAN_HDD_GET_CTX(pHostapdAdapter);
    ret = wlan_hdd_validate_context(hdd_ctx);
    if (0 != ret)
        return ret;

#ifndef WLAN_FEATURE_MBSSID
    pVosContext = hdd_ctx->pvosContext;
    if (NULL == pVosContext) {
        VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
                  "%s: pVosContext is NULL", __func__);
        return -EINVAL;
    }
#endif

    key_index = encoding->flags & IW_ENCODE_INDEX;

    key_index = encoding->flags & IW_ENCODE_INDEX;

    if(key_index > 0) {

         /*Convert from 1-based to 0-based keying*/
        key_index--;
    }
    if(!ext->key_len || ext->key_len > CSR_MAX_KEY_LEN) {
#if 0
      /*Set the encryption type to NONE*/
#if 0
       pRoamProfile->EncryptionType.encryptionType[0] = eCSR_ENCRYPT_TYPE_NONE;
#endif

         RemoveKey.keyId = key_index;
         if(ext->ext_flags & IW_ENCODE_EXT_GROUP_KEY) {
              /*Key direction for group is RX only*/
             vos_mem_copy(RemoveKey.peerMac,groupmacaddr, VOS_MAC_ADDR_SIZE);
         }
         else {
             vos_mem_copy(RemoveKey.peerMac,ext->addr.sa_data, VOS_MAC_ADDR_SIZE);
         }
         switch(ext->alg)
         {
           case IW_ENCODE_ALG_NONE:
              RemoveKey.encType = eCSR_ENCRYPT_TYPE_NONE;
              break;
           case IW_ENCODE_ALG_WEP:
              RemoveKey.encType = (ext->key_len== 5) ? eCSR_ENCRYPT_TYPE_WEP40:eCSR_ENCRYPT_TYPE_WEP104;
              break;
           case IW_ENCODE_ALG_TKIP:
              RemoveKey.encType = eCSR_ENCRYPT_TYPE_TKIP;
              break;
           case IW_ENCODE_ALG_CCMP:
              RemoveKey.encType = eCSR_ENCRYPT_TYPE_AES;
              break;
          default:
              RemoveKey.encType = eCSR_ENCRYPT_TYPE_NONE;
              break;
         }
         VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_INFO, "%s: Remove key cipher_alg:%d key_len%d *pEncryptionType :%d",
                    __func__,(int)ext->alg,(int)ext->key_len,RemoveKey.encType);
         VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_INFO, "%s: Peer Mac = "MAC_ADDRESS_STR,
                    __func__, MAC_ADDR_ARRAY(RemoveKey.peerMac));
          );
#ifdef WLAN_FEATURE_MBSSID
         vstatus = WLANSAP_DelKeySta( WLAN_HDD_GET_SAP_CTX_PTR(pHostapdAdapter), &RemoveKey );
#else
         vstatus = WLANSAP_DelKeySta( pVosContext, &RemoveKey );
#endif

         if ( vstatus != VOS_STATUS_SUCCESS )
         {
             VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR, "[%4d] WLANSAP_DeleteKeysSta returned ERROR status= %d",
                        __LINE__, vstatus );
             retval = -EINVAL;
         }
#endif
         return -EINVAL;

    }

    vos_mem_zero(&setKey,sizeof(tCsrRoamSetKey));

    setKey.keyId = key_index;
    setKey.keyLength = ext->key_len;

    vos_mem_copy(&setKey.Key[0],ext->key,ext->key_len);

    if(ext->ext_flags & IW_ENCODE_EXT_GROUP_KEY) {
      /*Key direction for group is RX only*/
       setKey.keyDirection = eSIR_RX_ONLY;
       vos_mem_copy(setKey.peerMac,groupmacaddr, VOS_MAC_ADDR_SIZE);
    }
    else {

       setKey.keyDirection =  eSIR_TX_RX;
       vos_mem_copy(setKey.peerMac,ext->addr.sa_data, VOS_MAC_ADDR_SIZE);
    }
    if(ext->ext_flags & IW_ENCODE_EXT_SET_TX_KEY)
    {
       setKey.keyDirection = eSIR_TX_DEFAULT;
       vos_mem_copy(setKey.peerMac,ext->addr.sa_data, VOS_MAC_ADDR_SIZE);
    }

    /*For supplicant pae role is zero*/
    setKey.paeRole = 0;

    switch(ext->alg)
    {
       case IW_ENCODE_ALG_NONE:
         setKey.encType = eCSR_ENCRYPT_TYPE_NONE;
         break;

       case IW_ENCODE_ALG_WEP:
         setKey.encType = (ext->key_len== 5) ? eCSR_ENCRYPT_TYPE_WEP40:eCSR_ENCRYPT_TYPE_WEP104;
         pHddApCtx->uPrivacy = 1;
         hddLog(LOG1, "(%s) uPrivacy=%d", __func__, pHddApCtx->uPrivacy);
         break;

       case IW_ENCODE_ALG_TKIP:
       {
          v_U8_t *pKey = &setKey.Key[0];

          setKey.encType = eCSR_ENCRYPT_TYPE_TKIP;

          vos_mem_zero(pKey, CSR_MAX_KEY_LEN);

          /*Supplicant sends the 32bytes key in this order

                |--------------|----------|----------|
                |   Tk1        |TX-MIC    |  RX Mic  |
                |--------------|----------|----------|
                <---16bytes---><--8bytes--><--8bytes-->

                */
          /*Sme expects the 32 bytes key to be in the below order

                |--------------|----------|----------|
                |   Tk1        |RX-MIC    |  TX Mic  |
                |--------------|----------|----------|
                <---16bytes---><--8bytes--><--8bytes-->
               */
          /* Copy the Temporal Key 1 (TK1) */
          vos_mem_copy(pKey,ext->key,16);

         /*Copy the rx mic first*/
          vos_mem_copy(&pKey[16],&ext->key[24],8);

         /*Copy the tx mic */
          vos_mem_copy(&pKey[24],&ext->key[16],8);

       }
       break;

       case IW_ENCODE_ALG_CCMP:
          setKey.encType = eCSR_ENCRYPT_TYPE_AES;
          break;

       default:
          setKey.encType = eCSR_ENCRYPT_TYPE_NONE;
          break;
    }

    VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_INFO,
          ("%s:EncryptionType:%d key_len:%d, KeyId:%d"), __func__, setKey.encType, setKey.keyLength,
            setKey.keyId);
    for(i=0; i< ext->key_len; i++)
        VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_INFO,
          ("%02x"), setKey.Key[i]);

#ifdef WLAN_FEATURE_MBSSID
    vstatus = WLANSAP_SetKeySta( WLAN_HDD_GET_SAP_CTX_PTR(pHostapdAdapter), &setKey );
#else
    vstatus = WLANSAP_SetKeySta( pVosContext, &setKey );
#endif

    if ( vstatus != VOS_STATUS_SUCCESS )
    {
       VOS_TRACE( VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
                   "[%4d] WLANSAP_SetKeySta returned ERROR status= %d", __LINE__, vstatus );
       ret = -EINVAL;
    }

    EXIT();
    return ret;
}

/**
 * iw_set_ap_encodeext() - Wrapper function to protect __iw_set_ap_encodeext
 *			from the SSR.
 *
 * @dev - Pointer to the net device.
 * @info - Pointer to the iw_request_info.
 * @wrqu - Pointer to the iwreq_data.
 * @extra - Pointer to the data.
 *
 * Return: 0 for success, non zero for failure.
 */
static int iw_set_ap_encodeext(struct net_device *dev,
                               struct iw_request_info *info,
                               union iwreq_data *wrqu, char *extra)
{
	int ret;

	vos_ssr_protect(__func__);
	ret = __iw_set_ap_encodeext(dev, info, wrqu, extra);
	vos_ssr_unprotect(__func__);

	return ret;
}

/**
 * __iw_set_ap_mlme() - set ap mlme
 * @dev: pointer to net_device
 * @info: pointer to iw_request_info
 * @wrqu; pointer to iwreq_data
 * @extra: extra
 *
 * Return; 0 on success, error number otherwise
 */
static int __iw_set_ap_mlme(struct net_device *dev,
			    struct iw_request_info *info,
			    union iwreq_data *wrqu,
			    char *extra)
{
    return 0;
//    return status;
}

/**
 * iw_set_ap_mlme() - SSR wrapper for __iw_set_ap_mlme
 * @dev: pointer to net_device
 * @info: pointer to iw_request_info
 * @wrqu; pointer to iwreq_data
 * @extra: extra
 *
 * Return; 0 on success, error number otherwise
 */
static int iw_set_ap_mlme(struct net_device *dev,
			  struct iw_request_info *info,
			  union iwreq_data *wrqu,
			  char *extra)
{
	int ret;

	vos_ssr_protect(__func__);
	ret = __iw_set_ap_mlme(dev, info, wrqu, extra);
	vos_ssr_unprotect(__func__);

	return ret;
}


/**
 * __iw_get_ap_rts_threshold() - get ap rts threshold
 * @dev - Pointer to the net device.
 * @info - Pointer to the iw_request_info.
 * @wrqu - Pointer to the iwreq_data.
 * @extra - Pointer to the data.
 *
 * Return: 0 for success, non zero for failure.
 */
static int __iw_get_ap_rts_threshold(struct net_device *dev,
				     struct iw_request_info *info,
				     union iwreq_data *wrqu, char *extra)
{
	hdd_adapter_t *pHostapdAdapter = netdev_priv(dev);
	int ret;
	hdd_context_t *hdd_ctx;

	ENTER();

	hdd_ctx = WLAN_HDD_GET_CTX(pHostapdAdapter);
	ret = wlan_hdd_validate_context(hdd_ctx);
	if (0 != ret)
		return ret;

	ret = hdd_wlan_get_rts_threshold(pHostapdAdapter, wrqu);

	return ret;
}

/**
 * iw_get_ap_rts_threshold() - Wrapper function to protect
 *			__iw_get_ap_rts_threshold from the SSR.
 * @dev - Pointer to the net device.
 * @info - Pointer to the iw_request_info.
 * @wrqu - Pointer to the iwreq_data.
 * @extra - Pointer to the data.
 *
 * Return: 0 for success, non zero for failure.
 */
static int iw_get_ap_rts_threshold(struct net_device *dev,
				   struct iw_request_info *info,
				   union iwreq_data *wrqu, char *extra)
{
	int ret;

	vos_ssr_protect(__func__);
	ret = __iw_get_ap_rts_threshold(dev, info, wrqu, extra);
	vos_ssr_unprotect(__func__);

	return ret;
}

/**
 * __iw_get_ap_frag_threshold() - get ap fragmentation threshold
 * @dev - Pointer to the net device.
 * @info - Pointer to the iw_request_info.
 * @wrqu - Pointer to the iwreq_data.
 * @extra - Pointer to the data.
 *
 * Return: 0 for success, non zero for failure.
 */
static int __iw_get_ap_frag_threshold(struct net_device *dev,
				      struct iw_request_info *info,
				      union iwreq_data *wrqu, char *extra)
{
	hdd_adapter_t *pHostapdAdapter = netdev_priv(dev);
	hdd_context_t *hdd_ctx;
	int ret = 0;

	ENTER();

	hdd_ctx = WLAN_HDD_GET_CTX(pHostapdAdapter);
	ret = wlan_hdd_validate_context(hdd_ctx);
	if (0 != ret)
		return ret;

	ret = hdd_wlan_get_frag_threshold(pHostapdAdapter, wrqu);

	return ret;
}

/**
 * iw_get_ap_frag_threshold() - Wrapper function to protect
 *			__iw_get_ap_frag_threshold from the SSR.
 * @dev - Pointer to the net device.
 * @info - Pointer to the iw_request_info.
 * @wrqu - Pointer to the iwreq_data.
 * @extra - Pointer to the data.
 *
 * Return: 0 for success, non zero for failure.
 */
static int iw_get_ap_frag_threshold(struct net_device *dev,
				    struct iw_request_info *info,
				    union iwreq_data *wrqu, char *extra)
{
	int ret;

	vos_ssr_protect(__func__);
	ret = __iw_get_ap_frag_threshold(dev, info, wrqu, extra);
	vos_ssr_unprotect(__func__);

	return ret;
}

/**
 * __iw_get_ap_freq() - get ap frequency
 * @dev - Pointer to the net device.
 * @info - Pointer to the iw_request_info.
 * @wrqu - Pointer to the iwreq_data.
 * @extra - Pointer to the data.
 *
 * Return: 0 for success, non zero for failure.
 */
static int __iw_get_ap_freq(struct net_device *dev,
                            struct iw_request_info *info,
                            struct iw_freq *fwrq, char *extra)
{
   v_U32_t status = FALSE, channel = 0, freq = 0;
   hdd_adapter_t *pHostapdAdapter = netdev_priv(dev);
   hdd_context_t *hdd_ctx;
   tHalHandle hHal;
   hdd_hostapd_state_t *pHostapdState;
   hdd_ap_ctx_t *pHddApCtx = WLAN_HDD_GET_AP_CTX_PTR(pHostapdAdapter);
   int ret;

   ENTER();

   hdd_ctx = WLAN_HDD_GET_CTX(pHostapdAdapter);
   ret = wlan_hdd_validate_context(hdd_ctx);
   if (0 != ret)
       return ret;

   pHostapdState = WLAN_HDD_GET_HOSTAP_STATE_PTR(pHostapdAdapter);
   hHal = WLAN_HDD_GET_HAL_CTX(pHostapdAdapter);

   if(pHostapdState->bssState == BSS_STOP )
   {
       if (ccmCfgGetInt(hHal, WNI_CFG_CURRENT_CHANNEL, &channel)
                                                  != eHAL_STATUS_SUCCESS)
       {
           return -EIO;
       }
       else
       {
          status = hdd_wlan_get_freq(channel, &freq);
          if( TRUE == status)
          {
              /* Set Exponent parameter as 6 (MHZ) in struct iw_freq
               * iwlist & iwconfig command shows frequency into proper
               * format (2.412 GHz instead of 246.2 MHz)*/
              fwrq->m = freq;
              fwrq->e = MHZ;
          }
       }
    }
    else
    {
       channel = pHddApCtx->operatingChannel;
       status = hdd_wlan_get_freq(channel, &freq);
       if( TRUE == status)
       {
          /* Set Exponent parameter as 6 (MHZ) in struct iw_freq
           * iwlist & iwconfig command shows frequency into proper
           * format (2.412 GHz instead of 246.2 MHz)*/
           fwrq->m = freq;
           fwrq->e = MHZ;
       }
    }
    EXIT();
    return 0;
}

/**
 * iw_get_ap_freq() - Wrapper function to protect
 *                    __iw_get_ap_freq from the SSR.
 * @dev - Pointer to the net device.
 * @info - Pointer to the iw_request_info.
 * @wrqu - Pointer to the iwreq_data.
 * @extra - Pointer to the data.
 *
 * Return: 0 for success, non zero for failure.
 */
static int iw_get_ap_freq(struct net_device *dev,
			  struct iw_request_info *info,
			  struct iw_freq *wrqu, char *extra)
{
	int ret;

	vos_ssr_protect(__func__);
	ret = __iw_get_ap_freq(dev, info, wrqu, extra);
	vos_ssr_unprotect(__func__);

	return ret;
}

static int __iw_softap_stopbss(struct net_device *dev,
                             struct iw_request_info *info,
                             union iwreq_data *wrqu,
                             char *extra)
{
    hdd_adapter_t *pHostapdAdapter = (netdev_priv(dev));
    VOS_STATUS status = VOS_STATUS_SUCCESS;
    hdd_context_t *pHddCtx;

    ENTER();

    pHddCtx = WLAN_HDD_GET_CTX(pHostapdAdapter);
    status = wlan_hdd_validate_context(pHddCtx);
    if (0 != status)
        return status;

    if(test_bit(SOFTAP_BSS_STARTED, &pHostapdAdapter->event_flags))
    {
        hdd_hostapd_state_t *pHostapdState =
                       WLAN_HDD_GET_HOSTAP_STATE_PTR(pHostapdAdapter);
        vos_event_reset(&pHostapdState->stop_bss_event);
#ifdef WLAN_FEATURE_MBSSID
        status = WLANSAP_StopBss(WLAN_HDD_GET_SAP_CTX_PTR(pHostapdAdapter));
#else
        status = WLANSAP_StopBss((WLAN_HDD_GET_CTX(pHostapdAdapter))->pvosContext);
#endif
        if (VOS_IS_STATUS_SUCCESS(status))
        {
            status = vos_wait_single_event(&pHostapdState->stop_bss_event,
                                           10000);
            if (!VOS_IS_STATUS_SUCCESS(status))
            {
                VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
                         ("%s: ERROR: HDD vos wait for single_event failed!!"),
                         __func__);
                VOS_ASSERT(0);
            }
        }
        clear_bit(SOFTAP_BSS_STARTED, &pHostapdAdapter->event_flags);
        wlan_hdd_decr_active_session(pHddCtx, pHostapdAdapter->device_mode);
    }
    if (pHddCtx->cfg_ini->apOBSSProtEnabled)
        vos_runtime_pm_allow_suspend(pHddCtx->runtime_context.obss);
    EXIT();
    return (status == VOS_STATUS_SUCCESS) ? 0 : -EBUSY;
}

static int iw_softap_stopbss(struct net_device *dev,
                             struct iw_request_info *info,
                             union iwreq_data *wrqu,
                             char *extra)
{
	int ret;

	vos_ssr_protect(__func__);
	ret = __iw_softap_stopbss(dev, info, wrqu, extra);
	vos_ssr_unprotect(__func__);

	return ret;
}

static int __iw_softap_version(struct net_device *dev,
                             struct iw_request_info *info,
                             union iwreq_data *wrqu,
                             char *extra)
{
    hdd_adapter_t *pHostapdAdapter = netdev_priv(dev);
    hdd_context_t *hdd_ctx;
    int ret;

    ENTER();

    hdd_ctx = WLAN_HDD_GET_CTX(pHostapdAdapter);
    ret = wlan_hdd_validate_context(hdd_ctx);
    if (0 != ret)
        return ret;

    hdd_wlan_get_version(pHostapdAdapter, wrqu, extra);
    EXIT();

    return ret;
}

static int iw_softap_version(struct net_device *dev,
                             struct iw_request_info *info,
                             union iwreq_data *wrqu,
                             char *extra)
{
	int ret;

	vos_ssr_protect(__func__);
	ret = __iw_softap_version(dev, info, wrqu, extra);
	vos_ssr_unprotect(__func__);

	return ret;
}

static int
hdd_softap_get_sta_info(hdd_adapter_t *pAdapter, v_U8_t *pBuf, int buf_len)
{
    v_U8_t i;
    v_U8_t maxSta = 0;
    int len = 0;
    const char sta_info_header[] = "staId staAddress";
    hdd_context_t *pHddCtx;
    int ret;

    ENTER();

    if (NULL == pAdapter) {
        VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
                  "%s: Adapter is NULL", __func__);
        return -EINVAL;
    }

    pHddCtx = WLAN_HDD_GET_CTX(pAdapter);
    ret = wlan_hdd_validate_context(pHddCtx);
    if (0 != ret)
        return ret;

    len = snprintf(pBuf, buf_len, sta_info_header);
    if (len >= buf_len) {
        hddLog(LOGE, FL("Insufficient buffer:%d, %d"), buf_len, len);
        return -E2BIG;
    }
    pBuf += len;
    buf_len -= len;

    maxSta = pHddCtx->max_peers;

    for (i = 0; i <= maxSta; i++)
    {
        if (!pAdapter->aStaInfo[i].isUsed)
                continue;

        if (CHAN_HOP_ALL_BANDS_ENABLE &&
            (i == (WLAN_HDD_GET_AP_CTX_PTR(pAdapter))->uBCStaId))
                continue;

        if (WE_GET_STA_INFO_SIZE > buf_len)
                break;

        len = scnprintf(pBuf, buf_len,
                        "%d: %02x:%02x:%02x:%02x:%02x:%02x \t ecsa=%d\n",
                        pAdapter->aStaInfo[i].ucSTAId,
                        pAdapter->aStaInfo[i].macAddrSTA.bytes[0],
                        pAdapter->aStaInfo[i].macAddrSTA.bytes[1],
                        pAdapter->aStaInfo[i].macAddrSTA.bytes[2],
                        pAdapter->aStaInfo[i].macAddrSTA.bytes[3],
                        pAdapter->aStaInfo[i].macAddrSTA.bytes[4],
                        pAdapter->aStaInfo[i].macAddrSTA.bytes[5],
                        pAdapter->aStaInfo[i].ecsa_capable);

        if (len >= buf_len) {
                hddLog(LOGE, FL("Insufficient buffer:%d, %d"), buf_len, len);
                return -E2BIG;
        }
        pBuf += len;
        buf_len -= len;
    }
    EXIT();
    return 0;
}

static int __iw_softap_get_sta_info(struct net_device *dev,
                                    struct iw_request_info *info,
                                    union iwreq_data *wrqu,
                                    char *extra)
{
    hdd_adapter_t *pHostapdAdapter = (netdev_priv(dev));
    hdd_context_t *hdd_ctx;
    int ret;

    ENTER();

    hdd_ctx = WLAN_HDD_GET_CTX(pHostapdAdapter);
    ret = wlan_hdd_validate_context(hdd_ctx);
    if (0 != ret)
        return ret;

    ret = hdd_softap_get_sta_info(pHostapdAdapter, extra, WE_SAP_MAX_STA_INFO);
    if (ret) {
       hddLog(VOS_TRACE_LEVEL_ERROR, "%s Failed!!!",__func__);
       return ret;
    }
    wrqu->data.length = strlen(extra);
    EXIT();
    return 0;
}

static int iw_softap_get_sta_info(struct net_device *dev,
                                  struct iw_request_info *info,
                                  union iwreq_data *wrqu,
                                  char *extra)
{
	int ret;

	vos_ssr_protect(__func__);
	ret = __iw_softap_get_sta_info(dev, info, wrqu, extra);
	vos_ssr_unprotect(__func__);

	return ret;
}

/**
 * __iw_set_ap_genie() - set ap wpa/rsn ie
 *
 * @dev - Pointer to the net device.
 * @info - Pointer to the iw_request_info.
 * @wrqu - Pointer to the iwreq_data.
 * @extra - Pointer to the data.
 *
 * Return: 0 for success, non zero for failure.
 */
static int __iw_set_ap_genie(struct net_device *dev,
			     struct iw_request_info *info,
			     union iwreq_data *wrqu,
			     char *extra)
{

    hdd_adapter_t *pHostapdAdapter = (netdev_priv(dev));
#ifndef WLAN_FEATURE_MBSSID
    v_CONTEXT_t pVosContext;
#endif
    eHalStatus halStatus= eHAL_STATUS_SUCCESS;
    u_int8_t *genie = (u_int8_t *)extra;
    hdd_context_t *hdd_ctx;
    int ret;

    ENTER();

    hdd_ctx = WLAN_HDD_GET_CTX(pHostapdAdapter);
    ret = wlan_hdd_validate_context(hdd_ctx);
    if (0 != ret)
        return ret;

#ifndef WLAN_FEATURE_MBSSID
    pVosContext = hdd_ctx->pvosContext;
    if (NULL == pVosContext) {
        VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
                "%s: VOS Context is NULL", __func__);
        return -EINVAL;
    }
#endif

    if(!wrqu->data.length)
    {
        EXIT();
        return 0;
    }

    if (wrqu->data.length > DOT11F_IE_RSN_MAX_LEN) {
       VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
               "%s: WPARSN Ie input length is more than max[%d]", __func__,
                wrqu->data.length);
       return -EINVAL;
    }

    switch (genie[0])
    {
        case DOT11F_EID_WPA:
        case DOT11F_EID_RSN:
            if((WLAN_HDD_GET_AP_CTX_PTR(pHostapdAdapter))->uPrivacy == 0)
            {
                hdd_softap_Deregister_BC_STA(pHostapdAdapter);
                hdd_softap_Register_BC_STA(pHostapdAdapter, 1);
            }
            (WLAN_HDD_GET_AP_CTX_PTR(pHostapdAdapter))->uPrivacy = 1;
#ifdef WLAN_FEATURE_MBSSID
            halStatus = WLANSAP_Set_WPARSNIes(WLAN_HDD_GET_SAP_CTX_PTR(pHostapdAdapter), genie, wrqu->data.length);
#else
            halStatus = WLANSAP_Set_WPARSNIes(pVosContext, genie, wrqu->data.length);
#endif
            break;

        default:
            hddLog (LOGE, "%s Set UNKNOWN IE %X",__func__, genie[0]);
            halStatus = 0;
    }

    EXIT();
    return halStatus;
}

/**
 * iw_set_ap_genie() - Wrapper function to protect __iw_set_ap_genie
 *                      from the SSR.
 *
 * @dev - Pointer to the net device.
 * @info - Pointer to the iw_request_info.
 * @wrqu - Pointer to the iwreq_data.
 * @extra - Pointer to the data.
 *
 * Return: 0 for success, non zero for failure.
 */
static int
iw_set_ap_genie(struct net_device *dev,
		struct iw_request_info *info,
		union iwreq_data *wrqu, char *extra)
{
	int ret;

	vos_ssr_protect(__func__);
	ret = __iw_set_ap_genie(dev, info, wrqu, extra);
	vos_ssr_unprotect(__func__);

	return ret;
}

struct linkspeed_priv {
	tSirLinkSpeedInfo linkspeed_info;
};

static void
hdd_get_link_speed_cb(tSirLinkSpeedInfo *linkspeed_info, void *cookie)
{
	struct hdd_request *request;
	struct linkspeed_priv *priv;

	if (NULL == linkspeed_info)
	{
		hddLog(VOS_TRACE_LEVEL_ERROR,
		       "%s: Bad param, linkspeed_info [%pK] cookie [%pK]",
		       __func__, linkspeed_info, cookie);
		return;
	}

	request = hdd_request_get(cookie);
	if (!request) {
		hddLog(VOS_TRACE_LEVEL_ERROR,"Obsolete request");
		return;
	}

	priv = hdd_request_priv(request);
	priv->linkspeed_info = *linkspeed_info;
	hdd_request_complete(request);
	hdd_request_put(request);
}

VOS_STATUS  wlan_hdd_get_linkspeed_for_peermac(hdd_adapter_t *pAdapter,
                                               tSirMacAddr macAddress)
{
   eHalStatus hstatus;
   VOS_STATUS status = VOS_STATUS_SUCCESS;
   void *cookie;
   tSirLinkSpeedInfo *linkspeed_info;
   int ret;
   struct hdd_request *request;
   struct linkspeed_priv *priv;
   static const struct hdd_request_params params = {
      .priv_size = sizeof(*priv),
      .timeout_ms = WLAN_WAIT_TIME_STATS,
   };

   if (NULL == pAdapter)
   {
      hddLog(VOS_TRACE_LEVEL_ERROR, "%s: pAdapter is NULL", __func__);
      return VOS_STATUS_E_FAULT;
   }

   request = hdd_request_alloc(&params);
   if (!request) {
      VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
                          "%s Request Buffer Alloc Fail", __func__);
      return VOS_STATUS_E_INVAL;
   }
   cookie = hdd_request_cookie(request);
   priv = hdd_request_priv(request);

   linkspeed_info = &priv->linkspeed_info;
   vos_mem_copy(linkspeed_info->peer_macaddr, macAddress, sizeof(tSirMacAddr) );
   hstatus = sme_GetLinkSpeed(WLAN_HDD_GET_HAL_CTX(pAdapter),
                              linkspeed_info,
                              cookie,
                              hdd_get_link_speed_cb);

   if (eHAL_STATUS_SUCCESS != hstatus)
   {
      hddLog(VOS_TRACE_LEVEL_ERROR,
            "%s: Unable to retrieve statistics for link speed, ret(%d)",
            __func__, hstatus);
      status = VOS_STATUS_E_INVAL;
      goto cleanup;
   }
   ret = hdd_request_wait_for_response(request);
   if (ret) {
      hddLog(VOS_TRACE_LEVEL_ERROR,
             "%s: SME timed out while retrieving link speed,ret(%d)",
             __func__, ret);
      status = VOS_STATUS_E_INVAL;
      goto cleanup;
   }
   pAdapter->ls_stats.estLinkSpeed = linkspeed_info->estLinkSpeed;

cleanup:
   /*
    * either we never sent a request, we sent a request and
    * received a response or we sent a request and timed out.
    * regardless we are done with the request.
    */
   hdd_request_put(request);
   return status;
}


static int
__iw_get_softap_linkspeed(struct net_device *dev, struct iw_request_info *info,
                        union iwreq_data *wrqu, char *extra)
{
   hdd_adapter_t *pHostapdAdapter = (netdev_priv(dev));
   hdd_context_t *pHddCtx;
   char *pLinkSpeed = (char*)extra;
   char *pmacAddress;
   v_U32_t link_speed = 0;
   int len = sizeof(v_U32_t)+1;
   tSirMacAddr macAddress;
   VOS_STATUS status = VOS_STATUS_E_FAILURE;
   int rc, valid, i;

   ENTER();

   pHddCtx = WLAN_HDD_GET_CTX(pHostapdAdapter);
   valid = wlan_hdd_validate_context(pHddCtx);
   if (0 != valid)
       return valid;

   hddLog(VOS_TRACE_LEVEL_INFO, "%s wrqu->data.length= %d\n", __func__, wrqu->data.length);

   if (wrqu->data.length >= MAC_ADDRESS_STR_LEN - 1)
   {
      pmacAddress = vos_mem_malloc(MAC_ADDRESS_STR_LEN);
      if (NULL == pmacAddress) {
          hddLog(LOG1, "unable to allocate memory");
          return -ENOMEM;
      }
      if (copy_from_user((void *)pmacAddress,
          wrqu->data.pointer, MAC_ADDRESS_STR_LEN))
      {
          hddLog(LOG1, "%s: failed to copy data to user buffer", __func__);
          vos_mem_free(pmacAddress);
          return -EFAULT;
      }
      pmacAddress[MAC_ADDRESS_STR_LEN -1] = '\0';

      status = hdd_string_to_hex (pmacAddress, MAC_ADDRESS_STR_LEN, macAddress );
      vos_mem_free(pmacAddress);

      if (!VOS_IS_STATUS_SUCCESS(status ))
      {
         hddLog(VOS_TRACE_LEVEL_ERROR, FL("String to Hex conversion Failed"));
      }
   }
   /* If no mac address is passed and/or its length is less than 17,
    * link speed for first connected client will be returned.
    */
   if (wrqu->data.length < 17 || !VOS_IS_STATUS_SUCCESS(status )) {
      for (i = 0; i < WLAN_MAX_STA_COUNT; i++) {
          if (pHostapdAdapter->aStaInfo[i].isUsed &&
             (!vos_is_macaddr_broadcast(&pHostapdAdapter->aStaInfo[i].macAddrSTA))) {
             vos_copy_macaddr((v_MACADDR_t *)macAddress,
                               &pHostapdAdapter->aStaInfo[i].macAddrSTA);
             status = VOS_STATUS_SUCCESS;
             break;
          }
      }
   }
   if (!VOS_IS_STATUS_SUCCESS(status )) {
      hddLog(VOS_TRACE_LEVEL_ERROR, FL("Invalid peer macaddress"));
      return -EINVAL;
   }
   status = wlan_hdd_get_linkspeed_for_peermac(pHostapdAdapter,
                                               macAddress);
   if (!VOS_IS_STATUS_SUCCESS(status ))
   {
        hddLog(VOS_TRACE_LEVEL_ERROR, FL("Unable to retrieve SME linkspeed"));
        return -EINVAL;
   }

   link_speed = pHostapdAdapter->ls_stats.estLinkSpeed;

   /* linkspeed in units of 500 kbps */
   link_speed = link_speed / 500;
   wrqu->data.length  = len;
   rc = snprintf(pLinkSpeed, len, "%u", link_speed);
   if ((rc < 0) || (rc >= len))
   {
       // encoding or length error?
       hddLog(VOS_TRACE_LEVEL_ERROR,FL("Unable to encode link speed"));
       return -EIO;
   }
   EXIT();
   return 0;
}

static int
iw_get_softap_linkspeed(struct net_device *dev, struct iw_request_info *info,
                        union iwreq_data *wrqu, char *extra)
{
	int ret;

	vos_ssr_protect(__func__);
	ret = __iw_get_softap_linkspeed(dev, info, wrqu, extra);
	vos_ssr_unprotect(__func__);

	return ret;
}

struct peer_rssi_priv {
	eHalStatus status;
	struct sir_peer_sta_info peer_sta_info;
};

/**
 * hdd_get_rssi_cb() - get station's rssi callback
 * @sta_rssi: pointer of peer information
 * @context: get rssi callback context
 *
 * This function will fill rssi information to hostapd
 * adapter
 *
 */
void hdd_get_rssi_cb(struct sir_peer_info_resp *sta_rssi, void *context)
{
	struct hdd_request *request;
	struct peer_rssi_priv *priv;
	struct sir_peer_info *rssi_info;
	uint8_t peer_num;

	request = hdd_request_get(context);
	if (!request) {
		hddLog(VOS_TRACE_LEVEL_ERROR,
		       "Obsolete request %pK", context);
		return;
	}
	priv = hdd_request_priv(request);

	if (!sta_rssi) {
		hddLog(VOS_TRACE_LEVEL_ERROR,
		       "%s: Bad param, sta_rssi [%pK] context [%pK]",
		       __func__, sta_rssi, context);
		priv->status = eHAL_STATUS_INVALID_PARAMETER;
		hdd_request_complete(request);
		hdd_request_put(request);

		return;
	}

	peer_num = sta_rssi->count;
	rssi_info = sta_rssi->info;

	hddLog(VOS_TRACE_LEVEL_INFO,
	       "%d peers", peer_num);

	if (peer_num > MAX_PEER_STA) {
		hddLog(VOS_TRACE_LEVEL_WARN,
		       "Exceed max peer sta to handle one time %d", peer_num);
		peer_num = MAX_PEER_STA;
	}
	vos_mem_copy(priv->peer_sta_info.info, rssi_info,
		     peer_num * sizeof(*rssi_info));
	priv->peer_sta_info.sta_num = peer_num;
	priv->status = eHAL_STATUS_SUCCESS;
	hdd_request_complete(request);
	hdd_request_put(request);

}

/**
 * wlan_hdd_get_peer_rssi() - get station's rssi
 * @adapter: hostapd interface
 * @macaddress: iwpriv request information
 * @wrqu: iwpriv command parameter
 * @extra
 *
 * This function will call sme_get_peer_info to get rssi
 *
 * Return: 0 on success, otherwise error value
 */
static int  wlan_hdd_get_peer_rssi(hdd_adapter_t *adapter,
				   v_MACADDR_t macaddress,
				   struct sir_peer_sta_info *peer_sta_info)
{
	eHalStatus hstatus;
	void *cookie;
	int ret;
	struct sir_peer_info_req rssi_req;
	struct hdd_request *request;
	struct peer_rssi_priv *priv;
	static const struct hdd_request_params params = {
		.priv_size = sizeof(*priv),
		.timeout_ms = WLAN_WAIT_TIME_STATS,
	};

	if (!adapter || !peer_sta_info) {
		hddLog(VOS_TRACE_LEVEL_ERROR,
		       "adapter [%pK], peer_sta_info[%pK]",
		       adapter, peer_sta_info);
		return -EFAULT;
	}

	request = hdd_request_alloc(&params);
	if (!request) {
		hddLog(VOS_TRACE_LEVEL_ERROR,
		       "Request allocation failure");
		return -ENOMEM;
	}

	cookie = hdd_request_cookie(request);
	priv = hdd_request_priv(request);
	priv->status = eHAL_STATUS_FAILURE;

	vos_mem_copy(&(rssi_req.peer_macaddr), &macaddress,
				VOS_MAC_ADDR_SIZE);
	rssi_req.sessionid = adapter->sessionId;

	hstatus = sme_get_peer_info(WLAN_HDD_GET_HAL_CTX(adapter),
				    rssi_req,
				    cookie,
				    hdd_get_rssi_cb);
	if (eHAL_STATUS_SUCCESS != hstatus) {
		hddLog(VOS_TRACE_LEVEL_ERROR,
		       "%s: Unable to retrieve statistics for rssi",
		       __func__);
		ret = -EFAULT;
	} else {
		ret = hdd_request_wait_for_response(request);
		if (ret) {
			hddLog(VOS_TRACE_LEVEL_ERROR,
			       "SME timed out while retrieving rssi");
			ret = -EFAULT;
		} else if (priv->status !=  eHAL_STATUS_SUCCESS) {
			hddLog(VOS_TRACE_LEVEL_ERROR,
			       "request failed %d", priv->status);
			ret = -EFAULT;
		} else {
			*peer_sta_info = priv->peer_sta_info;
			ret = 0;
		}
	}

	hdd_request_put(request);

	return ret;
}

/**
 * __iw_get_peer_rssi() - get station's rssi
 * @dev: net device
 * @info: iwpriv request information
 * @wrqu: iwpriv command parameter
 * @extra
 *
 * This function will call wlan_hdd_get_peer_rssi
 * to get rssi
 *
 * Return: 0 on success, otherwise error value
 */
static int
__iw_get_peer_rssi(struct net_device *dev, struct iw_request_info *info,
			union iwreq_data *wrqu, char *extra)
{
	hdd_adapter_t *adapter = (netdev_priv(dev));
	hdd_context_t *hddctx;
	char macaddrarray[18];
	v_MACADDR_t macaddress = VOS_MAC_ADDR_BROADCAST_INITIALIZER;
	VOS_STATUS status = VOS_STATUS_E_FAILURE;
	int ret;
	char *rssi_info_output = extra;
	struct sir_peer_sta_info peer_sta_info;
	struct sir_peer_info *rssi_info;
	int i;
	int buf;
	int length;

	ENTER();

	hddctx = WLAN_HDD_GET_CTX(adapter);
	ret = wlan_hdd_validate_context(hddctx);
	if (0 != ret)
		return ret;

	hddLog(VOS_TRACE_LEVEL_INFO, "%s wrqu->data.length= %d",
	       __func__, wrqu->data.length);

	if (wrqu->data.length >= MAC_ADDRESS_STR_LEN - 1) {

		if (copy_from_user(macaddrarray,
			wrqu->data.pointer, MAC_ADDRESS_STR_LEN - 1)) {

			hddLog(LOG1, "%s: failed to copy data to user buffer",
			       __func__);
			return -EFAULT;
		}

		macaddrarray[MAC_ADDRESS_STR_LEN - 1] = '\0';
		hddLog(LOG1, "%s, %s",
		       __func__, macaddrarray);

		status = hdd_string_to_hex(macaddrarray,
				MAC_ADDRESS_STR_LEN, macaddress.bytes );

		if (!VOS_IS_STATUS_SUCCESS(status)) {
			hddLog(VOS_TRACE_LEVEL_ERROR,
			       FL("String to Hex conversion Failed"));
		}
	}

	vos_mem_zero(&peer_sta_info, sizeof(peer_sta_info));
	ret = wlan_hdd_get_peer_rssi(adapter, macaddress, &peer_sta_info);
	if (ret) {
		hddLog(VOS_TRACE_LEVEL_ERROR,
		       "Unable to retrieve peer rssi: %d", ret);
		return ret;
	}

	/*
	 * The iwpriv tool default print is before mac addr and rssi.
	 * Add '\n' before first rssi item to align the frist rssi item
	 * with others
	 *
	 * wlan     getRSSI:
	 * [macaddr1] [rssi1]
	 * [macaddr2] [rssi2]
	 * [macaddr3] [rssi3]
	 */
	length = scnprintf((rssi_info_output), WE_MAX_STR_LEN, "\n");
	rssi_info = &peer_sta_info.info[0];
	for (i = 0; i < peer_sta_info.sta_num; i++) {
		buf = scnprintf((rssi_info_output + length),
				WE_MAX_STR_LEN - length,
				"[%pM] [%d]\n",
				rssi_info[i].peer_macaddr,
				rssi_info[i].rssi);
		length += buf;
	}
	wrqu->data.length = length + 1;

	EXIT();

	return 0;
}

/**
 * iw_get_peer_rssi() - get station's rssi
 * @dev: net device
 * @info: iwpriv request information
 * @wrqu: iwpriv command parameter
 * @extra
 *
 * This function will call __iw_get_peer_rssi
 *
 * Return: 0 on success, otherwise error value
 */
static int
iw_get_peer_rssi(struct net_device *dev, struct iw_request_info *info,
			union iwreq_data *wrqu, char *extra)
{
	int ret;

	vos_ssr_protect(__func__);
	ret = __iw_get_peer_rssi(dev, info, wrqu, extra);
	vos_ssr_unprotect(__func__);

	return ret;
}

static const iw_handler      hostapd_handler[] =
{
   (iw_handler) NULL,           /* SIOCSIWCOMMIT */
   (iw_handler) NULL,           /* SIOCGIWNAME */
   (iw_handler) NULL,           /* SIOCSIWNWID */
   (iw_handler) NULL,           /* SIOCGIWNWID */
   (iw_handler) NULL,           /* SIOCSIWFREQ */
   (iw_handler) iw_get_ap_freq,    /* SIOCGIWFREQ */
   (iw_handler) NULL,           /* SIOCSIWMODE */
   (iw_handler) NULL,           /* SIOCGIWMODE */
   (iw_handler) NULL,           /* SIOCSIWSENS */
   (iw_handler) NULL,           /* SIOCGIWSENS */
   (iw_handler) NULL,           /* SIOCSIWRANGE */
   (iw_handler) NULL,           /* SIOCGIWRANGE */
   (iw_handler) NULL,           /* SIOCSIWPRIV */
   (iw_handler) NULL,           /* SIOCGIWPRIV */
   (iw_handler) NULL,           /* SIOCSIWSTATS */
   (iw_handler) NULL,           /* SIOCGIWSTATS */
   (iw_handler) NULL,           /* SIOCSIWSPY */
   (iw_handler) NULL,           /* SIOCGIWSPY */
   (iw_handler) NULL,           /* SIOCSIWTHRSPY */
   (iw_handler) NULL,           /* SIOCGIWTHRSPY */
   (iw_handler) NULL,           /* SIOCSIWAP */
   (iw_handler) NULL,           /* SIOCGIWAP */
   (iw_handler) iw_set_ap_mlme,    /* SIOCSIWMLME */
   (iw_handler) NULL,           /* SIOCGIWAPLIST */
   (iw_handler) NULL,           /* SIOCSIWSCAN */
   (iw_handler) NULL,           /* SIOCGIWSCAN */
   (iw_handler) NULL,           /* SIOCSIWESSID */
   (iw_handler) NULL,           /* SIOCGIWESSID */
   (iw_handler) NULL,           /* SIOCSIWNICKN */
   (iw_handler) NULL,           /* SIOCGIWNICKN */
   (iw_handler) NULL,           /* -- hole -- */
   (iw_handler) NULL,           /* -- hole -- */
   (iw_handler) NULL,           /* SIOCSIWRATE */
   (iw_handler) NULL,           /* SIOCGIWRATE */
   (iw_handler) NULL,           /* SIOCSIWRTS */
   (iw_handler) iw_get_ap_rts_threshold,     /* SIOCGIWRTS */
   (iw_handler) NULL,           /* SIOCSIWFRAG */
   (iw_handler) iw_get_ap_frag_threshold,    /* SIOCGIWFRAG */
   (iw_handler) NULL,           /* SIOCSIWTXPOW */
   (iw_handler) NULL,           /* SIOCGIWTXPOW */
   (iw_handler) NULL,           /* SIOCSIWRETRY */
   (iw_handler) NULL,           /* SIOCGIWRETRY */
   (iw_handler) NULL,           /* SIOCSIWENCODE */
   (iw_handler) NULL,           /* SIOCGIWENCODE */
   (iw_handler) NULL,           /* SIOCSIWPOWER */
   (iw_handler) NULL,           /* SIOCGIWPOWER */
   (iw_handler) NULL,           /* -- hole -- */
   (iw_handler) NULL,           /* -- hole -- */
   (iw_handler) iw_set_ap_genie,     /* SIOCSIWGENIE */
   (iw_handler) NULL,           /* SIOCGIWGENIE */
   (iw_handler) iw_set_auth_hostap,    /* SIOCSIWAUTH */
   (iw_handler) NULL,           /* SIOCGIWAUTH */
   (iw_handler) iw_set_ap_encodeext,     /* SIOCSIWENCODEEXT */
   (iw_handler) NULL,           /* SIOCGIWENCODEEXT */
   (iw_handler) NULL,           /* SIOCSIWPMKSA */
};

/*
 * Note that the following ioctls were defined with semantics which
 * cannot be handled by the "iwpriv" userspace application and hence
 * they are not included in the hostapd_private_args array
 *     QCSAP_IOCTL_ASSOC_STA_MACADDR
 */

static const struct iw_priv_args hostapd_private_args[] = {
  { QCSAP_IOCTL_SETPARAM,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 2, 0, "setparam" },
  { QCSAP_IOCTL_SETPARAM,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1, 0, "" },
  { QCSAP_PARAM_MAX_ASSOC,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1, 0, "setMaxAssoc" },
   { QCSAP_PARAM_HIDE_SSID,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1, 0,  "hideSSID" },
   { QCSAP_PARAM_SET_MC_RATE,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1, 0,  "setMcRate" },
   { QCSAP_PARAM_SET_TXRX_FW_STATS,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1, 0,  "txrx_fw_stats" },
   { QCSAP_PARAM_SET_MCC_CHANNEL_LATENCY,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1, 0,  "setMccLatency" },
   { QCSAP_PARAM_SET_MCC_CHANNEL_QUOTA,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1, 0,  "setMccQuota" },
   { QCSAP_PARAM_AUTO_CHANNEL,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1, 0,  "setAutoChannel" },
   { QCSAP_PARAM_SET_CHANNEL_CHANGE,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1, 0,  "setChanChange" },

 /* Sub-cmds DBGLOG specific commands */
    {   QCSAP_DBGLOG_LOG_LEVEL ,
        IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
        0,
        "dl_loglevel" },

    {   QCSAP_DBGLOG_VAP_ENABLE ,
        IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
        0,
        "dl_vapon" },

    {   QCSAP_DBGLOG_VAP_DISABLE ,
        IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
        0,
        "dl_vapoff" },

    {   QCSAP_DBGLOG_MODULE_ENABLE ,
        IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
        0,
        "dl_modon" },

    {   QCSAP_DBGLOG_MODULE_DISABLE,
        IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
        0,
        "dl_modoff" },

    {   QCSAP_DBGLOG_MOD_LOG_LEVEL,
        IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
        0,
        "dl_mod_loglevel" },

    {   QCSAP_DBGLOG_TYPE,
        IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
        0,
        "dl_type" },
    {   QCSAP_DBGLOG_REPORT_ENABLE,
        IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
        0,
        "dl_report" },
    {   QCASAP_TXRX_FWSTATS_RESET,
        IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
        0,
        "txrx_fw_st_rst" },
    {   QCSAP_PARAM_RTSCTS,
        IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
        0,
        "enablertscts" },

    {   QCASAP_SET_11N_RATE,
        IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
        0,
        "set11NRates" },

    {   QCASAP_SET_VHT_RATE,
        IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
        0,
        "set11ACRates" },

    {   QCASAP_SHORT_GI,
        IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
        0,
        "enable_short_gi" },

    {   QCSAP_SET_AMPDU,
        IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
        0,
        "ampdu" },

    {   QCSAP_SET_AMSDU,
        IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
        0,
        "amsdu" },

    {  QCSAP_GTX_HT_MCS,
       IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
        0,
        "gtxHTMcs" },

    {  QCSAP_GTX_VHT_MCS,
       IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
        0,
        "gtxVHTMcs" },

    {  QCSAP_GTX_USRCFG,
       IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
        0,
        "gtxUsrCfg" },

    {  QCSAP_GTX_THRE,
       IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
        0,
        "gtxThre" },

    {  QCSAP_GTX_MARGIN,
       IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
        0,
        "gtxMargin" },

    {  QCSAP_GTX_STEP,
       IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
        0,
        "gtxStep" },

    {  QCSAP_GTX_MINTPC,
       IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
        0,
        "gtxMinTpc" },

    {  QCSAP_GTX_BWMASK,
       IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
        0,
        "gtxBWMask" },

    { QCSAP_PARAM_CLR_ACL,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
      0,
      "setClearAcl" },

   {  QCSAP_PARAM_ACL_MODE,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
      0,
      "setAclMode" },

#ifdef QCA_PKT_PROTO_TRACE
    {   QCASAP_SET_DEBUG_LOG,
        IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
        0,
        "setDbgLvl" },
#endif /* QCA_PKT_PROTO_TRACE */

    {   QCASAP_SET_TM_LEVEL,
        IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
        0,
        "setTmLevel" },

    {   QCASAP_SET_DFS_IGNORE_CAC,
        IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
        0,
        "setDfsIgnoreCAC" },

    {   QCASAP_SET_DFS_NOL,
        IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
        0,
        "setdfsnol" },

    {   QCASAP_SET_DFS_TARGET_CHNL,
        IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
        0,
        "setNextChnl" },

    {   QCASAP_SET_RADAR_CMD,
        IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
        0,
        "setRadar" },
#ifdef IPA_UC_OFFLOAD
    {   QCSAP_IPA_UC_STAT,
        IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
        0,
        "ipaucstat" },
#endif /* IPA_UC_OFFLOAD */

    {   QCASAP_TX_CHAINMASK_CMD,
        IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
        0,
        "set_txchainmask" },

    {   QCASAP_RX_CHAINMASK_CMD,
        IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
        0,
        "set_rxchainmask" },

    {   QCASAP_NSS_CMD,
        IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
        0,
        "set_nss" },

    {   QCASAP_SET_PHYMODE,
        IW_PRIV_TYPE_INT| IW_PRIV_SIZE_FIXED | 1,
        0,
        "setphymode" },

    {   QCASAP_DUMP_STATS,
        IW_PRIV_TYPE_INT| IW_PRIV_SIZE_FIXED | 1,
        0,
        "dumpStats" },

    {   QCASAP_CLEAR_STATS,
        IW_PRIV_TYPE_INT| IW_PRIV_SIZE_FIXED | 1,
        0,
        "clearStats" },
    {   QCASAP_PARAM_LDPC,
        IW_PRIV_TYPE_INT| IW_PRIV_SIZE_FIXED | 1,
        0,
        "set_ldpc" },
    {   QCASAP_PARAM_TX_STBC,
        IW_PRIV_TYPE_INT| IW_PRIV_SIZE_FIXED | 1,
        0,
        "set_tx_stbc" },
    {   QCASAP_PARAM_RX_STBC,
        IW_PRIV_TYPE_INT| IW_PRIV_SIZE_FIXED | 1,
        0,
        "set_rx_stbc" },

    {   QCSAP_SET_DEFAULT_AMPDU,
        IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
        0,
        "def_ampdu" },

    {   QCSAP_ENABLE_RTS_BURSTING,
        IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
        0,
        "rts_bursting" },

    {   QCSAP_ENABLE_DYNAMIC_BW,
        IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
        0,
        "cwmenable" },


  { QCSAP_IOCTL_GETPARAM, 0,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,    "getparam" },
  { QCSAP_IOCTL_GETPARAM, 0,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,    "" },
  { QCSAP_PARAM_MAX_ASSOC, 0,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,    "getMaxAssoc" },
  { QCSAP_PARAM_GET_WLAN_DBG, 0,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,    "getwlandbg" },
  { QCSAP_PARAM_AUTO_CHANNEL, 0,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,    "getAutoChannel" },
  { QCSAP_GTX_BWMASK, 0,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,    "get_gtxBWMask" },
  { QCSAP_GTX_MINTPC, 0,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,    "get_gtxMinTpc" },
  { QCSAP_GTX_STEP, 0,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,    "get_gtxStep" },
  { QCSAP_GTX_MARGIN, 0,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,    "get_gtxMargin" },
  { QCSAP_GTX_THRE, 0,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,    "get_gtxThre" },
  { QCSAP_GTX_USRCFG, 0,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,    "get_gtxUsrCfg" },
  { QCSAP_GTX_VHT_MCS, 0,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,    "get_gtxVHTMcs" },
  { QCSAP_GTX_HT_MCS, 0,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,    "get_gtxHTMcs" },
  { QCASAP_SHORT_GI, 0,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,    "get_short_gi" },
  { QCSAP_PARAM_RTSCTS, 0,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,    "get_rtscts" },
  { QCASAP_GET_DFS_NOL, 0,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,    "getdfsnol" },
  { QCSAP_GET_ACL, 0,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,    "get_acl_list" },
  { QCASAP_PARAM_LDPC, 0,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,    "get_ldpc" },
  { QCASAP_PARAM_TX_STBC, 0,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,    "get_tx_stbc" },
  { QCASAP_PARAM_RX_STBC, 0,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,    "get_rx_stbc" },
  { QCSAP_PARAM_CHAN_WIDTH, 0,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,    "get_chwidth" },
#ifdef WLAN_FEATURE_TSF
  { QCSAP_CAP_TSF, 0,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,    "cap_tsf" },
#endif
  { QCSAP_IOCTL_SET_NONE_GET_THREE, 0,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 3,    "" },
#ifdef WLAN_FEATURE_TSF
  { QCSAP_GET_TSF, 0,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 3,    "get_tsf" },
#endif
  { QCASAP_TX_CHAINMASK_CMD, 0,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,    "get_txchainmask" },
  { QCASAP_RX_CHAINMASK_CMD, 0,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,    "get_rxchainmask" },
  { QCSAP_GET_DYNAMIC_BW, 0,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,    "get_cwmenable" },
  { QCASAP_NSS_CMD, 0,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,    "get_nss" },
  { QCASAP_GET_TEMP_CMD, 0,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,    "get_temp" },
  { QCSAP_GET_FW_STATUS, 0,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,    "get_fwstate" },

  { QCSAP_IOCTL_GET_STAWPAIE,
      0,IW_PRIV_TYPE_BYTE |  DOT11F_IE_RSN_MAX_LEN, "get_staWPAIE" },
  { QCSAP_IOCTL_STOPBSS,
      IW_PRIV_TYPE_BYTE | IW_PRIV_SIZE_FIXED, 0, "stopbss" },
  { QCSAP_IOCTL_VERSION, 0,
      IW_PRIV_TYPE_CHAR | WE_MAX_STR_LEN, "version" },
  { QCSAP_IOCTL_GET_STA_INFO, 0,
      IW_PRIV_TYPE_CHAR | WE_SAP_MAX_STA_INFO, "get_sta_info" },
  { QCSAP_IOCTL_GET_WPS_PBC_PROBE_REQ_IES,
      IW_PRIV_TYPE_BYTE | sizeof(sQcSapreq_WPSPBCProbeReqIES_t) | IW_PRIV_SIZE_FIXED, 0, "getProbeReqIEs" },
  { QCSAP_IOCTL_GET_CHANNEL, 0,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1, "getchannel" },
  { QCSAP_IOCTL_DISASSOC_STA,
        IW_PRIV_TYPE_BYTE | IW_PRIV_SIZE_FIXED | 6 , 0, "disassoc_sta" },
  { QCSAP_IOCTL_AP_STATS, 0,
        IW_PRIV_TYPE_CHAR | QCSAP_MAX_WSC_IE, "ap_stats" },
   /* handler for main ioctl */
  { QCSAP_PRIV_GET_CHAR_SET_NONE, 0,
        IW_PRIV_TYPE_CHAR | WE_MAX_STR_LEN,"" },
   /* handler for sub-ioctl */
  { QCSAP_GET_STATS, 0,
        IW_PRIV_TYPE_CHAR | WE_MAX_STR_LEN, "getStats" },
  { QCSAP_IOCTL_PRIV_GET_SOFTAP_LINK_SPEED,
        IW_PRIV_TYPE_CHAR | 18,
        IW_PRIV_TYPE_CHAR | 5, "getLinkSpeed" },
  { QCSAP_IOCTL_PRIV_GET_RSSI,
        IW_PRIV_TYPE_CHAR | 18,
        IW_PRIV_TYPE_CHAR | WE_MAX_STR_LEN, "getRSSI" },
  { QCSAP_IOCTL_PRIV_SET_THREE_INT_GET_NONE,
        IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 3, 0, "" },
   /* handlers for sub-ioctl */
   {   WE_SET_WLAN_DBG,
       IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 3,
       0,
       "setwlandbg" },

   {   WE_SET_SAP_CHANNELS,
       IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 3,
       0,
       "setsapchannels" },

   /* handlers for sub-ioctl */
   {  WE_SET_DP_TRACE,
      IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 3,
      0,
      "set_dp_trace" },

   /* handlers for main ioctl */
   {   QCSAP_IOCTL_PRIV_SET_VAR_INT_GET_NONE,
       IW_PRIV_TYPE_INT | MAX_VAR_ARGS,
       0,
       "" },

   /* handlers for sub-ioctl */
   {   WE_LOG_DUMP_CMD,
       IW_PRIV_TYPE_INT | MAX_VAR_ARGS,
       0,
       "dump" },
   {   WE_P2P_NOA_CMD,
       IW_PRIV_TYPE_INT | MAX_VAR_ARGS,
       0,
       "SetP2pPs" },
     /* handlers for sub ioctl */
    {
        WE_MCC_CONFIG_CREDENTIAL,
        IW_PRIV_TYPE_INT | MAX_VAR_ARGS,
        0,
        "setMccCrdnl" },

     /* handlers for sub ioctl */
    {
        WE_MCC_CONFIG_PARAMS,
        IW_PRIV_TYPE_INT | MAX_VAR_ARGS,
        0,
        "setMccConfig" },

    {
        WE_UNIT_TEST_CMD,
        IW_PRIV_TYPE_INT | MAX_VAR_ARGS,
        0,
        "setUnitTestCmd" },

#ifdef MEMORY_DEBUG
    /* handlers for sub ioctl */
    {   WE_MEM_TRACE_DUMP,
        IW_PRIV_TYPE_INT | MAX_VAR_ARGS,
        0,
        "memTraceLog" },
#endif

    /* handlers for main ioctl */
    {   QCSAP_IOCTL_MODIFY_ACL,
        IW_PRIV_TYPE_BYTE | IW_PRIV_SIZE_FIXED | 8,
        0,
        "modify_acl" },

    /* handlers for main ioctl */
    {   QCSAP_IOCTL_GET_CHANNEL_LIST,
        0,
        IW_PRIV_TYPE_BYTE | sizeof(tChannelListInfo),
        "getChannelList" },

    /* handlers for main ioctl */
    {   QCSAP_IOCTL_SET_TX_POWER,
        IW_PRIV_TYPE_INT| IW_PRIV_SIZE_FIXED | 1,
        0,
        "setTxPower" },

    /* handlers for main ioctl */
    {   QCSAP_IOCTL_SET_MAX_TX_POWER,
        IW_PRIV_TYPE_INT| IW_PRIV_SIZE_FIXED | 1,
        0,
        "setTxMaxPower" },

    {   QCSAP_IOCTL_DATAPATH_SNAP_SHOT,
        IW_PRIV_TYPE_NONE | IW_PRIV_TYPE_NONE,
        0,
        "dataSnapshot" },

    /* Set HDD CFG Ini param */
    {   QCSAP_IOCTL_SET_INI_CFG,
        IW_PRIV_TYPE_CHAR | QCSAP_IOCTL_MAX_STR_LEN,
        0,
        "setConfig" },

    /* Get HDD CFG Ini param */
    {   QCSAP_IOCTL_GET_INI_CFG,
        0,
        IW_PRIV_TYPE_CHAR | QCSAP_IOCTL_MAX_STR_LEN,
        "getConfig" },

    /* handlers for main ioctl */
    {   QCSAP_IOCTL_SET_TWO_INT_GET_NONE,
        IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 2,
        0,
        "" },
    /* handlers for sub-ioctl */
#ifdef WLAN_DEBUG
    {   QCSAP_IOCTL_SET_FW_CRASH_INJECT,
        IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 2,
        0,
        "crash_inject" },
#endif

    /* handlers for main ioctl */
    {   QCSAP_IOCTL_WOWL_CONFIG_PTRN,
        IW_PRIV_TYPE_CHAR | 512,
        0,
        "" },

    /* handlers for sub-ioctl */
    {   WE_WOWL_ADD_PTRN,
        IW_PRIV_TYPE_CHAR | 512,
        0,
        "wowlAddPtrn" },

    {   WE_WOWL_DEL_PTRN,
        IW_PRIV_TYPE_CHAR | 512,
        0,
        "wowlDelPtrn" },

    {   QCASAP_SET_RADAR_DBG,
        IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
        0,  "setRadarDbg" },

    /* dump dp trace - descriptor or dp trace records */
    {   QCSAP_IOCTL_DUMP_DP_TRACE_LEVEL,
        IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 2,
        0, "dump_dp_trace" },

    {   WE_SET_THERMAL_THROTTLE_CONFIG,
        IW_PRIV_TYPE_INT | MAX_VAR_ARGS,
        0, "setThermalConfig" },
};

static const iw_handler hostapd_private[] = {
   [QCSAP_IOCTL_SETPARAM - SIOCIWFIRSTPRIV] = iw_softap_setparam,  //set priv ioctl
   [QCSAP_IOCTL_GETPARAM - SIOCIWFIRSTPRIV] = iw_softap_getparam,  //get priv ioctl
   [QCSAP_IOCTL_SET_NONE_GET_THREE - SIOCIWFIRSTPRIV] = iw_softap_get_three,
   [QCSAP_IOCTL_GET_STAWPAIE - SIOCIWFIRSTPRIV] = iw_get_genie, //get station genIE
   [QCSAP_IOCTL_STOPBSS - SIOCIWFIRSTPRIV] = iw_softap_stopbss,       // stop bss
   [QCSAP_IOCTL_VERSION - SIOCIWFIRSTPRIV] = iw_softap_version,       // get driver version
   [QCSAP_IOCTL_GET_WPS_PBC_PROBE_REQ_IES - SIOCIWFIRSTPRIV] = iw_get_WPSPBCProbeReqIEs,
   [QCSAP_IOCTL_GET_CHANNEL - SIOCIWFIRSTPRIV] = iw_softap_getchannel,
   [QCSAP_IOCTL_ASSOC_STA_MACADDR - SIOCIWFIRSTPRIV] = iw_softap_getassoc_stamacaddr,
   [QCSAP_IOCTL_DISASSOC_STA - SIOCIWFIRSTPRIV] = iw_softap_disassoc_sta,
   [QCSAP_IOCTL_AP_STATS - SIOCIWFIRSTPRIV] = iw_softap_ap_stats,
   [QCSAP_PRIV_GET_CHAR_SET_NONE - SIOCIWFIRSTPRIV] = iw_get_char_setnone,
   [QCSAP_IOCTL_PRIV_SET_THREE_INT_GET_NONE - SIOCIWFIRSTPRIV]  = iw_set_three_ints_getnone,
   [QCSAP_IOCTL_PRIV_SET_VAR_INT_GET_NONE - SIOCIWFIRSTPRIV]     = iw_set_var_ints_getnone,
   [QCSAP_IOCTL_SET_CHANNEL_RANGE - SIOCIWFIRSTPRIV] =
                                             iw_softap_set_force_acs_ch_range,
   [QCSAP_IOCTL_MODIFY_ACL - SIOCIWFIRSTPRIV]   = iw_softap_modify_acl,
   [QCSAP_IOCTL_GET_CHANNEL_LIST - SIOCIWFIRSTPRIV]   = iw_get_channel_list,
   [QCSAP_IOCTL_GET_STA_INFO - SIOCIWFIRSTPRIV] = iw_softap_get_sta_info,
   [QCSAP_IOCTL_PRIV_GET_SOFTAP_LINK_SPEED - SIOCIWFIRSTPRIV]     = iw_get_softap_linkspeed,
   [QCSAP_IOCTL_PRIV_GET_RSSI - SIOCIWFIRSTPRIV] = iw_get_peer_rssi,
   [QCSAP_IOCTL_SET_TX_POWER - SIOCIWFIRSTPRIV]   = iw_softap_set_tx_power,
   [QCSAP_IOCTL_SET_MAX_TX_POWER - SIOCIWFIRSTPRIV]   = iw_softap_set_max_tx_power,
   [QCSAP_IOCTL_DATAPATH_SNAP_SHOT - SIOCIWFIRSTPRIV]  =   iw_display_data_path_snapshot,
   [QCSAP_IOCTL_SET_INI_CFG - SIOCIWFIRSTPRIV]  =  iw_softap_set_ini_cfg,
   [QCSAP_IOCTL_GET_INI_CFG - SIOCIWFIRSTPRIV]  =  iw_softap_get_ini_cfg,
   [QCSAP_IOCTL_SET_TWO_INT_GET_NONE - SIOCIWFIRSTPRIV] =
                                                iw_softap_set_two_ints_getnone,
   [QCSAP_IOCTL_WOWL_CONFIG_PTRN - SIOCIWFIRSTPRIV] = iw_softap_wowl_config_pattern,
};
const struct iw_handler_def hostapd_handler_def = {
   .num_standard     = sizeof(hostapd_handler) / sizeof(hostapd_handler[0]),
#ifdef CONFIG_WEXT_PRIV
   .num_private      = sizeof(hostapd_private) / sizeof(hostapd_private[0]),
   .num_private_args = sizeof(hostapd_private_args) / sizeof(hostapd_private_args[0]),
#endif
   .standard         = (iw_handler *)hostapd_handler,
#ifdef CONFIG_WEXT_PRIV
   .private          = (iw_handler *)hostapd_private,
   .private_args     = hostapd_private_args,
#endif
   .get_wireless_stats = NULL,
};

struct net_device_ops net_ops_struct  = {
    .ndo_open = hdd_hostapd_open,
    .ndo_stop = hdd_hostapd_stop,
    .ndo_uninit = hdd_hostapd_uninit,
    .ndo_start_xmit = hdd_softap_hard_start_xmit,
    .ndo_tx_timeout = hdd_softap_tx_timeout,
    .ndo_get_stats = hdd_softap_stats,
    .ndo_set_mac_address = hdd_hostapd_set_mac_address,
    .ndo_do_ioctl = hdd_hostapd_ioctl,
    .ndo_change_mtu = hdd_hostapd_change_mtu,
    .ndo_select_queue = hdd_hostapd_select_queue,
 };

static int hdd_set_hostapd(hdd_adapter_t *pAdapter)
{
    return VOS_STATUS_SUCCESS;
}

void hdd_set_ap_ops( struct net_device *pWlanHostapdDev )
{
  pWlanHostapdDev->netdev_ops = &net_ops_struct;
}

VOS_STATUS hdd_init_ap_mode(hdd_adapter_t *pAdapter, bool reinit)
{
    hdd_hostapd_state_t * phostapdBuf;
#ifdef CONFIG_WIRELESS_EXT
    struct net_device *dev = pAdapter->dev;
#endif
    hdd_context_t *pHddCtx = WLAN_HDD_GET_CTX(pAdapter);
    VOS_STATUS status;
#ifdef WLAN_FEATURE_MBSSID
    v_CONTEXT_t pVosContext = (WLAN_HDD_GET_CTX(pAdapter))->pvosContext;
    v_CONTEXT_t sapContext=NULL;
    enum dfs_mode mode;
    tVOS_CON_MODE device_mode;
    uint32_t session_id = CSR_SESSION_ID_INVALID;
#endif
    int ret;

    ENTER();

    hdd_set_sap_auth_offload(pAdapter, TRUE);

    ret = hdd_set_client_block_info(pAdapter);
    if (0 != ret) {
        hddLog(VOS_TRACE_LEVEL_ERROR,
            "%s: set client block info failed %d",
            __func__, ret);
    }

    VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_INFO,
               FL("SSR in progress: %d"), reinit);
#ifdef WLAN_FEATURE_MBSSID
    if (reinit) {
        sapContext = pAdapter->sessionCtx.ap.sapContext;
    } else {
        sapContext = WLANSAP_Open(pVosContext);
        if (sapContext == NULL)
        {
            VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
                       FL("ERROR: WLANSAP_Open failed!!"));
            return VOS_STATUS_E_FAULT;
        }

        pAdapter->sessionCtx.ap.sapContext = sapContext;
        pAdapter->sessionCtx.ap.sapConfig.channel =
                                   pHddCtx->acs_policy.acs_channel;
        mode = pHddCtx->acs_policy.acs_dfs_mode;
        pAdapter->sessionCtx.ap.sapConfig.acs_dfs_mode =
                                        wlan_hdd_get_dfs_mode(mode);
    }

    if (pAdapter->device_mode == WLAN_HDD_P2P_GO) {
        device_mode = VOS_P2P_GO_MODE;
    } else if (pAdapter->device_mode == WLAN_HDD_SOFTAP) {
        device_mode = VOS_STA_SAP_MODE;
    } else {
        VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
                FL("Invalid device_mode for AP: %d"), pAdapter->device_mode);
        return VOS_STATUS_E_FAILURE;
    }

    ret = process_wma_set_command((int)pAdapter->sessionId,
                         (int)WMI_PDEV_PARAM_BURST_ENABLE,
                         (int)pHddCtx->cfg_ini->enableSifsBurst,
                         PDEV_CMD);

    if (0 != ret) {
        hddLog(VOS_TRACE_LEVEL_ERROR,
                    "%s: WMI_PDEV_PARAM_BURST_ENABLE set failed %d",
                    __func__, ret);
    }

    status = WLANSAP_Start(sapContext, device_mode,
            pAdapter->macAddressCurrent.bytes,
            &session_id);
    if ( ! VOS_IS_STATUS_SUCCESS( status ) )
    {
          VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR, ("ERROR: WLANSAP_Start failed!!"));
          WLANSAP_Close(sapContext);
          pAdapter->sessionCtx.ap.sapContext = NULL;
          return status;
    }
    pAdapter->sessionId = session_id;
#endif

    // Allocate the Wireless Extensions state structure
    phostapdBuf = WLAN_HDD_GET_HOSTAP_STATE_PTR( pAdapter );

    sme_SetCurrDeviceMode(pHddCtx->hHal, pAdapter->device_mode);

    // Zero the memory.  This zeros the profile structure.
    memset(phostapdBuf, 0,sizeof(hdd_hostapd_state_t));

    // Set up the pointer to the Wireless Extensions state structure
    // NOP
    status = hdd_set_hostapd(pAdapter);
    if(!VOS_IS_STATUS_SUCCESS(status)) {
         VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,("ERROR: hdd_set_hostapd failed!!"));
#ifdef WLAN_FEATURE_MBSSID
         WLANSAP_Close(sapContext);
         pAdapter->sessionCtx.ap.sapContext = NULL;
#endif
         return status;
    }

    status = vos_event_init(&phostapdBuf->vosEvent);
    if (!VOS_IS_STATUS_SUCCESS(status))
    {
         VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR, ("ERROR: Hostapd HDD vos event init failed!!"));
#ifdef WLAN_FEATURE_MBSSID
         WLANSAP_Close(sapContext);
         pAdapter->sessionCtx.ap.sapContext = NULL;
#endif
         return status;
    }

    status = vos_event_init(&phostapdBuf->stop_bss_event);
    if (!VOS_IS_STATUS_SUCCESS(status))
    {
         VOS_TRACE(VOS_MODULE_ID_HDD,
                   VOS_TRACE_LEVEL_ERROR,
                   "ERROR: Hostapd HDD stop bss event init failed!!");
#ifdef WLAN_FEATURE_MBSSID
         WLANSAP_Close(sapContext);
         pAdapter->sessionCtx.ap.sapContext = NULL;
#endif
         return status;
    }

    status = vos_event_init(&phostapdBuf->sta_disassoc_event);
    if (!VOS_IS_STATUS_SUCCESS(status)) {
        VOS_TRACE(VOS_MODULE_ID_HDD, VOS_TRACE_LEVEL_ERROR,
            "ERROR: Hostapd HDD sta disassoc event init failed!!");
#ifdef WLAN_FEATURE_MBSSID
        WLANSAP_Close(sapContext);
        pAdapter->sessionCtx.ap.sapContext = NULL;
#endif
        return status;
    }

    sema_init(&(WLAN_HDD_GET_AP_CTX_PTR(pAdapter))->semWpsPBCOverlapInd, 1);

     // Register as a wireless device
#ifdef CONFIG_WIRELESS_EXT
    dev->wireless_handlers = (struct iw_handler_def *)& hostapd_handler_def;
#endif

    //Initialize the data path module
    status = hdd_softap_init_tx_rx(pAdapter);
    if ( !VOS_IS_STATUS_SUCCESS( status ))
    {
       hddLog(VOS_TRACE_LEVEL_FATAL, "%s: hdd_softap_init_tx_rx failed", __func__);
    }

    set_bit(INIT_TX_RX_SUCCESS, &pAdapter->event_flags);

    status = hdd_wmm_adapter_init( pAdapter );
    if (!VOS_IS_STATUS_SUCCESS(status))
    {
       hddLog(VOS_TRACE_LEVEL_ERROR,
             "hdd_wmm_adapter_init() failed with status code %08d [x%08x]",
                             status, status );
       goto error_wmm_init;
    }

    set_bit(WMM_INIT_DONE, &pAdapter->event_flags);

    if (!reinit) {
        pAdapter->sessionCtx.ap.sapConfig.acs_cfg.acs_mode = false;
        vos_mem_free(pAdapter->sessionCtx.ap.sapConfig.acs_cfg.ch_list);
        vos_mem_zero(&pAdapter->sessionCtx.ap.sapConfig.acs_cfg,
                                           sizeof(struct sap_acs_cfg));
    }
    return status;

error_wmm_init:
    hdd_softap_deinit_tx_rx( pAdapter );
    clear_bit(INIT_TX_RX_SUCCESS, &pAdapter->event_flags);
#ifdef WLAN_FEATURE_MBSSID
    WLANSAP_Close(sapContext);
    pAdapter->sessionCtx.ap.sapContext = NULL;
#endif
    EXIT();
    return status;
}

hdd_adapter_t* hdd_wlan_create_ap_dev(hdd_context_t *pHddCtx,
                                      tSirMacAddr macAddr,
                                      unsigned char name_assign_type,
                                      tANI_U8 *iface_name )
{
    struct net_device *pWlanHostapdDev = NULL;
    hdd_adapter_t *pHostapdAdapter = NULL;

   hddLog(VOS_TRACE_LEVEL_DEBUG, "%s: iface_name = %s", __func__, iface_name);

   pWlanHostapdDev = alloc_netdev_mq(sizeof(hdd_adapter_t),
                                     iface_name,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,17,0)) || defined(WITH_BACKPORTS)
                                     name_assign_type,
#endif
                                     ether_setup,
                                     NUM_TX_QUEUES);

    if (pWlanHostapdDev != NULL)
    {
        pHostapdAdapter = netdev_priv(pWlanHostapdDev);

        //Init the net_device structure
        ether_setup(pWlanHostapdDev);

        //Initialize the adapter context to zeros.
        vos_mem_zero(pHostapdAdapter, sizeof( hdd_adapter_t ));
        pHostapdAdapter->dev = pWlanHostapdDev;
        pHostapdAdapter->pHddCtx = pHddCtx;
        pHostapdAdapter->magic = WLAN_HDD_ADAPTER_MAGIC;

        hddLog(VOS_TRACE_LEVEL_DEBUG, "%s: pWlanHostapdDev = %pK, "
                                      "pHostapdAdapter = %pK, "
                                      "concurrency_mode=0x%x", __func__,
                                      pWlanHostapdDev,
                                      pHostapdAdapter,
                                      (int)vos_get_concurrency_mode());

        //Init the net_device structure
        strlcpy(pWlanHostapdDev->name, (const char *)iface_name, IFNAMSIZ);

        hdd_set_ap_ops( pHostapdAdapter->dev );

        pWlanHostapdDev->watchdog_timeo = HDD_TX_TIMEOUT;
        pWlanHostapdDev->mtu = HDD_DEFAULT_MTU;
        pWlanHostapdDev->tx_queue_len = HDD_NETDEV_TX_QUEUE_LEN;

        vos_mem_copy(pWlanHostapdDev->dev_addr, (void *)macAddr,sizeof(tSirMacAddr));
        vos_mem_copy(pHostapdAdapter->macAddressCurrent.bytes, (void *)macAddr, sizeof(tSirMacAddr));

        pHostapdAdapter->offloads_configured = FALSE;
        hdd_dev_setup_destructor(pWlanHostapdDev);
        pWlanHostapdDev->ieee80211_ptr = &pHostapdAdapter->wdev ;
        pHostapdAdapter->wdev.wiphy = pHddCtx->wiphy;
        pHostapdAdapter->wdev.netdev =  pWlanHostapdDev;
        vos_event_init(&pHostapdAdapter->scan_info.scan_finished_event);
        pHostapdAdapter->scan_info.scan_pending_option = WEXT_SCAN_PENDING_GIVEUP;
        /*
         * kernel will consume ethernet header length buffer for hard_header,
         * so just reserve it
         */
        hdd_set_needed_headroom(pWlanHostapdDev,
                           pWlanHostapdDev->hard_header_len);

        if (pHddCtx->cfg_ini->enableIPChecksumOffload)
            pWlanHostapdDev->features |= NETIF_F_HW_CSUM;
        else if (pHddCtx->cfg_ini->enableTCPChkSumOffld)
            pWlanHostapdDev->features |= NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM;
        pWlanHostapdDev->features |= NETIF_F_RXCSUM;

        SET_NETDEV_DEV(pWlanHostapdDev, pHddCtx->parent_dev);
        spin_lock_init(&pHostapdAdapter->pause_map_lock);
        pHostapdAdapter->last_tx_jiffies = jiffies;
        pHostapdAdapter->bug_report_count = 0;
        pHostapdAdapter->start_time =
            pHostapdAdapter->last_time = vos_system_ticks();
    }
    return pHostapdAdapter;
}

VOS_STATUS hdd_register_hostapd( hdd_adapter_t *pAdapter, tANI_U8 rtnl_lock_held )
{
   struct net_device *dev = pAdapter->dev;
   VOS_STATUS status = VOS_STATUS_SUCCESS;

   ENTER();

   if( rtnl_lock_held )
   {
     if (strnchr(dev->name, strlen(dev->name), '%')) {
         if( dev_alloc_name(dev, dev->name) < 0 )
         {
            hddLog(VOS_TRACE_LEVEL_FATAL, "%s:Failed:dev_alloc_name", __func__);
            return VOS_STATUS_E_FAILURE;
         }
      }
      if (register_netdevice(dev))
      {
         hddLog(VOS_TRACE_LEVEL_FATAL,
                "%s:Failed:register_netdevice", __func__);
         return VOS_STATUS_E_FAILURE;
      }
   }
   else
   {
      if (register_netdev(dev))
      {
         hddLog(VOS_TRACE_LEVEL_FATAL, "%s: Failed:register_netdev", __func__);
         return VOS_STATUS_E_FAILURE;
      }
   }
   set_bit(NET_DEVICE_REGISTERED, &pAdapter->event_flags);

   EXIT();
   return status;
}

VOS_STATUS hdd_unregister_hostapd(hdd_adapter_t *pAdapter, bool rtnl_held)
{
#ifdef WLAN_FEATURE_MBSSID
   VOS_STATUS status;
   v_PVOID_t sapContext=WLAN_HDD_GET_SAP_CTX_PTR(pAdapter);
#endif

   ENTER();

   /* if we are being called during driver unload, then the dev has already
      been invalidated.  if we are being called at other times, then we can
      detach the wireless device handlers */
   if (pAdapter->dev)
   {
#ifdef CONFIG_WIRELESS_EXT
      if (rtnl_held)
          pAdapter->dev->wireless_handlers = NULL;
      else {
          rtnl_lock();
          pAdapter->dev->wireless_handlers = NULL;
          rtnl_unlock();
      }
#endif
   }

#ifdef WLAN_FEATURE_MBSSID
   status = WLANSAP_Stop(sapContext);
   if ( ! VOS_IS_STATUS_SUCCESS( status ) ) {
         hddLog(VOS_TRACE_LEVEL_FATAL, "%s: Failed:WLANSAP_Stop", __func__);
   }

   status = WLANSAP_Close(sapContext);
   if ( ! VOS_IS_STATUS_SUCCESS( status ) ) {
         hddLog(VOS_TRACE_LEVEL_FATAL, "%s: Failed:WLANSAP_close", __func__);
   }
   pAdapter->sessionCtx.ap.sapContext = NULL;
#endif

   EXIT();
   return 0;
}

/**
 * hdd_sap_indicate_disconnect_for_sta() - Indicate disconnect indication
 * to supplicant, if there any clients connected to SAP interface.
 * @adapter: sap adapter context
 *
 * Return:   nothing
 */
void hdd_sap_indicate_disconnect_for_sta(hdd_adapter_t *adapter)
{
	tSap_Event sap_event;
	int staId;
	ptSapContext sap_ctx = WLAN_HDD_GET_SAP_CTX_PTR(adapter);

	ENTER();

	if (!sap_ctx) {
		hddLog(LOGE, FL("invalid sap context"));
		return;
	}

	for (staId = 0; staId < WLAN_MAX_STA_COUNT; staId++) {
		if (adapter->aStaInfo[staId].isUsed) {
			hddLog(LOG1, FL("staId: %d isUsed: %d %pK"),
				staId, adapter->aStaInfo[staId].isUsed,
				sap_ctx);

			if (vos_is_macaddr_broadcast(
				&adapter->aStaInfo[staId].macAddrSTA))
				continue;

			sap_event.sapHddEventCode = eSAP_STA_DISASSOC_EVENT;
			vos_mem_copy(
				&sap_event.sapevt.
					sapStationDisassocCompleteEvent.staMac,
				&adapter->aStaInfo[staId].macAddrSTA,
				sizeof(v_MACADDR_t));
			sap_event.sapevt.sapStationDisassocCompleteEvent.
			reason =
				eSAP_MAC_INITATED_DISASSOC;
			sap_event.sapevt.sapStationDisassocCompleteEvent.
			statusCode =
				eSIR_SME_RESOURCES_UNAVAILABLE;
			hdd_hostapd_SAPEventCB(&sap_event,
				sap_ctx->pUsrContext);
		}
	}

	clear_bit(SOFTAP_BSS_STARTED, &adapter->event_flags);

	EXIT();
}

/**
 * hdd_sap_destroy_events() - Destroy sap evets
 * @adapter: sap adapter context
 *
 * Return:   nothing
 */
void hdd_sap_destroy_events(hdd_adapter_t *adapter)
{
	ptSapContext sap_ctx = WLAN_HDD_GET_SAP_CTX_PTR(adapter);

	if (!sap_ctx) {
	hddLog(LOGE, FL("invalid sap context"));
	return;
	}

	if (!VOS_IS_STATUS_SUCCESS(vos_lock_destroy(&sap_ctx->SapGlobalLock)))
		VOS_TRACE(VOS_MODULE_ID_SAP, VOS_TRACE_LEVEL_ERROR,
		FL("WLANSAP_Stop failed destroy lock"));

	if (!VOS_IS_STATUS_SUCCESS(vos_event_destroy(
		&sap_ctx->sap_session_opened_evt)))
		VOS_TRACE(VOS_MODULE_ID_SAP, VOS_TRACE_LEVEL_ERROR,
		FL("failed to destroy session open event"));
        if (!VOS_IS_STATUS_SUCCESS(vos_event_destroy(
               &sap_ctx->sap_session_closed_evt)))
            VOS_TRACE(VOS_MODULE_ID_SAP, VOS_TRACE_LEVEL_ERROR,
               FL("failed to destroy session close event"));

}
