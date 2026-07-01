#include "mode_lizard.h"
#include "triton.h"
#include "config.h"
#include "usb_tx.h"
#include "bonds.h" // NSLOT

// Per-controller lizard state. rfLizard runs once per input report on the report's bond slot, so the glide
// velocity / sub-pixel carry / button + keyboard edge trackers must be PER SLOT -- shared statics would let
// one controller's motion clobber another's (garbage deltas). Zero-init (static storage) matches the old
// per-function initial values (all 0 / false).
struct LizardState {
	int prx, pry; // last right-pad sample (mouse)
	bool prt; // right pad was touched last frame
	float vx, vy, rmx, rmy; // mouse velocity + sub-pixel carry
	int ply; // last left-pad Y (scroll)
	bool plt; // left pad was touched last frame
	float sacc; // scroll accumulator
	uint8_t pmbtn; // last mouse-button mask
	bool prevL5, prevR5; // MODE_LIZARD media-key edges (L5/R5)
	uint8_t pmod, pkc[6]; // last keyboard modifier + keycodes (edge-send)
};
static LizardState g_lz[NSLOT];

void rfLizard(int slot, const uint8_t *r, Adafruit_USBD_HID *mdev,
	      Adafruit_USBD_HID *kdev, uint8_t mrid, uint8_t krid)
{
	if (slot < 0 || slot >= NSLOT)
		return;
	LizardState &ls = g_lz[slot];
	uint32_t b = btnsOf(r);
	bool qamHeld = (b & TB_QAM) != 0;
	// --- right pad -> mouse motion with glide (mirrors mode_xinput's rfXboxMouse) ---
	bool rtouch = b & TB_RPADT;
	int rx = s16off(r, 22), ry = s16off(r, 24);
	if (rtouch) {
		if (ls.prt) {
			ls.vx += (rx - ls.prx);
			ls.vy += (ry - ls.pry);
		}
		ls.prx = rx;
		ls.pry = ry;
	}
	ls.prt = rtouch;

	// Y inverted; *10 = desktop-cursor sensitivity (g_mDiv 64 -> eff 640). Lower g_mDiv via WebUI slider = faster.
	float mxf = ls.vx / (float)(g_mDiv * 10) + ls.rmx,
	      myf = -(ls.vy / (float)(g_mDiv * 10)) + ls.rmy;
	int dx = (int)mxf, dy = (int)myf;
	ls.rmx = mxf - dx;
	ls.rmy = myf - dy; // sub-pixel carry
	if (dx > 127)
		dx = 127;
	if (dx < -127)
		dx = -127;
	if (dy > 127)
		dy = 127;
	if (dy < -127)
		dy = -127;
	float f = g_mFric / 100.0f;
	ls.vx *= f;
	ls.vy *= f;
	if (ls.vx > -1 && ls.vx < 1)
		ls.vx = 0;
	if (ls.vy > -1 && ls.vy < 1)
		ls.vy = 0; // friction = glide/decay
	// --- left pad -> vertical scroll wheel (no momentum; only while touching, coarse) ---
	bool ltouch = b & TB_LPADT;
	int ly = s16off(r, 18);
	if (ltouch) {
		if (ls.plt) {
			ls.sacc += (ly - ls.ply) / (float)(g_mDiv * 24);
		}
		ls.ply = ly;
	} else
		ls.sacc = 0;
	ls.plt = ltouch;
	int dw = (int)ls.sacc;
	ls.sacc -= dw;
	if (dw > 15)
		dw = 15;
	if (dw < -15)
		dw = -15; // finger up = wheel up (positive)
	// --- mouse buttons: left=R-pad-click|R-trigger, right=L-trigger, middle=L-pad-click ---
	// trigU8 scales full-range so a full pull reaches ~0xFF; a raw >>8 tops out ~0x80, never crossing threshold.
	uint8_t rtrig = trigU8(u16off(r, 6)), ltrig = trigU8(u16off(r, 4));
	uint8_t mbtn = 0;
	if ((b & TB_RPADC) || rtrig > 180)
		mbtn |= 1; // right trigger -> left (primary) click
	if (ltrig > 180)
		mbtn |= 2; // left trigger  -> right click
	if (b & TB_LPADC)
		mbtn |= 4;
	if (dx || dy || dw || mbtn != ls.pmbtn) {
		ls.pmbtn = mbtn;
		hid_mouse_report_t m;
		m.buttons = mbtn;
		m.x = (int8_t)dx;
		m.y = (int8_t)dy;
		m.wheel = (int8_t)dw;
		m.pan = 0;
		if (mdev->ready())
			usbTxHid(mdev, mrid, &m, sizeof m);
	}
	// --- keyboard: modifiers + up to 6 keycodes ---
	uint8_t mod = 0, kc[6] = { 0, 0, 0, 0, 0, 0 }, nk = 0;
	if (b & TB_LB)
		mod |= KEYBOARD_MODIFIER_LEFTCTRL;
	if (b & TB_RB)
		mod |= KEYBOARD_MODIFIER_LEFTALT;
#define LZK(cond, code)                    \
	do {                               \
		if ((cond) && nk < 6)      \
			kc[nk++] = (code); \
	} while (0)
	LZK(b & TB_A, HID_KEY_ENTER);
	LZK(b & TB_B, HID_KEY_ESCAPE);
	LZK(b & TB_X, HID_KEY_PAGE_UP);
	LZK(b & TB_Y, HID_KEY_PAGE_DOWN);
	LZK(b & TB_VIEW,
	    (g_usbMode == MODE_LIZARD) ? HID_KEY_ESCAPE : HID_KEY_TAB);
	LZK(b & TB_MENU,
	    (g_usbMode == MODE_LIZARD) ? HID_KEY_TAB : HID_KEY_ESCAPE);

	// left stick (XInput sign: +Y = up); deflect ~37% acts as a d-pad
	int sx = s16off(r, 8), sy = s16off(r, 10);
	LZK((b & TB_DUP) || sy > 12000, HID_KEY_ARROW_UP);
	LZK((b & TB_DDN) || sy < -12000, HID_KEY_ARROW_DOWN);
	LZK((b & TB_DLF) || sx < -12000, HID_KEY_ARROW_LEFT);
	LZK((b & TB_DRT) || sx > 12000, HID_KEY_ARROW_RIGHT);
#undef LZK
	if (g_usbMode == MODE_LIZARD) {
		bool mh = (b & TB_STEAM) || qamHeld;
		bool nL5 = mh && (b & TB_L5), nR5 = mh && (b & TB_R5);
		if (nL5 && !ls.prevL5) {
			uint8_t cc = 0x02;
			if (mdev->ready())
				usbTxHid(mdev, 0x03, &cc, 1);
		}
		if (nR5 && !ls.prevR5) {
			uint8_t cc = 0x01;
			if (mdev->ready())
				usbTxHid(mdev, 0x03, &cc, 1);
		}
		if ((!nL5 && ls.prevL5) || (!nR5 && ls.prevR5)) {
			uint8_t cc = 0x00;
			if (mdev->ready())
				usbTxHid(mdev, 0x03, &cc, 1);
		}
		ls.prevL5 = nL5;
		ls.prevR5 = nR5;
		if (mh && (b & TB_X)) {
			mod = KEYBOARD_MODIFIER_LEFTGUI |
			      KEYBOARD_MODIFIER_LEFTCTRL;
			kc[0] = HID_KEY_O;
			kc[1] = 0;
			kc[2] = 0;
			kc[3] = 0;
			kc[4] = 0;
			kc[5] = 0;
			nk = 1;
		}
		if (mh && (b & TB_L4)) {
			mod = KEYBOARD_MODIFIER_LEFTCTRL |
			      KEYBOARD_MODIFIER_LEFTALT;
			kc[0] = HID_KEY_DELETE;
			kc[1] = 0;
			kc[2] = 0;
			kc[3] = 0;
			kc[4] = 0;
			kc[5] = 0;
			nk = 1;
		}
	}
	bool chg = (mod != ls.pmod);
	for (int i = 0; i < 6; i++)
		if (kc[i] != ls.pkc[i])
			chg = true;
	if (chg) {
		ls.pmod = mod;
		for (int i = 0; i < 6; i++)
			ls.pkc[i] = kc[i];
		uint8_t krep[8] = { mod,   0,	  kc[0], kc[1],
				    kc[2], kc[3], kc[4], kc[5] };
		if (kdev->ready())
			usbTxHid(kdev, krid, krep, 8);
	}
}
