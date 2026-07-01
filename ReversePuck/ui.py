"""ui.py -- fullscreen touchscreen UI for the Steam Deck forwarder.

Big tappable tiles, one per bonded puck. A tile is tappable when its puck is LIVE (the RF link is up).
Tapping a live tile detaches the Deck controls and forwards to that puck (a single tap -- connecting is
cheap and reversible). The two DESTRUCTIVE actions -- stopping/disconnecting a forward, and removing a
bond (unpair) -- instead require a HOLD (HOLD_SECS): an errant screen tap mid-game used to drop the forward
(which on the puck side tears down a USB device and could reboot the MCU), so a brief touch must no longer
trigger it. A progress fill shows the hold; lifting before it completes cancels. Sized for the Deck's
1280x800 touchscreen but scales.

Status icons (dots, the remove ✕) are drawn with primitives, not font glyphs -- the Deck's available fonts
don't carry ● ○ ✕ etc., so rendering them as text just showed blank/boxes.
"""
import time
import pygame

# Destructive actions (stop forwarding, remove bond) require holding this long; a tap no longer triggers them.
HOLD_SECS = 2.0

BG = (16, 18, 24)
FG = (230, 232, 238)
DIM = (120, 126, 140)
ACCENT = (90, 170, 255)
LIVE = (70, 200, 120)
WARN = (235, 180, 70)
STOP = (235, 90, 90)
TILE = (32, 36, 46)
TILE_LIVE = (28, 54, 40)
TILE_SEL = (40, 70, 110)


def _font(sz, bold=False):
    f = pygame.font.SysFont("noto sans,dejavusans,sans", sz, bold=bold)
    return f


def _draw_dot(screen, center, r, color, filled=True):
    """Status dot drawn with primitives -- font glyphs (● ○) don't render on the Deck's fallback font."""
    if filled:
        pygame.draw.circle(screen, color, center, r)
    else:
        pygame.draw.circle(screen, color, center, r, max(2, r // 3))


def _draw_cross(screen, rect, color, width):
    """Remove-bond ✕ drawn as two diagonals (the font glyph doesn't render)."""
    pad = max(6, rect.width // 4)
    pygame.draw.line(screen, color, (rect.x + pad, rect.y + pad),
                     (rect.right - pad, rect.bottom - pad), width)
    pygame.draw.line(screen, color, (rect.right - pad, rect.y + pad),
                     (rect.x + pad, rect.bottom - pad), width)


def run(app):
    pygame.init()
    pygame.display.set_caption("ReversePuck · Steam Deck")
    screen = pygame.display.set_mode((0, 0), pygame.FULLSCREEN)
    W, H = screen.get_size()
    clock = pygame.time.Clock()
    f_title = _font(int(H * 0.05), bold=True)
    f_big = _font(int(H * 0.045), bold=True)
    f_med = _font(int(H * 0.03))
    f_small = _font(int(H * 0.022))

    tile_rects = []  # (rect, slot, tappable)
    del_rects = []   # (rect, slot) -- the per-tile "remove bond" ✕ target
    # Active hold-to-confirm gesture, or None. dict(kind="stop"|"unpair", slot, start). Read by draw() for
    # the progress fill; set in handle_down()/cleared in handle_up() and on completion.
    hold = None

    def hold_progress():
        """0.0..1.0 of the current hold, or None if no hold is active."""
        if hold is None:
            return None
        return max(0.0, min(1.0, (time.monotonic() - hold["start"]) / HOLD_SECS))

    def draw():
        nonlocal tile_rects, del_rects
        screen.fill(BG)
        s = app.status
        connected = app.dongle_connected
        # compact status line (no big title banner): dongle presence first, then RF link. The leading status
        # dot is drawn (font glyphs don't render); filled = linked, ring = no-dongle / searching.
        if not connected:
            badge, col = "no dongle", WARN
        elif s["link_up"]:
            badge, col = "linked  ch%d" % s["sess_ch"], LIVE
        else:
            badge, col = "searching...", DIM
        t = f_med.render(badge, True, col)
        bx = W - t.get_width() - 40
        screen.blit(t, (bx, 24))
        _draw_dot(screen, (bx - 18, 24 + t.get_height() // 2), 8, col,
                  filled=connected and s["link_up"])
        # legend (top-left): how the gestures work
        screen.blit(f_small.render("Tap to connect, hold to disconnect or remove", True, DIM),
                    (40, 30))

        # tiles -- live pucks float to the top, then by slot index
        tile_rects = []
        del_rects = []
        bonds = sorted(s["bonds"], key=lambda b: (not b["alive"], b["slot"]))
        top = int(H * 0.16)
        gap = int(H * 0.025)
        th = int((H * 0.42 - gap * max(0, len(bonds) - 1)) / max(1, len(bonds)))
        th = min(th, int(H * 0.20))
        if not connected:
            msg = f_big.render("Plug in the ReversePuck dongle", True, WARN)
            screen.blit(msg, (W // 2 - msg.get_width() // 2, H // 2 - 40))
            sub = f_small.render("Waiting for the nRF (Valve 28DE:1302) on USB... it'll appear here automatically.",
                                 True, DIM)
            screen.blit(sub, (W // 2 - sub.get_width() // 2, H // 2 + 20))
        elif not bonds:
            msg = f_big.render("No paired pucks", True, DIM)
            screen.blit(msg, (W // 2 - msg.get_width() // 2, H // 2 - 40))
            sub = f_small.render("Pair on a computer with Steam or PairTUI (scpair.py), then plug the puck into your host.",
                                 True, DIM)
            screen.blit(sub, (W // 2 - sub.get_width() // 2, H // 2 + 20))
        for n, b in enumerate(bonds):
            slot = b["slot"]
            y = top + n * (th + gap)
            rect = pygame.Rect(40, y, W - 80, th)
            sel = app.forwarding and app.fwd_slot == slot
            tappable = b["alive"] and not app.forwarding
            color = TILE_SEL if sel else (TILE_LIVE if b["alive"] else TILE)
            pygame.draw.rect(screen, color, rect, border_radius=18)
            pygame.draw.rect(screen, (ACCENT if (tappable or sel) else DIM), rect, width=2, border_radius=18)
            # status dot (drawn -- font ● ○ don't render) + serial
            gcol = LIVE if b["alive"] else DIM
            _draw_dot(screen, (rect.x + 46, rect.y + th // 2), 13, gcol, filled=b["alive"])
            screen.blit(f_big.render(b["serial"] or "puck %d" % slot, True, FG),
                        (rect.x + 90, rect.y + 20))
            state = ("FORWARDING - hold anywhere to stop" if sel
                     else "available - tap to forward" if tappable
                     else "live (forwarding elsewhere)" if b["alive"]
                     else "paired - offline")
            screen.blit(f_small.render("slot %d - %s" % (slot, state), True, DIM),
                        (rect.x + 90, rect.y + th - 38))
            tile_rects.append((rect, slot, tappable or sel))
            # remove-bond ✕ on the far right (only when idle -- can't un-bond while forwarding)
            if not app.forwarding:
                dsz = min(th - 24, 64)
                drect = pygame.Rect(rect.right - dsz - 18, rect.y + (th - dsz) // 2, dsz, dsz)
                pygame.draw.rect(screen, TILE, drect, border_radius=12)
                # while this ✕ is being held, fill it from the bottom up as a hold-to-unpair progress meter
                held = (hold is not None and hold["kind"] == "unpair"
                        and hold["slot"] == slot)
                hp = hold_progress()
                if held and hp:
                    fh = int(dsz * hp)
                    fill = pygame.Rect(drect.x, drect.bottom - fh, dsz, fh)
                    pygame.draw.rect(screen, STOP, fill, border_radius=12)
                pygame.draw.rect(screen, STOP, drect, width=2, border_radius=12)
                _draw_cross(screen, drect, FG if held else STOP, 4)
                del_rects.append((drect, slot))

        # firmware log panel (the in-app serial monitor) — fills the area below the tiles
        log_top = int(H * 0.60)
        log_bot = H - int(H * (0.12 if app.forwarding else 0.02))
        lh = f_small.get_height() + 2
        nlines = max(1, (log_bot - log_top - 24) // lh)
        screen.blit(f_small.render("firmware log", True, DIM), (40, log_top))
        lines = app.log[-nlines:]
        for i, ln in enumerate(lines):
            # colorize the lines that matter for connecting
            c = DIM
            if "SESSION live" in ln or "CONNECTED" in ln:
                c = LIVE
            elif "adopted" in ln or "E1 heard" in ln:
                c = ACCENT
            elif "lost" in ln or "no bond" in ln or "fail" in ln:
                c = WARN
            screen.blit(f_small.render(ln[:120], True, c), (40, log_top + 24 + i * lh))

        # forwarding banner (drawn last so it sits on top)
        if app.forwarding:
            bar = pygame.Rect(0, H - int(H * 0.1), W, int(H * 0.1))
            pygame.draw.rect(screen, STOP, bar)
            hp = hold_progress()
            if hold is not None and hold["kind"] == "stop" and hp:
                # fill the banner left->right as the stop hold progresses; lighten so the fill reads as a meter
                pygame.draw.rect(screen, (255, 150, 150),
                                 pygame.Rect(bar.x, bar.y, int(bar.width * hp), bar.height))
                left = max(0.0, HOLD_SECS * (1.0 - hp))
                msg = f_big.render("KEEP HOLDING TO STOP - %.0fs" % (left + 0.99),
                                   True, (255, 255, 255))
            else:
                msg = f_big.render("FORWARDING - Deck controls detached - hold to stop",
                                   True, (255, 255, 255))
            screen.blit(msg, (W // 2 - msg.get_width() // 2, bar.y + bar.height // 2 - 22))
        pygame.display.flip()

    def handle_down(pos):
        # Press handler. CONNECT (toggle a live tile) fires immediately -- it's cheap and reversible. The
        # destructive actions (STOP a forward, REMOVE a bond) instead ARM a hold; they only fire from
        # check_hold() once HOLD_SECS elapses, so an errant tap can't trigger them. Actions still only
        # ENQUEUE intents so they never race the IO thread's serial/libusb access.
        nonlocal hold
        if hold is not None:
            return  # a hold is already in progress; ignore re-entrant/duplicate down events
        if app.forwarding:
            # hold anywhere on the screen to stop forwarding
            hold = {"kind": "stop", "slot": None, "start": time.monotonic()}
            return
        # the remove-bond ✕ takes priority over the tile body it sits on -- hold it to unpair
        for drect, slot in del_rects:
            if drect.collidepoint(pos):
                hold = {"kind": "unpair", "slot": slot, "start": time.monotonic()}
                return
        # live tile: connecting/forwarding stays a single tap
        for rect, slot, tappable in tile_rects:
            if rect.collidepoint(pos) and tappable:
                app.request_toggle(slot)
                return

    def handle_up():
        # Lifting (or a stray up) before HOLD_SECS cancels the pending destructive action.
        nonlocal hold
        hold = None

    def check_hold():
        # Fire the held action once it has been sustained long enough, then disarm. Also cancel a hold whose
        # target went away (the forward auto-released, or forwarding started so a bond can't be removed).
        nonlocal hold
        if hold is None:
            return
        if hold["kind"] == "stop" and not app.forwarding:
            hold = None
            return
        if hold["kind"] == "unpair" and app.forwarding:
            hold = None
            return
        if time.monotonic() - hold["start"] >= HOLD_SECS:
            if hold["kind"] == "stop":
                app.request_stop()
            elif hold["kind"] == "unpair":
                app.request_remove(hold["slot"])
            hold = None

    running = True
    while running:
        for ev in pygame.event.get():
            if ev.type == pygame.QUIT:
                running = False
            elif ev.type == pygame.KEYDOWN and ev.key in (pygame.K_ESCAPE, pygame.K_q):
                running = False
            elif ev.type == pygame.MOUSEBUTTONDOWN:
                handle_down(ev.pos)
            elif ev.type == pygame.FINGERDOWN:
                handle_down((int(ev.x * W), int(ev.y * H)))
            elif ev.type in (pygame.MOUSEBUTTONUP, pygame.FINGERUP):
                handle_up()

        check_hold()  # promote a sustained hold into the actual stop/unpair intent

        # Render only -- serial I/O + input forwarding run on app._io_loop (a separate thread) so the
        # vsync'd flip() below can't throttle forwarding to 60Hz. A bad draw must never kill the app.
        try:
            draw()
        except Exception as ex:
            app._flog("UI draw error: %r" % ex)
            app.note = "recovered from error: %s" % ex
        clock.tick(60)  # UI render rate; forwarding is decoupled on the IO thread now

    pygame.quit()
