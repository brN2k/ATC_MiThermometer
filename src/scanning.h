/*
 * scanning.h
 *
 *  Created on: 31 янв. 2025 г.
 *      Author: pvvx
 */

#ifndef _SCANING_H_
#define _SCANING_H_

#define SCAN_USE_BINDKEY	0

// saved to EEP_ID_SCN
typedef struct {
	u32 interval;		// интервал вызова в сек, =0 - отключено
	u8 	MAC[6]; 		// MAC сервера [0] - lo, .. [6] - hi digits
#if SCAN_USE_BINDKEY
	u8 	bindkey[16]; 	// for ext dev MAC
#endif
} scan_cfg_t;

typedef struct {
	u32	start_tik;		// = 0 - сканирование отключено (разрешить sleep), !=0 - штамп времени старта сканирования
	u32	start_time;		// = wrk.utc_time_sec при старте каждого интервала
	scan_cfg_t cfg;
	u8 	enabled;
#if USE_SYNC_SCAN == 2
	u8	sync_count;		// consecutive successful syncs (adaptive window)
	u8	fail_count;		// consecutive scan misses (spring-back)
	u16	window_ms;		// current scan window in ms (adaptive: 50..1800)
#elif USE_SYNC_SCAN == 3
	u32	v3_next_sync_utc;	// 0 = bootstrap/due as soon as radio is free
	u32	v3_request_tick; // monotonic system tick for 10-second request watchdog
	u32	v3_last_sync_utc;	// reference time of last successful sync
	s32	v3_drift_ppm_x100;	// signed frequency error: ppm * 100
	s32	v3_last_sample_ppm_x100; // latest rate also bounds the next interval
	u32	v3_sample_elapsed;	// accumulate short syncs to a >=12 h sample
	s32	v3_sample_error;	// sum of corrections since that sample began
	u8	v3_has_ref;		// first reply only establishes the baseline
	u8	v3_has_drift;	// second sufficiently separated sync establishes rate
	u8	v3_inflight;		// request advertising or response listening
#endif
} scan_wrk_t;

extern scan_wrk_t scan;

//////////////////////////////////////////////////////////
// scan stop
//////////////////////////////////////////////////////////
#if USE_SYNC_SCAN == 3
void scan_stop(void);
void scan_v3_abort(void);
void scan_v3_clock_changed(void);
#else
inline void scan_stop(void) {
	scan.enabled = 0; // stop scan
	scan.start_tik = 0;
}
#endif

void scan_init(void);
void scan_wakeup(void);
void scan_start(void);
void scan_task(void);
#if USE_SYNC_SCAN == 3
void scan_v3_poll(void);
#endif

#endif /* _SCANNING_H_ */
