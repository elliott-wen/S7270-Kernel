#ifndef __HAWAII_PM_H__
#define __HAWAII_PM_H__

#ifndef __ASSEMBLY__
#include <linux/suspend.h>
#endif

#define KONA_MACH_MAX_IDLE_STATE 1

/* Uncomment this to profile time taken by the dormant
 * mode code. Use the following commands to control
 * dormant mode profiling:
 * 1. Enable profiling:
 *    echo d p 1 <ns_gpio> <sec_gpio> <ref_gpio>
 * 2. Disable profiling:
 *    echo d p 0 0 0 0
 * Three GPIOs are used in profiling the time taken by
 * dormant mode code (in both Linux side and ABI-EXT).
 *    a. ref_gpio: This GPIO is turned ON at dormant sequence
 *       start and turned OFF at dormant sequence end. This GPIO
 *       is used to create a reference on the oscilloscope to
 *       correctly determine the start and end of one dormant
 *       entry.
 *    b. ns_gpio: This GPIO is used to time the dormant sequence
 *       in the Linux side (till SMC in entry and from reset handler
 *       to the end in the exit path). Two pulses are seen on ns_gpio
 *       under one pulse of ref_gpio.
 *    c. sec_gpio: Set this value to the GPIO used in ABI-EXT. If ABI-EXT
 *       does not include the GPIO toggle code for timing, then give
 *       a number which does not conflict with either of the above(this
 *       is only a piece of test code and hence does not do any fancy
 *       sanity check!).
 */
/* #define DORMANT_PROFILE */

/* Set this to 0 to disable dormant mode tracing code */
#define DORMANT_TRACE_ENABLE		1
#define DORMANT_MAX_TRACE_ENTRIES	0x1f
#define DORMANT_TRACE_SUCCESS_SHIFT	0
#define DORMANT_TRACE_SERVICE_SHIFT	4

/* Dormant trace values */
#define DORMANT_ENTRY			0x1000
#define DORMANT_ENTRY_REGS_SAVE		0x2000
#define DORMANT_ENTRY_L2_OFF		0x3000
#define DORMANT_ENTRY_SWITCHES_OFF	0x4000
#define DORMANT_ENTRY_LINUX		0x5000
#define DORMANT_EXIT_WAKE_UP		0x9000
#define DORMANT_EXIT_L2_ON		0xA000
#define DORMANT_EXIT_BASIC_RESTORE	0xB000
#define DORMANT_EXIT_SWITCHES_ON	0xC000
#define DORMANT_EXIT_PROC_RESTORE	0xC100
#define DORMANT_EXIT_BEFORE_DISABLE	0xC200
#define DORMANT_EXIT_AFTER_DISABLE	0XC300
#define DORMANT_EXIT_PLLARM_SAVE	0xC400
#define DORMANT_EXIT_POLICY_GO		0xC500
#define DORMANT_EXIT_BEFORE_FREQ	0xC600
#define DORMANT_EXIT_AFTER_FREQ		0xC700
#define DORMANT_EXIT_REGS_RESTORE	0xD000
#define DORMANT_EXIT_CPU1_WAKEUP	0xE000
#define DORMANT_EXIT			0xF000

#define DEEP_SLEEP_LATENCY     8000 /*latency due to xtal warm up delay*/

/* Following macro values should be loadable via a single
 * mov instruction.
 */
#define DORMANT_RESTORE1_START      0xF1
#define DORMANT_RESTORE1_END        0xF2
#define DORMANT_RESTORE2_START      0xF3
#define DORMANT_CTRL_PROG_START     0xE0
#define DORMANT_CTRL_PROG_END       0xE1

#ifndef __ASSEMBLY__

/**
 * C State definitions and latencies
 */
enum {
	CSTATE_SIMPLE_WFI, /* Simple WFI */
	CSTATE_26MHZ_WFI, /* 26MHz WFI */
	CSTATE_SUSPEND_RETN, /* suspend retention */
	CSTATE_SUSPEND_DRMT, /* suspend dormant */
	CSTATE_DS_DRMT, /* deep sleep dormant */
};

/*Cstate Exit latency*/
enum {
	EXIT_LAT_SIMPLE_WFI = 0,
	EXIT_LAT_26MHZ_WFI = 100,
	EXIT_LAT_SUSPEND_RETN = 200,
	/* Worst case dormant sequence delay */
	EXIT_LAT_SUSPEND_DRMT = 2000,
	/*dormant latency + xtal warmup delay*/
	EXIT_LAT_DS_DRMT = DEEP_SLEEP_LATENCY + 2000,
};

/*CState target residency values*/
enum {
	TRGT_RESI_SIMPLE_WFI = 0,
	TRGT_RESI_26MHZ_WFI = 100,
	TRGT_RESI_SUSPEND_RETN = 200,
	TRGT_RESI_SUSPEND_DRMT = EXIT_LAT_SUSPEND_DRMT + 2000,
	TRGT_RESI_DS_DRMT = EXIT_LAT_DS_DRMT + 5000,
};
#endif

#ifndef __ASSEMBLY__
extern u32 dormant_start(void);
extern int dormant_attempt;

/* Set this to 0 to disable retention  mode tracing code */
#define RETENTION_TRACE_ENABLE        1
#define RETENTION_ENTRY               0xACACACAC
#define RETENTION_EXIT                0xABABABAB

/* Set this to 0 to disable wfi mode tracing code */
#define WFI_TRACE_ENABLE        1
#define WFI_ENTRY               0xBABABABA
#define WFI_EXIT                0xBCBCBCBC

enum {
	TRACE_ENTRY,
	TRACE_EXIT
};


#ifdef DORMANT_PROFILE
/* Vars exported by dm_pwr_policy_top.S for dormant profiling */
extern u32 dormant_profile_on;
extern u32 ns_gpio_set_p;
extern u32 ns_gpio_clr_p;
extern u32 ns_gpio_set_v;
extern u32 ns_gpio_clr_v;
extern u32 ns_gpio_bit;

extern void clear_ns_gpio(void);
#endif /* DORMANT_PROFILE */

extern void enter_wfi(void);
extern void dbg_gpio_set(u32 gpio);
extern void dbg_gpio_clr(u32 gpio);
extern int hawaii_force_sleep(suspend_state_t state);

#ifdef CONFIG_EARLYSUSPEND
extern void request_suspend_state(suspend_state_t state);
#else
static inline void request_suspend_state(suspend_state_t state) { }
#endif
extern void instrument_dormant_trace(u32 trace, u32 service, u32 success);
extern void instrument_wfi(int trace_path);
extern void instrument_retention(int trace_path);
extern int get_force_sleep_state(void);
extern void set_spare_power_status(unsigned int mode);
extern u32 num_cpus(void);

#endif /* __ASSEMBLY__ */

#endif /*__HAWAII_PM_H__*/
