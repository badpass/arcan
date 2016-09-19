/*
 * Copyright 2014-2016, Björn Ståhl
 * License: 3-Clause BSD, see COPYING file in arcan source repository.
 * Reference: http://arcan-fe.com
 * Description: text-user interface support library derived from the work on
 * the afsrv_terminal frameserver. Could use a refactor cleanup,
 * different/faster font-rendering/text-support.
 */

/*
 * MISSING: mapping events (test against tui_test.c),
 * - main event loop isn't right
 * - testing
 * - migrate terminal to use lib
 * - switch font management method default
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <locale.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <poll.h>
#include <math.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "../arcan_shmif.h"
#include "../arcan_shmif_tui.h"

#include "libtsm.h"
#include "libtsm_int.h"

#include "util/font_8x8.h"

#define DEFINE_XKB
#include "util/xsymconv.h"

/*
 * For font support, we should have more (especially faster/less complicated)
 * font engines here. Right now, the options are simply TTF_ via freetype or
 * going to an 8x8 built-in ugly thing.
 */
#ifdef TTF_SUPPORT
#include "arcan_ttf.h"
#else
#define TTF_HINTING_NORMAL  3
#define TTF_HINTING_LIGHT  2
#define TTF_HINTING_MONO   1
#define TTF_HINTING_NONE   0
#define TTF_HINTING_RGB 4
#define TTF_HINTING_VRGB 5
#endif

enum dirty_state {
	DIRTY_NONE = 0,
	DIRTY_UPDATED = 1,
	DIRTY_PENDING = 2,
	DIRTY_PENDING_FULL = 4
};

struct tui_context {
/* terminal / state control */
	struct tsm_screen* screen;
	struct tui_cbcfg cfg;
	struct tsm_utf8_mach* ucsconv;

	unsigned flags;
	bool focus, inactive;
	int inact_timer;

/* font rendering / tracking - we support one main that defines cell size
 * and one secondary that can be used for alternative glyphs */
#ifdef TTF_SUPPORT
	TTF_Font* font[2];
/* size in font pt */
#endif
	size_t font_sz;
	int hint;
	int font_fd[2];
	float ppcm;
	enum dirty_state dirty;
	int64_t last;

/* if we receive a label set in mouse events, we switch to a different
 * interpreteation where drag, click, dblclick, wheelup, wheeldown work */
	bool gesture_support;

/* mouse selection management */
	int mouse_x, mouse_y;
	int lm_x, lm_y;
	int bsel_x, bsel_y;
	bool in_select;
	int scrollback;
	bool mouse_forward;
	bool scroll_lock;

/* tracking when to reset scrollback */
	int sbofs;

/* color, cursor and other drawing states */
	int cursor_x, cursor_y;
	int last_dbl_x,last_dbl_y;
	int rows;
	int cols;
	int last_shmask;
	int cell_w, cell_h, pad_w, pad_h;

	uint8_t fgc[3];
	uint8_t bgc[3];
	shmif_pixel ccol, clcol;

/* store a copy of the state where the cursor is */
	struct tsm_screen_attr cattr;
	uint32_t cvalue;
	bool cursor_off;
	enum tui_cursors cursor;

	uint8_t alpha;

/* track last time counter we did update on to avoid overdraw */
	tsm_age_t age;

/* upstream connection */
	struct arcan_shmif_cont acon;
	struct arcan_shmif_cont clip_in;
	struct arcan_shmif_cont clip_out;
	struct arcan_event last_ident;
};

/* additional state synch that needs negotiation and may need to be
 * re-built in the event of a RESET request */
static void queue_requests(struct tui_context* tui,
	bool clipboard, bool clock, bool ident);

/* to be able to update the cursor cell with other information */
static int draw_cbt(struct tui_context* tui,
	uint32_t ch, unsigned x, unsigned y, const struct tsm_screen_attr* attr,
	tsm_age_t age, bool cstate, bool empty
);

static void tsm_log(void* data, const char* file, int line,
	const char* func, const char* subs, unsigned int sev,
	const char* fmt, va_list arg)
{
	fprintf(stderr, "[%d] %s:%d - %s, %s()\n", sev, file, line, subs, func);
	vfprintf(stderr, fmt, arg);
}

const char* curslbl[] = {
	"block",
	"halfblock",
	"frame",
	"vline",
	"uline",
	NULL
};

static void cursor_at(struct tui_context* tui,
	int x, int y, shmif_pixel ccol, bool active)
{
	shmif_pixel* dst = tui->acon.vidp;
	x *= tui->cell_w;
	y *= tui->cell_h;

/* first draw "original character" if it's not occluded */
	if (tui->cursor_off || tui->cursor != CURSOR_BLOCK){
		draw_cbt(tui, tui->cvalue, tui->cursor_x, tui->cursor_y,
			&tui->cattr, 0, false, false);
	}
	if (tui->cursor_off)
		return;

	switch (tui->cursor){
	case CURSOR_BLOCK:
		draw_box(&tui->acon, x, y, tui->cell_w, tui->cell_h, ccol);
	break;
	case CURSOR_HALFBLOCK:
	draw_box(&tui->acon, x, y, tui->cell_w >> 1, tui->cell_h, ccol);
	break;
	case CURSOR_FRAME:
		for (int col = x; col < x + tui->cell_w; col++){
			dst[y * tui->acon.pitch + col] = ccol;
			dst[(y + tui->cell_h-1 ) * tui->acon.pitch + col] = ccol;
		}

		for (int row = y+1; row < y + tui->cell_h-1; row++){
			dst[row * tui->acon.pitch + x] = ccol;
			dst[row * tui->acon.pitch + x + tui->cell_w - 1] = ccol;
		}
	break;
	case CURSOR_VLINE:
		draw_box(&tui->acon, x + 1, y, 1, tui->cell_h, ccol);
	break;
	case CURSOR_ULINE:
		draw_box(&tui->acon, x, y+tui->cell_h-1, tui->cell_w, 1, ccol);
	break;
	case CURSOR_END:
	default:
	break;
	}
}

static void draw_ch_u8(struct tui_context* tui,
	uint8_t u8_ch[5],
	int base_x, int base_y, uint8_t fg[4], uint8_t bg[4],
	bool bold, bool underline, bool italic)
{
	u8_ch[1] = '\0';
	draw_text_bg(&tui->acon, (const char*) u8_ch, base_x, base_y,
		SHMIF_RGBA(fg[0], fg[1], fg[2], fg[3]),
		SHMIF_RGBA(bg[0], bg[1], bg[2], bg[3])
	);
}

#ifdef TTF_SUPPORT
static void draw_ch(struct tui_context* tui,
	uint32_t ch, int base_x, int base_y, uint8_t fg[4], uint8_t bg[4],
	bool bold, bool underline, bool italic)
{
	draw_box(&tui->acon, base_x, base_y, tui->cell_w, tui->cell_h,
		SHMIF_RGBA(bg[0], bg[1], bg[2], bg[3]));

	int prem = TTF_STYLE_NORMAL | (TTF_STYLE_UNDERLINE * underline);
	prem |= TTF_STYLE_ITALIC * italic;
	prem |= (bold ? TTF_STYLE_BOLD : TTF_STYLE_NORMAL);

	unsigned xs = 0, ind = 0;
	int adv = 0;

	TTF_RenderUNICODEglyph(
		&tui->acon.vidp[base_y * tui->acon.pitch + base_x],
		tui->cell_w, tui->cell_h, tui->acon.pitch,
		tui->font, tui->font[1] ? 2 : 1,
		ch, &xs, fg, bg, true, false, prem, &adv, &ind
	);
}
#endif

static void send_cell_sz(struct tui_context* tui)
{
	arcan_event ev = {
		.category = EVENT_EXTERNAL,
		.ext.kind = ARCAN_EVENT(MESSAGE),
	};

	sprintf((char*)ev.ext.message.data, "cell_w:%d:cell_h:%d",
		tui->cell_w, tui->cell_h);
	arcan_shmif_enqueue(&tui->acon, &ev);
}

static int draw_cb(struct tsm_screen* screen, uint32_t id,
	const uint32_t* ch, size_t len, unsigned width, unsigned x, unsigned y,
	const struct tsm_screen_attr* attr, tsm_age_t age, void* data)
{
	struct tui_context* tui = data;
	return
	draw_cbt((struct tui_context*) data, *ch, x, y, attr, age,
		!(tui->flags & TSM_SCREEN_HIDE_CURSOR), len == 0);
}

static int draw_cbt(struct tui_context* tui,
	uint32_t ch, unsigned x, unsigned y, const struct tsm_screen_attr* attr,
	tsm_age_t age, bool cstate, bool empty)
{
	uint8_t fgc[4] = {attr->fr, attr->fg, attr->fb, 255};
	uint8_t bgc[4] = {attr->br, attr->bg, attr->bb, tui->alpha};
	uint8_t* dfg = fgc, (* dbg) = bgc;
	int y1 = y * tui->cell_h;
	int x1 = x * tui->cell_w;

	if (x >= tui->cols || y >= tui->rows)
		return 0;

	if (age && tui->age && age <= tui->age)
		return 0;

	if (attr->inverse){
		dfg = bgc;
		dbg = fgc;
		dbg[3] = tui->alpha;
		dfg[3] = 0xff;
	}

	int x2 = x1 + tui->cell_w;
	int y2 = y1 + tui->cell_h;

/* update dirty rectangle for synchronization */
	if (x1 < tui->acon.dirty.x1)
		tui->acon.dirty.x1 = x1;
	if (x2 > tui->acon.dirty.x2)
		tui->acon.dirty.x2 = x2;
	if (y1 < tui->acon.dirty.y1)
		tui->acon.dirty.y1 = y1;
	if (y2 > tui->acon.dirty.y2)
		tui->acon.dirty.y2 = y2;

	bool match_cursor = false;
	if (x == tui->cursor_x && y == tui->cursor_y){
		if (!(tsm_screen_get_flags(tui->screen) & TSM_SCREEN_HIDE_CURSOR))
			match_cursor = cstate;
	}

	tui->dirty |= DIRTY_UPDATED;

	draw_box(&tui->acon, x1, y1, tui->cell_w, tui->cell_h,
		SHMIF_RGBA(bgc[0], bgc[1], bgc[2], tui->alpha));

/* quick erase if nothing more is needed */
	if (empty){
		if (attr->inverse)
	draw_box(&tui->acon, x1, y1, tui->cell_w, tui->cell_h,
		SHMIF_RGBA(fgc[0], fgc[1], fgc[2], tui->alpha));

	if (!match_cursor)
			return 0;
		else
			ch = 0x00000008;
	}

	if (match_cursor){
		tui->cattr = *attr;
		tui->cvalue = ch;
		cursor_at(tui, x, y, tui->scroll_lock ? tui->clcol : tui->ccol, true);
		return 0;
	}

#ifdef TTF_SUPPORT
	if (!tui->font[0]){
#endif
	size_t u8_sz = tsm_ucs4_get_width(ch) + 1;
	uint8_t u8_ch[u8_sz];
	size_t nch = tsm_ucs4_to_utf8(ch, (char*) u8_ch);
	u8_ch[u8_sz-1] = '\0';
		draw_ch_u8(tui, u8_ch, x1, y1, dfg, dbg,
			attr->bold, attr->underline, attr->italic);
#ifdef TTF_SUPPORT
	}
	else
		draw_ch(tui, ch, x1, y1, dfg, dbg,
			attr->bold, attr->underline, attr->italic);
#endif

	return 0;
}

static void update_screen(struct tui_context* tui)
{
/* don't redraw while we have an update pending or when we
 * are in an invisible state */
	if (tui->inactive)
		return;

/* "always" erase previous cursor, except when terminal screen state explicitly
 * say that cursor drawing should be turned off */
	draw_cbt(tui, tui->cvalue, tui->cursor_x, tui->cursor_y,
		&tui->cattr, 0, false, false);

	tui->cursor_x = tsm_screen_get_cursor_x(tui->screen);
	tui->cursor_y = tsm_screen_get_cursor_y(tui->screen);

	if (tui->dirty & DIRTY_PENDING_FULL){
		tui->acon.dirty.x1 = 0;
		tui->acon.dirty.x2 = tui->acon.w;
		tui->acon.dirty.y1 = 0;
		tui->acon.dirty.y2 = tui->acon.h;
		tsm_screen_selection_reset(tui->screen);

		shmif_pixel col = SHMIF_RGBA(
			tui->bgc[0],tui->bgc[1],tui->bgc[2],tui->alpha);

		if (tui->pad_w)
			draw_box(&tui->acon,
				tui->acon.w-tui->pad_w-1, 0, tui->pad_w+1, tui->acon.h, col);
		if (tui->pad_h)
			draw_box(&tui->acon,
				0, tui->acon.h-tui->pad_h-1, tui->acon.w, tui->pad_h+1, col);
	}

	tui->flags = tsm_screen_get_flags(tui->screen);
	tui->age = tsm_screen_draw(tui->screen, draw_cb, tui);

	draw_cbt(tui, tui->cvalue, tui->cursor_x, tui->cursor_y,
		&tui->cattr, 0, !tui->cursor_off, false);
}

void arcan_tui_invalidate(struct tui_context* tui)
{
	tui->dirty |= DIRTY_PENDING;
}

static void page_up(struct tui_context* tui)
{
	tsm_screen_sb_up(tui->screen, tui->rows);
	tui->sbofs += tui->rows;
	tui->dirty |= DIRTY_PENDING;
}

static void page_down(struct tui_context* tui)
{
	tsm_screen_sb_down(tui->screen, tui->rows);
	tui->sbofs -= tui->rows;
	tui->sbofs = tui->sbofs < 0 ? 0 : tui->sbofs;
	tui->dirty |= DIRTY_PENDING;
}

static void scroll_up(struct tui_context* tui)
{
	tsm_screen_sb_up(tui->screen, 1);
	tui->sbofs += 1;
	tui->dirty |= DIRTY_PENDING;
}

static void scroll_down(struct tui_context* tui)
{
	tsm_screen_sb_down(tui->screen, 1);
	tui->sbofs -= 1;
	tui->sbofs = tui->sbofs < 0 ? 0 : tui->sbofs;
	tui->dirty |= DIRTY_PENDING;
}

static void move_up(struct tui_context* tui)
{
	if (tui->scroll_lock)
		page_up(tui);
/*
  FIXME: input forward
	else if (tsm_vte_handle_keyboard(tui->vte, xkb_key_up, 0, 0, 0))
		tui->dirty |= DIRTY_PENDING;
 */
}

static void move_down(struct tui_context* tui)
{
	if (tui->scroll_lock)
		page_down(tui);

/* FIXME: input forward
	if (tsm_vte_handle_keyboard(tui->vte, xkb_key_down, 0, 0, 0))
		tui->dirty |= DIRTY_PENDING;
 */
}

/* in tsm< typically mapped to ctrl+ arrow but we allow external rebind */
static void move_left(struct tui_context* tui)
{
/*
 * FIXME: input forward
	if (tsm_vte_handle_keyboard(tui->vte, xkb_key_left, 0, 0, 0))
		tui->dirty |= DIRTY_PENDING;
 */
}

static void move_right(struct tui_context* tui)
{
/* FIXME: input forward
	if (tsm_vte_handle_keyboard(tui->vte, xkb_key_right, 0, 0, 0))
		tui->dirty |= DIRTY_PENDING;
 */
}

static void select_begin(struct tui_context* tui)
{
	tsm_screen_selection_start(tui->screen,
		tsm_screen_get_cursor_x(tui->screen),
		tsm_screen_get_cursor_y(tui->screen)
	);
}

#include "util/utf8.c"

static void select_copy(struct tui_context* tui)
{
	char* sel = NULL;
/*
 * there are more advanced clipboard options to be used when
 * we have the option of exposing other devices using a fuse- vfs
 * in: /vdev/istream, /vdev/vin, /vdev/istate
 * out: /vdev/ostream, /dev/vout, /vdev/vstate, /vdev/dsp
 */
	if (!tui->clip_out.vidp)
		return;

/* the selection routine here seems very wonky, assume the complexity comes
 * from char.conv and having to consider scrollback */
	ssize_t len = tsm_screen_selection_copy(tui->screen, &sel);
	if (!sel || len <= 1)
		return;

	len--;
	arcan_event msgev = {
		.ext.kind = ARCAN_EVENT(MESSAGE)
	};

/* empty cells gets marked as NULL, but that would cut the copy short */
	for (size_t i = 0; i < len; i++){
		if (sel[i] == '\0')
			sel[i] = ' ';
	}

	uint32_t state = 0, codepoint = 0;
	char* outs = sel;
	size_t maxlen = sizeof(msgev.ext.message.data) - 1;

/* utf8- point aligned against block size */
	while (len > maxlen){
		size_t i, lastok = 0;
		state = 0;
		for (i = 0; i <= maxlen - 1; i++){
		if (UTF8_ACCEPT == utf8_decode(&state, &codepoint, (uint8_t)(sel[i])))
			lastok = i;

			if (i != lastok){
				if (0 == i)
					return;
			}
		}

		memcpy(msgev.ext.message.data, outs, lastok);
		msgev.ext.message.data[lastok] = '\0';
		len -= lastok;
		outs += lastok;
		if (len)
			msgev.ext.message.multipart = 1;
		else
			msgev.ext.message.multipart = 0;

		arcan_shmif_enqueue(&tui->clip_out, &msgev);
	}

/* flush remaining */
	if (len){
		snprintf((char*)msgev.ext.message.data, maxlen, "%s", outs);
		msgev.ext.message.multipart = 0;
		arcan_shmif_enqueue(&tui->clip_out, &msgev);
	}

	free(sel);
}

static void select_cancel(struct tui_context* tui)
{
	tsm_screen_selection_reset(tui->screen);
}

static void select_at(struct tui_context* tui)
{
	tsm_screen_selection_reset(tui->screen);
	unsigned sx, sy, ex, ey;
	int rv = tsm_screen_get_word(tui->screen,
		tui->mouse_x, tui->mouse_y, &sx, &sy, &ex, &ey);

	if (0 == rv){
		tsm_screen_selection_reset(tui->screen);
		tsm_screen_selection_start(tui->screen, sx, sy);
		tsm_screen_selection_target(tui->screen, ex, ey);
		select_copy(tui);
		tui->dirty |= DIRTY_PENDING;
	}

	tui->in_select = false;
}

static void select_row(struct tui_context* tui)
{
	tsm_screen_selection_reset(tui->screen);
	tsm_screen_selection_start(tui->screen, 0, tui->cursor_y);
	tsm_screen_selection_target(tui->screen, tui->cols-1, tui->cursor_y);
	select_copy(tui);
	tui->dirty |= DIRTY_PENDING;
	tui->in_select = false;
}

struct lent {
	const char* lbl;
	void(*ptr)(struct tui_context*);
};

#ifdef TTF_SUPPORT
static bool setup_font(struct tui_context* tui,
	int fd, size_t font_sz, int mode);

void inc_fontsz(struct tui_context* tui)
{
	tui->font_sz += 2;
	setup_font(tui, badfd, tui->font_sz, 0);
}

void dec_fontsz(struct tui_context* tui)
{
	if (tui->font_sz > 8)
		tui->font_sz -= 2;
	setup_font(tui, badfd, tui->font_sz, 0);
}
#endif

static void scroll_lock(struct tui_context* tui)
{
	tui->scroll_lock = !tui->scroll_lock;
	if (!tui->scroll_lock){
		tui->sbofs = 0;
		tsm_screen_sb_reset(tui->screen);
		tui->dirty |= DIRTY_PENDING;
	}
}

static void mouse_forward(struct tui_context* tui)
{
	tui->mouse_forward = !tui->mouse_forward;
}

static const struct lent labels[] = {
	{"line_up", scroll_up},
	{"line_down", scroll_down},
	{"page_up", page_up},
	{"page_down", page_down},
	{"up", move_up},
	{"down", move_down},
	{"left", move_left},
	{"right", move_right},
	{"mouse_forward", mouse_forward},
	{"select_at", select_at},
	{"select_row", select_row},
	{"scroll_lock", scroll_lock},
	{NULL, NULL}
};

static void expose_labels(struct tui_context* tui)
{
	const struct lent* cur = labels;

	while(cur->lbl){
		arcan_event ev = {
			.category = EVENT_EXTERNAL,
			.ext.kind = ARCAN_EVENT(LABELHINT),
			.ext.labelhint.idatatype = EVENT_IDATATYPE_DIGITAL
		};
		snprintf(ev.ext.labelhint.label,
			sizeof(ev.ext.labelhint.label)/sizeof(ev.ext.labelhint.label[0]),
			"%s", cur->lbl
		);
		cur++;
		arcan_shmif_enqueue(&tui->acon, &ev);
	}
}

static bool consume_label(struct tui_context* tui,
	arcan_ioevent* ioev, const char* label)
{
	const struct lent* cur = labels;

	while(cur->lbl){
		if (strcmp(label, cur->lbl) == 0){
			cur->ptr(tui);
			return true;
		}
		cur++;
	}

	return false;
}

static void ioev_ctxtbl(struct tui_context* tui,
	arcan_ioevent* ioev, const char* label)
{
/* keyboard input */
	int shmask = 0;
	tui->last = 0;

	if (ioev->datatype == EVENT_IDATATYPE_TRANSLATED){
		bool pressed = ioev->input.translated.active;
		if (!pressed)
			return;

		if (tui->in_select){
			tui->in_select = false;
			tsm_screen_selection_reset(tui->screen);
		}
		tui->inact_timer = -4;
		if (label[0] && consume_label(tui, ioev, label))
			return;

		if (tui->sbofs != 0){
			tui->sbofs = 0;
			tsm_screen_sb_reset(tui->screen);
			tui->dirty |= DIRTY_PENDING;
		}

/* ignore the meta keys as we already treat them in modifiers */
		int sym = ioev->input.translated.keysym;
		if (sym >= 300 && sym <= 314)
			return;

/*
 * FIXME: INPUT CALLBACK
 * if utf8- values have been supplied, use them!
		if (ioev->input.translated.utf8[0]){
			size_t len = 0;
			while (len < 5 && ioev->input.translated.utf8[len]) len++;
			shl_pty_write(tui->pty, (char*)ioev->input.translated.utf8, len);
			shl_pty_dispatch(tui->pty);
			return;
		}
*/

/* otherwise try to hack something together,
 * possible that we should maintain an XKB translation table here
 * instead as we have little actual options .. */
		shmask |= ((ioev->input.translated.modifiers &
			(ARKMOD_RSHIFT | ARKMOD_LSHIFT)) > 0) * TSM_SHIFT_MASK;
		shmask |= ((ioev->input.translated.modifiers &
			(ARKMOD_LCTRL | ARKMOD_RCTRL)) > 0) * TSM_CONTROL_MASK;
		shmask |= ((ioev->input.translated.modifiers &
			(ARKMOD_LALT | ARKMOD_RALT)) > 0) * TSM_ALT_MASK;
		shmask |= ((ioev->input.translated.modifiers &
			(ARKMOD_LMETA | ARKMOD_RMETA)) > 0) * TSM_LOGO_MASK;
		shmask |= ((ioev->input.translated.modifiers & ARKMOD_NUM) > 0) * TSM_LOCK_MASK;
		tui->last_shmask = shmask;

		if (sym && sym < sizeof(symtbl_out) / sizeof(symtbl_out[0]))
			sym = symtbl_out[ioev->input.translated.keysym];

/* FIXME: forward input,
 * sym,
 * mask,
 * subid, ...
 */
	}
	else if (ioev->devkind == EVENT_IDEVKIND_MOUSE){
		if (ioev->datatype == EVENT_IDATATYPE_ANALOG){
			if (ioev->subid == 0)
				tui->mouse_x = ioev->input.analog.axisval[0] / tui->cell_w;
			else if (ioev->subid == 1){
				int yv = ioev->input.analog.axisval[0];
				tui->mouse_y = yv / tui->cell_h;

/* FIXME: forward input
				if (tui->mouse_forward){
					tsm_vte_mouse_motion(tui->vte,
						tui->mouse_x, tui->mouse_y, tui->last_shmask);
					return;
				}
 */

				if (!tui->in_select)
					return;

				bool upd = false;
				if (tui->mouse_x != tui->lm_x){
					tui->lm_x = tui->mouse_x;
					upd = true;
				}
				if (tui->mouse_y != tui->lm_y){
					tui->lm_y = tui->mouse_y;
					upd = true;
				}
/* we use the upper / lower regions as triggers for scrollback + selection,
 * with a magnitude based on how far "off" we are */
				if (yv < 0.3 * tui->cell_h)
					tui->scrollback = -1 * (1 + yv / tui->cell_h);
				else if (yv > tui->rows * tui->cell_h + 0.3 * tui->cell_h)
					tui->scrollback = 1 + (yv - tui->rows * tui->cell_h) / tui->cell_h;
				else
					tui->scrollback = 0;

/* in select and drag negative in window or half-size - then use ticker
 * to scroll and an accelerated scrollback */
				if (upd){
					tsm_screen_selection_target(tui->screen, tui->lm_x, tui->lm_y);
					tui->dirty |= DIRTY_PENDING;
				}
/* in select? check if motion tile is different than old, if so,
 * tsm_selection_target */
			}
		}
/* press? press-point tsm_screen_selection_start,
 * release and press-tile ~= release_tile? copy */
		else if (ioev->datatype == EVENT_IDATATYPE_DIGITAL){
/* FIXME: FORWARD INPUT
			if (tui->mouse_forward){
				tsm_vte_mouse_button(tui->vte,
					ioev->subid,ioev->input.digital.active,tui->last_shmask);
				return;
			}
 */

			if (ioev->flags & ARCAN_IOFL_GESTURE){
				if (strcmp(ioev->label, "dblclick") == 0){
/* select row if double doubleclick */
					if (tui->last_dbl_x == tui->mouse_x &&
						tui->last_dbl_y == tui->mouse_y){
						tsm_screen_selection_reset(tui->screen);
						tsm_screen_selection_start(tui->screen, 0, tui->mouse_y);
						tsm_screen_selection_target(
							tui->screen, tui->cols-1, tui->mouse_y);
						select_copy(tui);
						tui->dirty |= DIRTY_PENDING;
						tui->in_select = false;
					}
/* select word */
					else{
						unsigned sx, sy, ex, ey;
						sx = sy = ex = ey = 0;
						int rv = tsm_screen_get_word(tui->screen,
							tui->mouse_x, tui->mouse_y, &sx, &sy, &ex, &ey);
						if (0 == rv){
							tsm_screen_selection_reset(tui->screen);
							tsm_screen_selection_start(tui->screen, sx, sy);
							tsm_screen_selection_target(tui->screen, ex, ey);
							select_copy(tui);
							tui->dirty |= DIRTY_PENDING;
							tui->in_select = false;
						}
					}

					tui->last_dbl_x = tui->mouse_x;
					tui->last_dbl_y = tui->mouse_y;
				}
				else if (strcmp(ioev->label, "click") == 0){
/* forward to terminal? */
				}
				return;
			}
/* scroll or select?
 * NOTE: should also consider a way to specify magnitude */
			if (ioev->subid == MBTN_WHEEL_UP_IND){
				if (ioev->input.digital.active)
					scroll_up(tui);
			}
			else if (ioev->subid == MBTN_WHEEL_DOWN_IND){
				if (ioev->input.digital.active)
					scroll_down(tui);
			}
			else if (ioev->input.digital.active){
				tsm_screen_selection_start(tui->screen, tui->mouse_x, tui->mouse_y);
				tui->bsel_x = tui->mouse_x;
				tui->bsel_y = tui->mouse_y;
				tui->lm_x = tui->mouse_x;
				tui->lm_y = tui->mouse_y;
				tui->in_select = true;
			}
			else{
				if (tui->mouse_x != tui->bsel_x || tui->mouse_y != tui->bsel_y)
					select_copy(tui);

				tsm_screen_selection_reset(tui->screen);
				tui->in_select = false;
				tui->dirty |= DIRTY_PENDING;
			}
		}
	}
}

static void update_screensize(struct tui_context* tui, bool clear)
{
	int cols = tui->acon.w / tui->cell_w;
	int rows = tui->acon.h / tui->cell_h;
	LOG("update screensize (%d * %d), (%d * %d)\n",
		cols, rows, (int)tui->acon.w, (int)tui->acon.h);

	tui->pad_w = tui->acon.w - (cols * tui->cell_w);
	tui->pad_h = tui->acon.h - (rows * tui->cell_h);

	if (cols != tui->cols || rows != tui->rows){
		if (cols > tui->cols)
			tui->pad_w += (cols - tui->cols) * tui->cell_w;

		if (rows > tui->rows)
			tui->pad_h += (rows - tui->rows) * tui->cell_h;

		int dr = tui->rows - rows;
		tui->cols = cols;
		tui->rows = rows;

/*
 * actual resize, we can assume that we are not in signal
 * state as shmif_ will block on that
 *
 * FIXME: CALLBACK, ORDER?
 */
		tsm_screen_resize(tui->screen, cols, rows);
	}

	while (atomic_load(&tui->acon.addr->vready))
		;

	shmif_pixel col = SHMIF_RGBA(
		tui->bgc[0],tui->bgc[1],tui->bgc[2],tui->alpha);

	if (clear)
		draw_box(&tui->acon, 0, 0, tui->acon.w, tui->acon.h, col);

/* will enforce full redraw, and full redraw will also update padding */
	tui->dirty |= DIRTY_PENDING_FULL;
	update_screen(tui);
}


static void targetev(struct tui_context* tui, arcan_tgtevent* ev)
{
	switch (ev->kind){
/* control alpha, palette, cursor mode, ... */
	case TARGET_COMMAND_GRAPHMODE:
		if (ev->ioevs[0].iv == 1){
			tui->alpha = ev->ioevs[1].fv;
			tui->dirty = DIRTY_PENDING_FULL;
		}
	break;

/* sigsuspend to group */
	case TARGET_COMMAND_PAUSE:
	break;

/* sigresume to session */
	case TARGET_COMMAND_UNPAUSE:
	break;

	case TARGET_COMMAND_RESET:
		tui->last_shmask = 0;
		switch(ev->ioevs[0].iv){
		case 0:
		case 1:
 /* normal request, we have no distinction between soft and hard
	* FIXME: FORWARD
			tsm_vte_hard_reset(tui->vte);
	*/
		break;
		case 2:
		case 3:
			queue_requests(tui, true, true, true);
			arcan_shmif_drop(&tui->clip_in);
			arcan_shmif_drop(&tui->clip_out);
		break;
		}
		tui->dirty = DIRTY_PENDING_FULL;
	break;

	case TARGET_COMMAND_BCHUNK_IN:
	case TARGET_COMMAND_BCHUNK_OUT:
/* FIXME: FORWARD
 * map ioev[0].iv to some reachable known path in
 * the terminal namespace, don't forget to dupe as it
 * will be on next event */
	break;

	case TARGET_COMMAND_SEEKCONTENT:
		if (ev->ioevs[0].iv){ /* relative */
			if (ev->ioevs[1].iv < 0)
				tsm_screen_sb_up(tui->screen, -1 * ev->ioevs[1].iv);
			else
				tsm_screen_sb_down(tui->screen, ev->ioevs[1].iv);
			tui->sbofs += ev->ioevs[1].iv;
			tui->dirty |= DIRTY_PENDING;
		}
	break;

	case TARGET_COMMAND_FONTHINT:{
#ifdef TTF_SUPPORT
		int fd = BADFD;
		if (ev->ioevs[1].iv == 1)
			fd = ev->ioevs[0].iv;

		switch(ev->ioevs[3].iv){
		case -1: break;
/* happen to match TTF_HINTING values, though LED layout could be
 * modified through DISPLAYHINT but in practice, not so much. */
		default:
			tui->hint = ev->ioevs[3].iv;
		break;
		}

/* unit conversion again, we get the size in cm, truetype wrapper takes pt,
 * (at 0.03527778 cm/pt), then update_font will take ppcm into account */
		float npx = setup_font(tui, fd, ev->ioevs[2].fv > 0 ?
			ev->ioevs[2].fv / 0.0352778 : 0, ev->ioevs[4].iv);

		update_screensize(tui, false);
#endif
	}
	break;

	case TARGET_COMMAND_DISPLAYHINT:{
/* be conservative in responding to resize,
 * parent should be running crop shader anyhow */
		bool dev =
			(ev->ioevs[0].iv && ev->ioevs[1].iv) &&
			(abs(ev->ioevs[0].iv - tui->acon.addr->w) > 0 ||
			 abs(ev->ioevs[1].iv - tui->acon.addr->h) > 0);

/* visibility change */
		bool update = false;
		if (!(ev->ioevs[2].iv & 128)){
			if (ev->ioevs[2].iv & 2){
				tui->last_shmask = 0;
				tui->inactive = true;
			}
			else if (tui->inactive){
				tui->inactive = false;
				update = true;
			}

	/* selection change */
			if (ev->ioevs[2].iv & 4){
				tui->focus = false;
				if (!tui->cursor_off){
					tui->last_shmask = 0;
					tui->cursor_off = true;
					tui->dirty |= DIRTY_PENDING;
				}
			}
			else{
				tui->focus = true;
				tui->inact_timer = 0;
				if (tui->cursor_off){
					tui->cursor_off = false;
					tui->dirty |= DIRTY_PENDING;
				}
			}
		}

/* switch cursor kind on changes to 4 in ioevs[2] */
		if (dev){
			if (!arcan_shmif_resize(&tui->acon, ev->ioevs[0].iv, ev->ioevs[1].iv))
				LOG("resize to (%d * %d) failed\n", ev->ioevs[0].iv, ev->ioevs[1].iv);
			update_screensize(tui, true);
		}

/* currently ignoring field [3], RGB layout as freetype with
 * subpixel hinting builds isn't default / tested properly here */

#ifdef TTF_SUPPORT
		if (ev->ioevs[4].fv > 0 && fabs(ev->ioevs[4].fv - tui->ppcm) > 0.01){
			float sf = tui->ppcm / ev->ioevs[4].fv;
			tui->ppcm = ev->ioevs[4].fv;
			setup_font(tui, BADFD, 0, 0);
			update = true;
		}
#endif

		if (update)
			tui->dirty = DIRTY_PENDING_FULL;
	}
	break;

/*
 * map the two clipboards needed for both cut and for paste operations
 */
	case TARGET_COMMAND_NEWSEGMENT:
		if (ev->ioevs[1].iv == 1){
			if (!tui->clip_in.vidp){
				tui->clip_in = arcan_shmif_acquire(&tui->acon,
					NULL, SEGID_CLIPBOARD_PASTE, 0);
			}
			else
				LOG("multiple paste- clipboards received, likely appl. error\n");
		}
		else if (ev->ioevs[1].iv == 0){
			if (!tui->clip_out.vidp){
				tui->clip_out = arcan_shmif_acquire(&tui->acon,
					NULL, SEGID_CLIPBOARD, 0);
			}
			else
				LOG("multiple clipboards received, likely appl. error\n");
		}
	break;

/* we use draw_cbt so that dirty region will be updated accordingly */
	case TARGET_COMMAND_STEPFRAME:
		if (ev->ioevs[1].iv == 1 && tui->focus){
			tui->inact_timer++;
			tui->cursor_off = tui->inact_timer > 1 ? !tui->cursor_off : false;
			tui->dirty |= DIRTY_PENDING;
		}
		else{
			if (!tui->cursor_off && tui->focus){
				tui->cursor_off = true;
				tui->dirty |= DIRTY_PENDING;
			}
		}
		if (tui->in_select && tui->scrollback != 0){
			if (tui->scrollback < 0)
				tsm_screen_sb_up(tui->screen, abs(tui->scrollback));
			else
				tsm_screen_sb_down(tui->screen, tui->scrollback);
			tui->dirty |= DIRTY_PENDING;
		}
	break;

/* problem:
 *  1. how to grab and pack shell environment?
 *  2. kill shell, spawn new using unpacked environment */
	case TARGET_COMMAND_STORE:
	case TARGET_COMMAND_RESTORE:
	break;

	case TARGET_COMMAND_EXIT:
		exit(EXIT_SUCCESS);
	break;

	default:
	break;
	}
}

static void event_dispatch(struct tui_context* tui, arcan_event* ev)
{
	switch (ev->category){
	case EVENT_IO:
		ioev_ctxtbl(tui, &(ev->io), ev->io.label);
	break;

	case EVENT_TARGET:
		targetev(tui, &ev->tgt);
	break;

	default:
	break;
	}
}

static bool check_pasteboard(struct tui_context* tui)
{
	arcan_event ev;
	bool rv = false;
	int pv = 0;

	while ((pv = arcan_shmif_poll(&tui->clip_in, &ev)) > 0){
		if (ev.category != EVENT_TARGET)
			continue;

		arcan_tgtevent* tev = &ev.tgt;
		switch(tev->kind){
		case TARGET_COMMAND_MESSAGE:
/* FIXME: FORWARD
			tsm_vte_paste(tui->vte, tev->message, strlen(tev->message));
 */
			rv = true;
		break;
		case TARGET_COMMAND_EXIT:
			arcan_shmif_drop(&tui->clip_in);
			return false;
		break;
		default:
		break;
		}
	}

	if (pv == -1)
		arcan_shmif_drop(&tui->clip_in);

	return rv;
}

static void queue_requests(struct tui_context* tui,
	bool clipboard, bool clock, bool ident)
{
/* immediately request a clipboard for cut operations (none received ==
 * running appl doesn't care about cut'n'paste/drag'n'drop support). */
/* and send a timer that will be used for cursor blinking when active */
	if (clipboard)
	arcan_shmif_enqueue(&tui->acon, &(struct arcan_event){
		.category = EVENT_EXTERNAL,
		.ext.kind = ARCAN_EVENT(SEGREQ),
		.ext.segreq.width = 1,
		.ext.segreq.height = 1,
		.ext.segreq.kind = SEGID_CLIPBOARD,
		.ext.segreq.id = 0xfeedface
	});

/* and a 1s. timer for blinking cursor */
	if (clock)
	arcan_shmif_enqueue(&tui->acon, &(struct arcan_event){
		.category = EVENT_EXTERNAL,
		.ext.kind = ARCAN_EVENT(CLOCKREQ),
		.ext.clock.rate = 12,
		.ext.clock.id = 0xabcdef00,
	});

	if (ident && tui->last_ident.ext.kind != 0)
		arcan_shmif_enqueue(&tui->acon, &tui->last_ident);
}

#ifdef TTF_SUPPORT
static void probe_font(struct tui_context* tui,
	TTF_Font* font, const char* msg, size_t* dw, size_t* dh)
{
	TTF_Color fg = {.r = 0xff, .g = 0xff, .b = 0xff};
	int w = *dw, h = *dh;
	TTF_SizeUTF8(font, msg, &w, &h, TTF_STYLE_BOLD | TTF_STYLE_UNDERLINE);

/* SizeUTF8 does not give the right dimensions for all hinting */
	if (tui->hint == TTF_HINTING_RGB)
		w++;

	if (w > *dw)
		*dw = w;

	if (h > *dh)
		*dh = h;
}

/*
 * modes supported now is 0 (default), 1 (append)
 */
static bool setup_font(int fd, size_t font_sz, int mode)
{
	TTF_Font* font;

	if (font_sz <= 0)
		font_sz = tui->font_sz;

	int modeind = mode >= 1 ? 1 : 0;

/* re-use last descriptor and change size or grab new */
	if (BADFD == fd)
		fd = tui->font_fd[modeind];

	float font_sf = font_sz;
	size_t sf_sz = font_sf + ((font_sf * tui->ppcm / 28.346566f) - font_sf);

	font = TTF_OpenFontFD(fd, sf_sz);
	if (!font)
		return false;

	TTF_SetFontHinting(font, tui->hint);

	size_t w = 0, h = 0;
	static const char* set[] = {
		"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l",
		"m", "n", "o", "p", "q", "r", "s", "t", "u", "v", "x", "y",
		"z", "!", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
		"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L",
		"M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "X", "Y",
		"Z", "|", "_"
	};

	if (mode == 0){
		tui->font_sz = font_sz;
		for (size_t i = 0; i < sizeof(set)/sizeof(set[0]); i++)
			probe_font(tui, font, set[i], &w, &h);

		if (w && h){
			tui->cell_w = w;
			tui->cell_h = h;
		}

		send_cell_sz(tui);
	}

	TTF_SetFontStyle(font, TTF_STYLE_NORMAL);
	TTF_Font* old_font = tui->font[modeind];

	tui->font[modeind] = font;

/* internally, TTF_Open dup:s the descriptor, we only keep it here
 * to allow size changes without specifying a new font */
	if (tui->font_fd[modeind] != fd)
		close(tui->font_fd[modeind]);
	tui->font_fd[modeind] = fd;

	if (old_font){
		TTF_CloseFont(old_font);
		update_screensize(false);
	}

	return true;
}
#endif

/*
 * get temporary access to the current state of the TUI/context,
 * returned pointer is undefined between calls to process/refresh
 */
struct arcan_shmif_cont* arcan_tui_acon(struct tui_context* c)
{
	if (!c)
		return NULL;

	return &c->acon;
}


uint64_t arcan_tui_process(
	struct tui_context** contexts, size_t n_contexts,
	int* fdset, size_t fdset_sz, int timeout, int* errc)
{
	arcan_event ev;
	uint64_t rdy_mask = 0;

	if (fdset_sz + n_contexts == 0){
		*errc = TUI_ERRC_BAD_ARG;
		return 0;
	}

	if ((n_contexts && !contexts) || (fdset_sz && !fdset)){
		*errc = TUI_ERRC_BAD_ARG;
		return 0;
	}

	if (n_contexts > 32 || fdset_sz > 32){
		return 0;
		*errc = TUI_ERRC_BAD_ARG;
	}

/* From each context, we need the relevant tui->acon.epipe to poll on,
 * along with the entries from the fdset that would require us to mask-
 * populate and return. This structure is not entirely cheap to set up
 * so there might be value in caching it somewhere between runs */
	short pollev = POLLIN | POLLERR | POLLNVAL | POLLHUP;
	struct pollfd fds[fdset_sz + (n_contexts * 2)];

/* need to distinguish between types in results and poll doesn't carry tag */
	uint64_t clip_mask = 0;

	size_t ofs = 0;
	for (size_t i = 0; i < n_contexts; i++){
		fds[ofs++] = (struct pollfd){
			.fd = contexts[i]->acon.epipe,
			.events = pollev
		};
		if (contexts[i]->clip_in.vidp){
			fds[ofs++] = (struct pollfd){
				.fd = contexts[i]->clip_in.epipe,
				.events = pollev
			};
			clip_mask |= 1 << ofs;
		}
	}
/* return condition to take responsibility for multiplexing */
	size_t fdset_ofs = ofs;
	for (size_t i = 0; i < fdset_sz; i++){
		fds[ofs++] = (struct pollfd){
			.fd = fdset[i],
			.events = pollev
		};
	}

	int flushc = 0, last_estate = 0;
	int pv = 0;
	int pc = 2;

/* if we've received a clipboard for paste- operations
	if (tui->clip_in.vidp){
		fds[2].fd = tui->clip_in.epipe;
		pc = 3;
	}
 */

/* try to balance latency and responsiveness in the case of saturation
	int sv, tv;
	if (last_estate == -EAGAIN)
		tv = 0;
	else if (atomic_load(&tui->acon.addr->vready) && tui->dirty)
		tv = 4;
	else
		tv = -1;
	sv = poll(fds, pc, tv);
	bool dispatch = last_estate == -EAGAIN;
	if (sv != 0){
		if (fds[1].revents & POLLIN){
			while ((pv = arcan_shmif_poll(&tui->acon, &ev)) > 0){
				event_dispatch(&ev);
			}
			dispatch = true;
			if (-1 == pv)
				return;
		}
 */
/* fail on upstream event
		else if (fds[1].revents)
			break;
		else if (pc == 3 && (fds[2].revents & POLLIN))
			dispatch |= check_pasteboard(tui);

 fail on the terminal descriptor
		if (fds[0].revents & POLLIN)
			dispatch = true;
		else if (fds[0].revents)
			break;
	}
 */

/* need some limiter here so we won't completely stall if the terminal
 * gets spammed (running find / or cat on huge file are good testcases)
	while ( (last_estate = shl_pty_dispatch(tui->pty)) == -EAGAIN &&
		(atomic_load(&tui->acon.addr->vready) || flushc++ < 10))
	;

	flushc = 0;
	if (atomic_load(&tui->acon.addr->vready)){
		*errno = 0;
		return rdy_mask;
	};

	update_screen(tui);
 */

/* we don't synch explicitly, hence the vready check above
	if (tui->dirty & DIRTY_UPDATED){
		tui->dirty = DIRTY_NONE;
		arcan_shmif_signal(&tui->acon, SHMIF_SIGVID | SHMIF_SIGBLK_NONE);
		tui->last = arcan_timemillis();
 set invalid synch region until redraw changes that
		tui->acon.dirty.x1 = tui->acon.w;
		tui->acon.dirty.x2 = 0;
		tui->acon.dirty.y1 = tui->acon.h;
		tui->acon.dirty.y2 = 0;
	}
*/

	*errc = 0;
	return rdy_mask;
}

void arcan_tui_destroy(struct tui_context* tui)
{
	if (!tui)
		return;

	if (tui->clip_in.vidp)
		arcan_shmif_drop(&tui->clip_in);

	if (tui->clip_out.vidp)
		arcan_shmif_drop(&tui->clip_out);

	arcan_shmif_drop(&tui->acon);
	tsm_utf8_mach_free(tui->ucsconv);
	memset(tui, '\0', sizeof(struct tui_context));
	free(tui);
}

struct tui_settings arcan_tui_defaults()
{
	return (struct tui_settings){
		.cell_w = 8,
		.cell_h = 8,
		.alpha = 0xff,
		.bgc = {0x00, 0x00, 0x00},
		.fgc = {0xff, 0xff, 0xff},
		.ccol = SHMIF_RGBA(0x00, 0xaa, 0x00, 0xff),
		.clcol = SHMIF_RGBA(0xaa, 0xaa, 0x00, 0xff),
		.ppcm = ARCAN_SHMPAGE_DEFAULT_PPCM,
		.hint = TTF_HINTING_NONE
	};
}

void arcan_tui_apply_arg(struct tui_settings* cfg, struct arg_arr* arg)
{
/* FIXME: pluck the relevant parsing from _terminal.c */
}

struct tui_context* arcan_tui_setup(struct arcan_shmif_cont* con,
	const struct tui_settings* set, const struct tui_cbcfg* cbs, ...)
{
	const char* val;
#ifdef TTF_SUPPORT
	TTF_Init();
#endif

	struct tui_context* res = malloc(sizeof(struct tui_context));
	memset(res, '\0', sizeof(struct tui_context));

	if (tsm_screen_new(&res->screen, tsm_log, res) < 0){
		free(res);
		return NULL;
	}

	res->focus = true;
	res->font_fd[0] = BADFD;
	res->font_fd[1] = BADFD;
	res->font_sz = set->font_sz;
	res->ccol = set->ccol;
	res->clcol = set->clcol;
	res->alpha = set->alpha;
	res->cell_w = set->cell_w;
	res->cell_h = set->cell_h;
	memcpy(res->bgc, set->bgc, 3);
	memcpy(res->fgc, set->fgc, 3);
	res->hint = set->hint;
	res->ppcm = set->ppcm;

	res->acon = *con;
	res->acon.hints = SHMIF_RHINT_SUBREGION;
	if (0 != tsm_utf8_mach_new(&res->ucsconv)){
		free(res);
		return NULL;
	}

	shmif_pixel bgc = SHMIF_RGBA(
		set->bgc[0], set->bgc[1], set->bgc[2], set->alpha);
	for (size_t i = 0; i < con->w * con->h; i++)
		res->acon.vidp[i] = bgc;
	arcan_shmif_signal(&res->acon, SHMIF_SIGVID | SHMIF_SIGBLK_NONE);

	expose_labels(res);
	tsm_screen_set_max_sb(res->screen, 1000);

	update_screensize(res, false);
/* clipboard, timer callbacks, no IDENT */
	queue_requests(res, true, true, false);

/* show the current cell dimensions to help limit resize requests */
	send_cell_sz(res);

	return res;
}

/*
 * context- unwrap to screen and forward to tsm_screen
 */

void arcan_tui_erase_screen(struct tui_context* c, bool protect)
{
	if (c)
	tsm_screen_erase_screen(c->screen, protect);
}

void arcan_tui_erase_region(struct tui_context* c,
	size_t x1, size_t y1, size_t x2, size_t y2, bool protect)
{
	if (c)
	tsm_screen_erase_region(c->screen, x1, y1, x2, y2, protect);
}

void arcan_tui_refinc(struct tui_context* c)
{
	if (c)
	tsm_screen_ref(c->screen);
}

void arcan_tui_refdec(struct tui_context* c)
{
	if (c)
	tsm_screen_unref(c->screen);
}

void arcan_tui_defattr(struct tui_context* c, struct tui_screen_attr* attr)
{
	if (c)
	tsm_screen_set_def_attr(c->screen, (struct tsm_screen_attr*) attr);
}

void arcan_tui_write(struct tui_context* c, uint32_t ucode,
	struct tui_screen_attr* attr)
{
	if (c)
	tsm_screen_write(c->screen, ucode, (struct tsm_screen_attr*) attr);
}

bool arcan_tui_writeu8(struct tui_context* c,
	uint8_t* u8, size_t len, struct tui_screen_attr* attr)
{
	if (!(c && u8 && len > 0))
		return false;

	for (size_t i = 0; i < len; i++){
		int state = tsm_utf8_mach_feed(c->ucsconv, u8[i]);
		if (state == TSM_UTF8_ACCEPT || state == TSM_UTF8_REJECT){
			uint32_t ucs4 = tsm_utf8_mach_get(c->ucsconv);
			arcan_tui_write(c, ucs4, attr);
		}
	}
	return true;
}

void arcan_tui_cursorpos(struct tui_context* c, size_t* x, size_t* y)
{
	if (!(c && x && y))
		return;

	*x = tsm_screen_get_cursor_x(c->screen);
	*x = tsm_screen_get_cursor_y(c->screen);
}

void arcan_tui_reset(struct tui_context* c)
{
	tsm_utf8_mach_reset(c->ucsconv);
	tsm_screen_reset(c->screen);
}

void arcan_tui_set_flags(struct tui_context* c, enum tui_flags flags)
{
	if (c)
	tsm_screen_set_flags(c->screen, flags);
}

void arcan_tui_reset_flags(struct tui_context* c, enum tui_flags flags)
{
	if (c)
	tsm_screen_reset_flags(c->screen, flags);
}

void arcan_tui_set_tabstop(struct tui_context* c)
{
	if (c)
	tsm_screen_set_tabstop(c->screen);
}

void arcan_tui_insert_lines(struct tui_context* c, size_t n)
{
	if (c)
	tsm_screen_insert_lines(c->screen, n);
}

void arcan_tui_delete_lines(struct tui_context* c, size_t n)
{
	if (c)
	tsm_screen_delete_lines(c->screen, n);
}

void arcan_tui_insert_chars(struct tui_context* c, size_t n)
{
	if (c)
	tsm_screen_insert_chars(c->screen, n);
}

void arcan_tui_delete_chars(struct tui_context* c, size_t n)
{
	if (c)
	tsm_screen_delete_chars(c->screen, n);
}

void arcan_tui_tab_right(struct tui_context* c, size_t n)
{
	if (c)
	tsm_screen_tab_right(c->screen, n);
}

void arcan_tui_tab_left(struct tui_context* c, size_t n)
{
	if (c)
	tsm_screen_tab_left(c->screen, n);
}

void arcan_tui_scroll_up(struct tui_context* c, size_t n)
{
	if (c)
	tsm_screen_scroll_up(c->screen, n);
}

void arcan_tui_scroll_down(struct tui_context* c, size_t n)
{
	if (c)
	tsm_screen_scroll_down(c->screen, n);
}

void arcan_tui_reset_tabstop(struct tui_context* c)
{
	if (c)
	tsm_screen_reset_tabstop(c->screen);
}


void arcan_tui_reset_all_tabstops(struct tui_context* c)
{
	if (c)
	tsm_screen_reset_all_tabstops(c->screen);
}

void arcan_tui_move_to(struct tui_context* c, size_t x, size_t y)
{
	if (c)
	tsm_screen_move_to(c->screen, x, y);
}

void arcan_tui_move_up(struct tui_context* c, size_t n, bool scroll)
{
	if (c)
	tsm_screen_move_up(c->screen, n, scroll);
}

void arcan_tui_move_down(struct tui_context* c, size_t n, bool scroll)
{
	if (c)
	tsm_screen_move_down(c->screen, n, scroll);
}

void arcan_tui_move_left(struct tui_context* c, size_t n)
{
	if (c)
	tsm_screen_move_left(c->screen, n);
}

void arcan_tui_move_right(struct tui_context* c, size_t n)
{
	if (c)
	tsm_screen_move_right(c->screen, n);
}

void arcan_tui_move_line_end(struct tui_context* c)
{
	if (c)
	tsm_screen_move_line_end(c->screen);
}

void arcan_tui_move_line_home(struct tui_context* c)
{
	if (c)
	tsm_screen_move_line_home(c->screen);
}

void arcan_tui_newline(struct tui_context* c)
{
	if (c)
	tsm_screen_newline(c->screen);
}

int arcan_tui_set_margins(struct tui_context* c, size_t top, size_t bottom)
{
	if (c)
	return tsm_screen_set_margins(c->screen, top, bottom);
	return -EINVAL;
}
