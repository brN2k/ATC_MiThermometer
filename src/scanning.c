/*
 * scaning.c
 *
 *  Created on: 31 янв. 2025 г.
 *      Author: pvvx
 */

#include "tl_common.h"
#include "drivers.h"
#include "app_config.h"
#if USE_SYNC_SCAN
#include "ble.h"
#include "vendor/common/blt_common.h"
#include "app.h"
#include "lcd.h"
#include "flash_eep.h"
#include "scanning.h"
#include "bthome_beacon.h"
#if USE_SYNC_SCAN == 3
#include "sync_v3_logic.h"
#ifndef TIME_SYNC_SERVER_ID
#define TIME_SYNC_SERVER_ID 0xA1B2C3D4UL
#endif
#define SYNC_V3_SCAN_WINDOW_MS 2500UL
#endif
#if (DEV_SERVICES & SERVICE_HARD_CLOCK)
#include "rtc.h"
#endif
#if SCAN_USE_BINDKEY
#include "ccm.h"
#endif

//#warning "USE_SYNC_SCAN - Test only!"


#if !(DEV_SERVICES & SERVICE_RDS)
#error "This feature will not work unless 'SERVICE_RDS' is set!"
#endif

// Maximum allowed drift before next sync (seconds).
// Adaptive algorithm targets: next_interval = SCAN_MAX_DRIFT_SEC * current_interval / measured_drift
#define SCAN_MAX_DRIFT_SEC	30

#define SCAN_ADV_INTERVAL	ADV_INTERVAL_505MS
#define SCAN_ADV_COUNT	3

typedef struct __attribute__((packed)) _ad_uuid16_t {
	u8 size;
	u8 type;
	u16 uuid16;
    u8 data[1]; // 1 - for check min length
} ad_uuid16_t, * pad_uuid16_t;

// BTHOME scan event, no security
typedef struct __attribute__((packed)) {
	adv_head_bth_t	head;
	u8	info;	// = 0x40 BtHomeID_Info
	u8	p_id;	// = BtHomeID_PacketId
	u8	pid;	// PacketId (!= measurement count)
	u8	t_id;	// = BtHomeID_timestamp
	u32	time;
} adv_bthome_ns_scan_t, * padv_bthome_ns_scan_t;

#if USE_SYNC_SCAN == 3
/* 4-byte service header + the exact 18-byte draft-v1 request payload. */
typedef struct __attribute__((packed)) {
	adv_head_bth_t head;
	u8 info;
	u8 packet_tag;
	u8 packet_id;
	u8 timestamp_tag;
	u32 utc;
	u8 raw_tag;
	u8 raw_len;
	u8 magic[2];
	u8 protocol_version;
	u8 role;
	u32 server_id;
} adv_bthome_sync_v3_t;
typedef char sync_v3_request_size_check[(sizeof(adv_bthome_sync_v3_t) == 22) ? 1 : -1];
#endif

#if USE_SYNC_SCAN != 3
// BTHome v2
// bit[4:5]: 0 - 1, 1 - 0.1, 2 - 0.01, 3 - 0.001
// bit[6]: bool (on/off)
// bit[7]: signed
// 0x - unsigned 1
// 1x - unsigned 0.1
// 2x - unsigned 0.01
// 3x - unsigned 0.001
// 8x - signed 1
// 9x - signed 0.1
// Ax - signed 0.01
// Bx - signed 0.001

const u8 tblBTHome[] = {
//  0     1     2     3     4     5     6     7     8     9   	a     b     c     d     e     f
	0x01, 0x01, 0xA2, 0x21, 0x23, 0x23, 0x22, 0x22, 0xA2, 0x01, 0x33, 0x23, 0x32, 0x02, 0x02, 0x41, // 0x
	0x41, 0x41, 0x02, 0x02, 0x32, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, // 1x
	0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x01, 0x01, // 2x
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x02, 0x04, 0x12, // 3x
	0x02, 0x12, 0x33, 0x32, 0x22, 0x12, 0x11, 0x12, 0x02, 0x32, 0x12, 0x33, 0x34, 0x34, 0x34, 0x34, // 4x
	0x04, 0x32, 0x32, 0x0f, 0x0f, 0x34, 0x02, 0x81, 0x81, 0x81, 0x82, 0x84, 0xA4, 0xB2, 0x22, 0x22, // 5x
	0x81 // 6x
/*
	0xF0	device type id	uint16 (2 bytes)	F00100	1
	0xF1	firmware version	uint32 (4 bytes)	F100010204	4.2.1.0
	0xF2	firmware version	uint24 (3 bytes)	F1000106	6.1.0
*/
};

#if SACN_USE_BINDKEY
const u8 ccm_aad = 0x11;
#endif

bthome_beacon_nonce_t scan_bthome_nonce;

__attribute__((optimize("-Os")))
void filter_bthome_ad(padv_bthome_t p, u8 * pmac) {
	int len = p->size;
	if(len > sizeof(padv_bthome_t)) {
		len -= sizeof(adv_bthome_t) - 2; // p->data len
		if(p->ver == BtHomeID_Info_Encrypt && len > 9) {
#if SCAN_USE_BINDKEY
			len -= 8;
			bthome_beacon_nonce_t scan_bthome_nonce;
			//memcpy(bthome_nonce.mac, dev_MAC[n], sizeof(bthome_nonce.mac));
			scan_bthome_nonce.mac[5] = pmac[0];
			scan_bthome_nonce.mac[4] = pmac[1];
			scan_bthome_nonce.mac[3] = pmac[2];
			scan_bthome_nonce.mac[2] = pmac[3];
			scan_bthome_nonce.mac[1] = pmac[4];
			scan_bthome_nonce.mac[0] = pmac[5];
			u8 *pb = (u8 *)&scan_bthome_nonce.uuid16;
			u8 *pmic = (u8 *)&p->UUID;
			// UUID16, ver
			*pb++ = *pmic++;
			*pb++ = *pmic++;
			*pb++ = *pmic;
			// count32
			pmic = &p->data[len];
			*pb++ = *pmic++;
			*pb++ = *pmic++;
			*pb++ = *pmic++;
			*pb = *pmic++; // pm = &mic[4]
			if(aes_ccm_auth_decrypt((const unsigned char *)scan.bindkey,
					(u8 *)&scan_bthome_nonce, sizeof(scan_bthome_nonce),
					NULL, 0,
					p->data, len, // len crypt_data
					p->data, // decrypt data
					pmic, 4))  // &mic: &crypt_data[len + size (ext_cnt[3])]
#endif
				return;
		} else if(p->ver != BtHomeID_Info) {
			return;
		}
		padv_bthome_sruct_t ps = (padv_bthome_sruct_t)&p->data;
		int size;
		while(len > 0) {
			if(ps->type < sizeof(tblBTHome)) {
				if(ps->type == BtHomeID_timestamp) { // in 1 sec
#if USE_SYNC_SCAN == 2
					u32 new_time = ps->data_uw;
					s32 drift = (s32)(new_time - wrk.utc_time_sec);
					if(drift < 0) drift = -drift;
					wrk.utc_time_sec = new_time;
#if (DEV_SERVICES & SERVICE_HARD_CLOCK)
					rtc_set_utime(wrk.utc_time_sec);
#endif
					// Adaptive interval: skip first sync (initial drift is meaningless)
					if(scan.sync_count > 0 && drift > 0) {
						u32 new_interval = (SCAN_MAX_DRIFT_SEC * scan.cfg.interval) / (u32)drift;
						if(new_interval < 1800) new_interval = 1800;      // min 30 min
						if(new_interval > 1296000) new_interval = 1296000; // max 15 days
						scan.cfg.interval = new_interval;
					}
					scan.fail_count = 0;
					if(scan.sync_count < 255) scan.sync_count++;
					// Shrink scan window after 3 consecutive successes (floor 50ms)
					if(scan.sync_count >= 3 && scan.window_ms > 50)
						scan.window_ms /= 2;
#else
					wrk.utc_time_sec = ps->data_uw; // + scan.cfg.localt;
#if (DEV_SERVICES & SERVICE_HARD_CLOCK)
					rtc_set_utime(wrk.utc_time_sec);
#endif
#endif
#if 0 //(DEV_SERVICES & SERVICE_SCREEN)
				} else if(ps->type == BtHomeID_raw) { // Show ext. small and big number
					size = ps->data_ub[0];
					if (size <= sizeof(ext)) {
						memcpy(&ext, &ps->data_ub[1], size);
						if(ext.vtime_sec == 0xffff)
							lcd_flg.chow_ext_ut = 0xffffffff;
						else {
							lcd_flg.chow_ext_ut = wrk.utc_time_sec + ext.vtime_sec;
							if(ext.vtime_sec >= 128)
								scan.cfg.interval = ext.vtime_sec - 8;
							else
								scan.cfg.interval = 120;
						}
#if (DEVICE_TYPE == DEVICE_MJWSD05MMC) || (DEVICE_TYPE == DEVICE_MJWSD05MMC_EN) || (DEVICE_TYPE == DEVICE_LYWSD02MMC) // TODO: 02?
						SET_LCD_UPDATE();
#else
						lcd_flg.update_next_measure = 0;
#endif
					}
#endif // DEV_SERVICES & SERVICE_SCREEN

#if	0
				} else if(ps->type == BtHomeID_temperature) { // in 0.01 C
					ext_measure.temperature = ps->data_is[0];
					ext_measure.update |= FLG_UPDATE_TEMP | FLG_UPDATE_FLG;
				} else if(ps->type == BtHomeID_temperature_01) {  // in 0.1 C
					ext_measure.temperature = ps->data_is[0]*10;
					ext_measure.update |= FLG_UPDATE_HUMI | FLG_UPDATE_FLG;
				} else if(ps->type == BtHomeID_humidity) { // in 0.01 %
					ext_measure.humidity = ps->data_us[0];
					ext_measure.update |= FLG_UPDATE_HUMI | FLG_UPDATE_FLG;
				} else if(ps->type == BtHomeID_humidity8) { // in 1 %
					ext_measure.humidity = ps->data_ub[0]*100;
					ext_measure.update |= FLG_UPDATE_HUMI | FLG_UPDATE_FLG;
				} else if(ps->type == BtHomeID_battery) { // Batt in %
					ext_measure.battery = ps->data_ub[0];
					ext_measure.update |= FLG_UPDATE_BAT | FLG_UPDATE_FLG;
				} else if(ps->type == BtHomeID_voltage) { // in 0.001V
					ext_measure.voltage = ps->data_us[0];
					ext_measure.update |= FLG_UPDATE_VBAT | FLG_UPDATE_FLG;
#endif
				}
			} else
				break;
			size = (tblBTHome[ps->type] & 0x0f) + 1;
			if(size == 0x10)
				size = ps->data_ub[0] + 2;
			len -= size;
			ps = (padv_bthome_sruct_t)((u32)ps + size);
		}
	}
}
#endif /* legacy BTHome parser */

RAM scan_wrk_t scan;

#if USE_SYNC_SCAN == 3
/* All exit paths must stop the controller as well as the retained flags. */
void scan_stop(void) {
	if(scan.start_tik) {
		blc_ll_setScanEnable(BLC_SCAN_DISABLE, DUP_FILTER_DISABLE);
		blc_ll_removeScanningFromAdvState();
	}
	if(scan.v3_inflight)
		bls_ll_setAdvDuration(0, 0);
	scan.enabled = 0;
	scan.start_tik = 0;
	scan.v3_inflight = 0;
}

void scan_v3_abort(void) {
	if(scan.v3_inflight) {
		scan.v3_next_sync_utc = wrk.utc_time_sec +
			(scan.v3_has_ref ? SYNC_V3_FAILED_RETRY_SEC : SYNC_V3_BOOT_RETRY_SEC);
		scan_stop();
	}
}

/* A manual GATT clock/rate change invalidates the drift measurement. */
void scan_v3_clock_changed(void) {
	scan_stop();
	scan.v3_has_ref = 0;
	scan.v3_has_drift = 0;
	scan.v3_drift_ppm_x100 = 0;
	scan.v3_last_sample_ppm_x100 = 0;
	scan.v3_sample_elapsed = 0;
	scan.v3_sample_error = 0;
	scan.v3_next_sync_utc = 0;
}

static void scan_v3_restore_advertising(void) {
	if(!wrk.ble_connected) {
		/* Drop the request from the controller; next ADV prepare rebuilds data. */
		adv_buf.data_size = 0;
		adv_buf.update_count = 0;
		load_adv_data();
		ev_adv_timeout(0, 0, 0);
	}
}

/* Only called on a successfully validated response. Do not derive drift
 * on the first sync after a cold boot; also skip short/implausible samples.
 */
static void scan_v3_accept_time(u32 server_utc) {
	u32 before = wrk.utc_time_sec;
#if (DEV_SERVICES & SERVICE_HARD_CLOCK)
	before = rtc_get_utime(); /* external RTC, not a possibly stale shadow */
#endif
	if(scan.v3_has_ref) {
		u32 elapsed = server_utc - scan.v3_last_sync_utc;
		s32 error = sync_v3_delta(before, server_utc); /* positive = clock is fast */
		/* A six-hour schedule must still learn: sum corrections across short
		 * syncs instead of discarding every sample and freezing the estimate. */
		if(!elapsed || elapsed > SYNC_V3_LEARN_MAX_SEC ||
			sync_v3_abs32(error) > SYNC_V3_MAX_SAMPLE_ERROR_SEC ||
			scan.v3_sample_elapsed > SYNC_V3_LEARN_MAX_SEC - elapsed) {
			scan.v3_has_drift = 0;
			scan.v3_sample_elapsed = 0;
			scan.v3_sample_error = 0;
		} else {
			scan.v3_sample_elapsed += elapsed;
			scan.v3_sample_error += error;
		}
		if(sync_v3_valid_sample(scan.v3_sample_error, scan.v3_sample_elapsed)) {
			s32 sample = sync_v3_drift_sample_x100(scan.v3_sample_error, scan.v3_sample_elapsed);
			scan.v3_last_sample_ppm_x100 = sample;
			if(scan.v3_has_drift)
				scan.v3_drift_ppm_x100 = sync_v3_smooth_x100(scan.v3_drift_ppm_x100, sample);
			else
				scan.v3_drift_ppm_x100 = sample;
			scan.v3_has_drift = 1;
			scan.v3_sample_elapsed = 0;
			scan.v3_sample_error = 0;
		} else if(sync_v3_abs32(scan.v3_sample_error) > SYNC_V3_MAX_SAMPLE_ERROR_SEC) {
			scan.v3_has_drift = 0;
			scan.v3_sample_elapsed = 0;
			scan.v3_sample_error = 0;
		}
	}
	wrk.utc_time_sec = server_utc;
	wrk.utc_time_sec_tick = clock_time();
#if (DEV_SERVICES & SERVICE_HARD_CLOCK)
	rtc_set_utime(server_utc);
#endif
	scan.v3_last_sync_utc = server_utc;
	scan.v3_has_ref = 1;
	scan.v3_next_sync_utc = server_utc + (scan.v3_has_drift ?
			sync_v3_conservative_interval_sec(scan.v3_drift_ppm_x100, scan.v3_last_sample_ppm_x100) :
			SYNC_V3_FIRST_INTERVAL_SEC);
	SET_LCD_UPDATE();
}

/* data has already been length-checked by scanning_event_callback.
 * A random HA transmitter MAC is okay; the in-payload server ID is checked.
 * This ID is an identifier, NOT an authentication mechanism.
 */
static int scan_v3_process_reply(const u8 *data) {
	uint32_t timestamp;
	if(!scan.v3_inflight || !scan.start_tik ||
		!sync_v3_decode_response(data, SYNC_V3_SERVICE_LENGTH, (u32)TIME_SYNC_SERVER_ID, &timestamp))
		return 0;
	scan_v3_accept_time(timestamp);
	return 1;
}

/* Start at boot, or on the scheduled device-specific UTC deadline.
 * Only one request/listen transaction may be active at a time.
 */
void scan_v3_poll(void) {
	if(scan.v3_inflight) {
		/* Watchdog: recovery if BLE never transitions from request to RX. */
		if(!scan.start_tik && clock_time() - scan.v3_request_tick >=
				10UL * CLOCK_16M_SYS_TIMER_CLK_1S) {
			scan_v3_abort();
			scan_v3_restore_advertising();
		}
		return;
	}
	if(scan.v3_next_sync_utc &&
		!sync_v3_due(wrk.utc_time_sec, scan.v3_next_sync_utc))
		return;
	if(adv_buf.ext_adv_init != EXT_ADV_Off || wrk.ble_connected || blta.adv_duraton_en)
		return;
	scan.v3_inflight = 1;
	scan.v3_request_tick = clock_time();
	scan.v3_next_sync_utc = 0;
	scan_start();
}
#endif /* USE_SYNC_SCAN == 3 */

u8 prev_advs[32];
//////////////////////////////////////////////////////////
// scan event call back
//////////////////////////////////////////////////////////
//_attribute_ram_code_
//__attribute__((optimize("-Os")))
int scanning_event_callback(u32 h, u8 *p, int n) {
#if USE_SYNC_SCAN == 3
	/* One legacy report: 11-byte header, data, RSSI. Validate before p[0]. */
	if(!p || n < 12)
		return 0;
#endif
	if (h & HCI_FLAG_EVENT_BT_STD) { // ble controller hci event
		if ((h & 0xff) == HCI_EVT_LE_META) {
			//----- hci le event: le adv report event -----
			if (p[0] == HCI_SUB_EVT_LE_ADVERTISING_REPORT) { // ADV packet
				// after controller is set to scan state, it will report all the adv packet it received by this event
				event_adv_report_t *pa = (event_adv_report_t *) p;
#if USE_SYNC_SCAN == 3
				/* Only a complete v3 response with the configured identifier
				 * can end scanning. Other BTHome packets are ignored.
				 */
				u32 adlen = pa->len;
				if(pa->nreport == 1 && adlen && adlen <= 31 && n >= 12 + (int)adlen) {
					u32 i = 0;
					while(i < adlen) {
						u32 field_len = pa->data[i];
						if(!field_len || field_len >= adlen - i)
							break; /* zero terminator, truncation or malformed */
						if(field_len == 21 &&
							pa->data[i+1] == GAP_ADTYPE_SERVICE_DATA_UUID_16BIT &&
							pa->data[i+2] == 0xD2 && pa->data[i+3] == 0xFC &&
							scan_v3_process_reply(&pa->data[i+4])) {
							scan_stop();
							scan_v3_restore_advertising();
							break;
						}
						i += field_len + 1;
					}
				}
#elif USE_SYNC_SCAN == 2
				// MAC filter: all-zero accepts any, otherwise match configured MAC
				u8 mac_any = 1;
				int k;
				for(k = 0; k < sizeof(scan.cfg.MAC); k++) {
					if(scan.cfg.MAC[k] != 0) { mac_any = 0; break; }
				}
				if(mac_any || memcmp(scan.cfg.MAC, pa->mac, sizeof(scan.cfg.MAC)) == 0) {
					u32 adlen = pa->len;
					u8 rssi = pa->data[adlen];
					if (adlen && adlen < 32 && rssi != 0) {
						u32 i = 0;
						while(adlen) {
							pad_uuid16_t pd = (pad_uuid16_t) &pa->data[i];
							u32 len = pd->size + 1;
							if(len <= adlen) {
								if(len >= sizeof(ad_uuid16_t) && pd->type == GAP_ADTYPE_SERVICE_DATA_UUID_16BIT) {
									if((pd->uuid16) == ADV_BTHOME_UUID16) { 	
										memcpy(prev_advs, pd, len);
										filter_bthome_ad((adv_bthome_t *)prev_advs, pa->mac);
										blc_ll_setScanEnable(BLC_SCAN_DISABLE, DUP_FILTER_DISABLE);
										scan.start_tik = 0;
										blta.adv_duraton_en = 0;
										scan_stop();
									}
								}
							} else
								break;
							adlen -= len;
							i += len;
						}
					}
				}
#else
				if(memcmp(scan.cfg.MAC, pa->mac, sizeof(scan.cfg.MAC)) == 0) {
					blc_ll_setScanEnable(BLC_SCAN_DISABLE, DUP_FILTER_DISABLE); // отсановить сканирование
					scan.start_tik = 0; // enable sleep
					u32 adlen = pa->len;
					u8 rssi = pa->data[adlen];
					if (adlen && adlen < 32 && rssi != 0) { // rssi != 0
						u32 i = 0;
						while(adlen) {
							pad_uuid16_t pd = (pad_uuid16_t) &pa->data[i];
							u32 len = pd->size + 1;
							if(len <= adlen) {
								if(len >= sizeof(ad_uuid16_t) && pd->type == GAP_ADTYPE_SERVICE_DATA_UUID_16BIT) {
									if((pd->uuid16) == ADV_BTHOME_UUID16) { // GATT Service: BTHome v2
										memcpy(prev_advs, pd, len);
										filter_bthome_ad((adv_bthome_t *)prev_advs, pa->mac);
										blta.adv_duraton_en = 0; // reload adv
										scan_stop(); // stop scan
									}
								}
							} else
								break;
							adlen -= len;
							i += len;
						}
					}
				}
#endif
			}
		}
	}
	return 0;
}

//////////////////////////////////////////////////////////
// start wakeup if(scan.enabled) scan_wakeup();
//////////////////////////////////////////////////////////
void scan_wakeup(void) {
#if USE_SYNC_SCAN == 3
	if(!scan.v3_inflight || !scan.enabled || scan.start_tik || wrk.ble_connected)
		return;
#endif
	scan.start_tik = clock_time() | 1;
	//scan setting
	blc_ll_initScanning_module(mac_public);
	//bluetooth low energy(LE) event
	blc_hci_le_setEventMask_cmd(HCI_LE_EVT_MASK_ADVERTISING_REPORT | HCI_LE_EVT_MASK_SCAN_REQUEST_RECEIVED);
	blc_hci_registerControllerEventHandler(scanning_event_callback); //controller hci event to host all processed in this func
	//set scan parameter and scan enable
	blc_ll_setScanParameter(SCAN_TYPE_PASSIVE, SCAN_INTERVAL_100MS, SCAN_INTERVAL_100MS,
							  OWN_ADDRESS_PUBLIC, SCAN_FP_ALLOW_ADV_ANY);
	blc_ll_setScanEnable(BLC_SCAN_ENABLE, DUP_FILTER_DISABLE);

	blc_ll_addScanningInAdvState();  //add scan in adv state
#if USE_SYNC_SCAN == 3
	/* Never enter retention between enabling RX and the first scan_task(). */
	bls_pm_setSuspendMask(SUSPEND_DISABLE);
#endif
	//blc_ll_addScanningInConnSlaveRole();  //add scan in conn slave role
}

//////////////////////////////////////////////////////////
// scan init
//////////////////////////////////////////////////////////
void scan_init(void) {

#if USE_SYNC_SCAN == 3
	if(flash_read_cfg(&scan.cfg, EEP_ID_SCN, sizeof(scan.cfg)) != sizeof(scan.cfg))
		memset(&scan.cfg, 0, sizeof(scan.cfg));
	/* Ignore the saved PVVX adaptive interval and old MAC filter. */
	scan.v3_next_sync_utc = 0; /* immediate bootstrap after boot */
	scan.v3_last_sync_utc = 0;
	scan.v3_request_tick = 0;
	scan.v3_drift_ppm_x100 = 0;
	scan.v3_last_sample_ppm_x100 = 0;
	scan.v3_sample_elapsed = 0;
	scan.v3_sample_error = 0;
	scan.v3_has_ref = 0;
	scan.v3_has_drift = 0;
	scan.v3_inflight = 0;
	scan_stop();
#else
	/* Keep the legacy initialization (including mode 2) unchanged. */
	if(flash_read_cfg(&scan.cfg, EEP_ID_SCN, sizeof(scan.cfg)) != sizeof(scan.cfg)) {
		memset(&scan.cfg, 0, sizeof(scan.cfg));
#if USE_SYNC_SCAN == 2
		scan.cfg.interval = 3600; // default: scan every hour
	}
	if(scan.cfg.interval == 0)
		scan.cfg.interval = 3600; // ensure scan is enabled
	scan.start_time = 0;    // trigger immediate first scan on boot
	scan.window_ms = 1800;  // initial window: guaranteed catch (~1.58s beacon)
#else
	}
	scan_stop();
#endif
#endif
#if SCAN_DEBUG
	// Test values:
	scan.cfg.interval = 30; // 30 sec for test
	scan.cfg.MAC[0] = 0x01; // test MAC
	scan.cfg.MAC[1] = 0x02;
	scan.cfg.MAC[2] = 0x03;
	scan.cfg.MAC[3] = 0x04;
	scan.cfg.MAC[4] = 0x05;
	scan.cfg.MAC[5] = 0x06;
#endif
}

//////////////////////////////////////////////////////////
// scan start if (adv_buf.ext_adv_init != EXT_ADV_Off) // not support extension advertise
//////////////////////////////////////////////////////////
void scan_start(void) {
	// init new adv
	bls_ll_setAdvEnable(BLC_ADV_DISABLE);  // adv disable
	bls_ll_setAdvParam(SCAN_ADV_INTERVAL, SCAN_ADV_INTERVAL,
			ADV_TYPE_NONCONNECTABLE_UNDIRECTED, OWN_ADDRESS_PUBLIC, 0, NULL,
			BLT_ENABLE_ADV_ALL, ADV_FP_NONE);
	//set_scan_adv_data(); // set scan adv data
	adv_buf.send_count++;
#if USE_SYNC_SCAN == 3
	adv_bthome_sync_v3_t *p = (adv_bthome_sync_v3_t *)&adv_buf.data;
	p->head.size = sizeof(*p) - sizeof(p->head.size);
	p->head.type = GAP_ADTYPE_SERVICE_DATA_UUID_16BIT;
	p->head.UUID = ADV_BTHOME_UUID16;
	p->info = BtHomeID_Info;
	p->packet_tag = BtHomeID_PacketId;
	p->packet_id = (u8)adv_buf.send_count;
	p->timestamp_tag = BtHomeID_timestamp;
	p->utc = wrk.utc_time_sec;
	p->raw_tag = BtHomeID_raw;
	p->raw_len = 8;
	p->magic[0] = 'T'; p->magic[1] = 'S';
	p->protocol_version = SYNC_V3_PROTOCOL_VERSION;
	p->role = SYNC_V3_ROLE_REQUEST;
	p->server_id = (u32)TIME_SYNC_SERVER_ID;
	adv_buf.data_size = sizeof(*p);
#else
	padv_bthome_ns_scan_t p = (padv_bthome_ns_scan_t)&adv_buf.data;
	p->head.size = sizeof(adv_bthome_ns_scan_t) - sizeof(p->head.size);
	p->head.type = GAP_ADTYPE_SERVICE_DATA_UUID_16BIT; // 16-bit UUID
	p->head.UUID = ADV_BTHOME_UUID16;
	p->info = BtHomeID_Info;
	p->p_id = BtHomeID_PacketId;
	p->pid = (u8)adv_buf.send_count;
	p->t_id = BtHomeID_timestamp;
	p->time = wrk.utc_time_sec; // - scan.cfg.localt;
	adv_buf.data_size = sizeof(adv_bthome_ns_scan_t);
#endif
	adv_buf.update_count = 0; // refresh adv_buf.data in next set_adv_data()
	load_adv_data();
	adv_buf.data_size = 0; // flag adv_buf.send_count++ over adv.event
	bls_ll_setAdvDuration(SCAN_ADV_INTERVAL*SCAN_ADV_COUNT*625+33, 1);
	//blta.adv_interval = 500*CLOCK_16M_SYS_TIMER_CLK_1MS; // system tick
	bls_ll_setAdvEnable(BLC_ADV_ENABLE);  // adv enable
	scan.enabled = 1;
}

//////////////////////////////////////////////////////////
// scan task if scan.enabled && scan.start_tik && !wrk.ble_connected
//////////////////////////////////////////////////////////
_attribute_ram_code_
__attribute__((optimize("-Os")))
void scan_task(void) {
	u32 tt = clock_time() - scan.start_tik;
#if USE_SYNC_SCAN == 3
	if(tt >= SYNC_V3_SCAN_WINDOW_MS * CLOCK_16M_SYS_TIMER_CLK_1MS) {
		/* Fixed window: no PVVX spring-back/frequency adaptation. */
		/* If no reference was ever received, retry once per hour.
		 * Otherwise, retry next day; no hourly scan for precise sensors.
		 */
		scan_v3_abort();
		scan_v3_restore_advertising();
#ifdef SET_NO_SLEEP_MODE
		bls_pm_setSuspendMask(SET_NO_SLEEP_MODE);
#else
		bls_pm_setSuspendMask(SUSPEND_ADV | SUSPEND_CONN | DEEPSLEEP_RETENTION_ADV);
#endif
	} else {
		bls_pm_setSuspendMask(SUSPEND_DISABLE);
	}
#else
#if USE_SYNC_SCAN == 2
	if(tt > scan.window_ms * CLOCK_16M_SYS_TIMER_CLK_1MS) {
		// Scan window expired without catching beacon
		if(scan.fail_count < 255) scan.fail_count++;
		scan.sync_count = 0;
		// Spring-back: widen window (cap 1800ms) + increase frequency (floor 1800s)
		if(scan.window_ms < 1800)
			scan.window_ms = (scan.window_ms * 2 > 1800) ? 1800 : scan.window_ms * 2;
		if(scan.cfg.interval > 1800)
			scan.cfg.interval /= 2;
#else
	if(tt > 9*CLOCK_16M_SYS_TIMER_CLK_1MS) {
#endif
		blc_ll_setScanEnable(BLC_SCAN_DISABLE, DUP_FILTER_DISABLE); // остановить сканирование
		scan.start_tik = 0;
#ifdef SET_NO_SLEEP_MODE
		bls_pm_setSuspendMask(SET_NO_SLEEP_MODE);
#else
		bls_pm_setSuspendMask(SUSPEND_ADV | SUSPEND_CONN | DEEPSLEEP_RETENTION_ADV);
#endif
	} else {
		bls_pm_setSuspendMask(SUSPEND_DISABLE);
	}
#endif /* USE_SYNC_SCAN == 3 */
}

#endif // USE_SYNC_SCAN
