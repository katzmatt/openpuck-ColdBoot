#include "fault_diag.h"
#include <Arduino.h> // readResetReason(), NRF_POWER, NVIC_SystemReset, POWER_RESETREAS_*_Msk
#include <FreeRTOS.h>
#include <task.h> // uxTaskGetSystemState -> per-task stack high-water (overflow hypothesis check)
#include <string.h>

// GPREGRET2 markers. GPREGRET (id 0) is reserved by the Adafruit bootloader for the DFU magic; GPREGRET2 is
// free for the application and is retained across soft/watchdog/pin reset, cleared only on power-on/brownout.
// Arbitrary non-zero tags -- any value distinct from each other and from the power-on default (0) works.
#define G2_INTENT 0xB1u
#define G2_FAULT 0xFAu

static uint8_t g_reason = RR_UNKNOWN;
static uint32_t g_resetReas = 0;
static uint8_t g_hangStage = 0xFF;

// ---- watchdog pre-reset PC capture ("software SWD") --------------------------------------------------------
// The nRF52 WDT raises a TIMEOUT interrupt ~2 LFCLK cycles BEFORE it resets the chip. We take that interrupt at
// the highest NVIC priority (0) -- above FreeRTOS/TinyUSB BASEPRI critical sections -- read the stacked PC of
// whatever code was wedged, and stash it in a .noinit RAM struct that survives the reset. Next boot reports it
// on the panel; map it with addr2line to name the stuck function. Caveat: this fires only if interrupts aren't
// hard-masked (PRIMASK/CPSID). If a watchdog reset reports NO pc, the hang was a PRIMASK-off / flash-stall type
// (a different, narrower class) -- itself a useful signal. .noinit isn't zeroed by the C runtime, so it rides
// through the watchdog reset (unlike GPREGRET2, which this board's bootloader wipes).
struct HangFrame {
	uint32_t pc, lr, magic;
};
#define HANGPC_MAGIC 0x48414E47u // "HANG"
__attribute__((section(".noinit"))) volatile struct HangFrame g_hangFrame;
static uint32_t g_reportHangPC = 0, g_reportHangLR = 0;

__attribute__((naked)) void WDT_IRQHandler(void)
{
	__asm volatile(
		"tst lr, #4            \n" // EXC_RETURN bit2: 0=frame on MSP, 1=on PSP
		"ite eq                \n"
		"mrseq r0, msp         \n"
		"mrsne r0, psp         \n"
		"ldr r1, [r0, #24]     \n" // stacked PC  (frame[6])
		"ldr r3, [r0, #20]     \n" // stacked LR  (frame[5])
		"ldr r2, =g_hangFrame  \n"
		"str r1, [r2, #0]      \n"
		"str r3, [r2, #4]      \n"
		"ldr r1, =0x48414E47   \n"
		"str r1, [r2, #8]      \n"
		"b .                   \n" // hold until the WDT reset lands (microseconds away)
		".ltorg                \n");
}

// Enable the WDT TIMEOUT interrupt at top priority. Call once, right after NRF_WDT->TASKS_START in setup().
void faultDiagArmHangCapture()
{
	NRF_WDT->INTENSET = WDT_INTENSET_TIMEOUT_Msk;
	NVIC_SetPriority(WDT_IRQn, 0); // above FreeRTOS configMAX_SYSCALL priority -> fires through BASEPRI guards
	NVIC_ClearPendingIRQ(WDT_IRQn);
	NVIC_EnableIRQ(WDT_IRQn);
}
uint32_t faultDiagHangPC()
{
	return g_reportHangPC;
}
uint32_t faultDiagHangLR()
{
	return g_reportHangLR;
}

// Hang breadcrumb: loop() stage written to GPREGRET2 as 0x80|stage. 0x80..0x9F can never collide with the
// intentional-reboot (0xB1) / HardFault (0xFA) markers or the power-on default (0), so the same register
// disambiguates all of them at boot.
#define G2_STAGE_FLAG 0x80u
static const char *const STAGE_STR[] = { "webusb", "ctrl.task", "serial",
					 "rfdiag", "rflink",	"haptic",
					 "led",	   "usbmount",	"usbtx" };
#define STAGE_COUNT ((uint8_t)(sizeof STAGE_STR / sizeof STAGE_STR[0]))

static const char *const REASON_STR[RR_COUNT] = {
	"unknown",   "power-on", "pin/replug", "WATCHDOG (hang)", "CPU lockup",
	"HARDFAULT", "reboot",	 "soft reset", "wake-from-off",
};

// HardFault override. The Adafruit core's default handler (cores/.../debug.cpp) already does NVIC_SystemReset();
// we replace it to first stamp the fault marker so the NEXT boot classifies this SREQ as RR_HARDFAULT rather
// than an intentional reboot. The core's strong symbol is only linked to satisfy the vector table, so our own
// strong definition takes its place. Keep this MINIMAL: we are in fault context on a possibly-corrupt stack --
// no Serial, no allocations, just stamp and reset.
extern "C" void HardFault_Handler(void)
{
	NRF_POWER->GPREGRET2 = G2_FAULT;
	NVIC_SystemReset();
	while (1) {
	}
}

void faultDiagArmIntentionalReset()
{
	NRF_POWER->GPREGRET2 = G2_INTENT;
}

// Live stage + loop heartbeat. The post-reset GPREGRET2 breadcrumb only works if the bootloader preserves
// GPREGRET2 across a watchdog reset -- some clone boards clear it, so it reads blank. These LIVE values don't
// depend on surviving a reset: the WebUSB blob send runs on the USB SOF interrupt (independent of loop()), so
// when loop() wedges, the SOF drain can still report the current stage + how long loop() has been stuck,
// straight to the panel during the ~8s before the watchdog fires.
static volatile uint8_t g_curStage = 0xFF;
static volatile uint32_t g_loopBeatMs = 0;

void faultDiagSetStage(uint8_t stage)
{
	g_curStage = stage;
	NRF_POWER->GPREGRET2 = (uint8_t)(G2_STAGE_FLAG | (stage & 0x1Fu));
}

void faultDiagBeat()
{
	g_loopBeatMs = millis();
}
uint8_t faultDiagCurStage()
{
	return g_curStage;
}
uint32_t faultDiagStallMs()
{
	// ms since loop() last beat. ~0 when healthy (loop beats ~250x/s); grows while loop() is stuck.
	return (uint32_t)(millis() - g_loopBeatMs);
}
const char *faultDiagStageStr(uint8_t s)
{
	return s < STAGE_COUNT ? STAGE_STR[s] : "?";
}

uint8_t faultDiagHangStage()
{
	return g_hangStage;
}
const char *faultDiagHangStageStr()
{
	return g_hangStage < STAGE_COUNT ? STAGE_STR[g_hangStage] : "n/a";
}

void faultDiagBoot()
{
	uint32_t rr =
		readResetReason(); // latched + cleared by the core's init()
	uint8_t g2 = (uint8_t)NRF_POWER->GPREGRET2;
	NRF_POWER->GPREGRET2 = 0; // consume the marker for this boot cycle
	g_resetReas = rr;

	// If this boot follows a watchdog/lockup reset and GPREGRET2 holds a stage breadcrumb (0x80|stage), recover
	// which loop stage was stuck. Only meaningful for hangs -- an intentional reboot/HardFault stamps its own
	// marker over the breadcrumb, and a clean power-on zeroes it.
	bool isHang = (rr & POWER_RESETREAS_DOG_Msk) ||
		      (rr & POWER_RESETREAS_LOCKUP_Msk);
	if (isHang && (g2 & G2_STAGE_FLAG) && g2 < G2_INTENT)
		g_hangStage = (uint8_t)(g2 & 0x1Fu);
	else
		g_hangStage = 0xFF;

	// Recover the PC the WDT pre-reset ISR captured (if it fired -- i.e. the hang wasn't PRIMASK-off). Consume
	// the magic so a later watchdog reset that DIDN'T capture (PRIMASK-off) can't report a stale address.
	if (isHang && g_hangFrame.magic == HANGPC_MAGIC) {
		g_reportHangPC = g_hangFrame.pc;
		g_reportHangLR = g_hangFrame.lr;
	} else {
		g_reportHangPC = 0;
		g_reportHangLR = 0;
	}
	g_hangFrame.magic = 0;

	uint8_t reason;
	// Precedence: a physical pin reset / watchdog / lockup is unambiguous from RESETREAS; only a SREQ needs the
	// GPREGRET2 marker to split intentional reboot vs HardFault.
	if (rr & POWER_RESETREAS_RESETPIN_Msk)
		reason = RR_PIN;
	else if (rr & POWER_RESETREAS_DOG_Msk)
		reason = RR_WATCHDOG;
	else if (rr & POWER_RESETREAS_LOCKUP_Msk)
		reason = RR_LOCKUP;
	else if (rr & POWER_RESETREAS_SREQ_Msk)
		reason = (g2 == G2_FAULT)  ? RR_HARDFAULT :
			 (g2 == G2_INTENT) ? RR_REBOOT :
					     RR_SOFT;
	else if (rr & POWER_RESETREAS_OFF_Msk)
		reason = RR_WAKE;
	else if (rr == 0)
		reason = RR_POWERON; // all bits clear == power-on / brownout
	else
		reason = RR_UNKNOWN;
	g_reason = reason;

	Serial.printf(
		"# reset cause: %s (RESETREAS=0x%08lX gpregret2=0x%02X)\n",
		REASON_STR[reason], (unsigned long)rr, g2);
	if (g_hangStage != 0xFF)
		Serial.printf("# hang stage: %s (%u)\n", faultDiagHangStageStr(),
			      g_hangStage);
	if (g_reportHangPC)
		Serial.printf("# hang PC=0x%08lX LR=0x%08lX\n",
			      (unsigned long)g_reportHangPC,
			      (unsigned long)g_reportHangLR);
}

// ---- clock fingerprint -------------------------------------------------------------------------------------
static uint8_t g_clkLf = 0, g_clkHf = 0;
static uint16_t g_clkUsPerMs = 0;

void clockDiagBoot()
{
	// LFCLKSTAT: bit16 STATE(running), bits1:0 SRC (0=RC,1=Xtal,2=Synth). HFCLKSTAT: bit16 STATE, bit0 SRC
	// (0=RC,1=Xtal). The bare-metal radio needs HFXO; if HF shows RC here, RF runs on the 64 MHz internal RC.
	uint32_t lf = NRF_CLOCK->LFCLKSTAT;
	uint32_t hf = NRF_CLOCK->HFCLKSTAT;
	bool lfrun = lf & CLOCK_LFCLKSTAT_STATE_Msk;
	uint8_t lfsrc = (uint8_t)(lf & CLOCK_LFCLKSTAT_SRC_Msk);
	g_clkLf = lfrun ? (uint8_t)(lfsrc + 1) : 0; // 1=RC,2=Xtal,3=Synth, 0=stopped
	bool hfxtal = hf & CLOCK_HFCLKSTAT_SRC_Msk;
	g_clkHf = hfxtal ? 2 : 0; // 2=crystal, 0=RC
	Serial.printf("# clock: LFCLK=%s HFCLK=%s\n",
		      g_clkLf == 2 ? "xtal" :
		      g_clkLf == 1 ? "RC" :
		      g_clkLf == 3 ? "synth" :
				     "stopped",
		      g_clkHf == 2 ? "xtal" : "RC");
}

void clockDiagTick()
{
	// Cross-check the two time bases: count micros() (HFCLK-derived) elapsed over a ~1s millis() (LFCLK/RTC)
	// window. usPerMs = micros-delta / millis-delta; 1000 = the clocks agree. A clone whose HFCLK runs fast vs
	// its LFCLK reads >1000 here -- the exact signature behind "poll rate too high, delivered too low".
	static uint32_t u0 = 0;
	static unsigned long m0 = 0;
	static bool init = false;
	unsigned long m = millis();
	if (!init) {
		u0 = micros();
		m0 = m;
		init = true;
		return;
	}
	unsigned long dm = m - m0;
	if (dm >= 1000) {
		uint32_t du = (uint32_t)(micros() - u0);
		g_clkUsPerMs = (uint16_t)(du / dm);
		u0 = micros();
		m0 = m;
	}
}

uint8_t clockLfSrc()
{
	return g_clkLf;
}
uint8_t clockHfSrc()
{
	return g_clkHf;
}
uint16_t clockUsPerMs()
{
	return g_clkUsPerMs;
}

// ---- per-task stack headroom (usbd-overflow hypothesis check) ---------------------------------------------
// uxTaskGetSystemState (configUSE_TRACE_FACILITY=1) reports each task's stack high-water mark = the LEAST free
// stack it ever had, in words. The "usbd" task (200 words / 800 B, core-fixed) runs handleSet->relayEnqueue;
// if this trends toward 0 under haptic load, the stack overflow is confirmed. Repo-scoped, read-only, no core
// changes -- just observing FreeRTOS.
static uint16_t g_usbdStackMin = 0xFFFF;
static uint16_t g_loopStackMin = 0xFFFF;
void faultDiagStackTick()
{
	static unsigned long ms = 0;
	if ((uint32_t)(millis() - ms) < 1000)
		return;
	ms = millis();
	static TaskStatus_t st[12];
	UBaseType_t n = uxTaskGetSystemState(st, 12, NULL);
	for (UBaseType_t i = 0; i < n; i++) {
		uint16_t hw = (uint16_t)st[i].usStackHighWaterMark;
		if (!strcmp(st[i].pcTaskName, "usbd") && hw < g_usbdStackMin)
			g_usbdStackMin = hw;
		else if (!strcmp(st[i].pcTaskName, "loop") && hw < g_loopStackMin)
			g_loopStackMin = hw;
	}
}
uint16_t faultDiagUsbdStackFree()
{
	return g_usbdStackMin == 0xFFFF ? 0 : g_usbdStackMin;
}
uint16_t faultDiagLoopStackFree()
{
	return g_loopStackMin == 0xFFFF ? 0 : g_loopStackMin;
}

uint8_t faultDiagReason()
{
	return g_reason;
}
uint32_t faultDiagResetReas()
{
	return g_resetReas;
}
const char *faultDiagReasonStr()
{
	return REASON_STR[g_reason < RR_COUNT ? g_reason : 0];
}
