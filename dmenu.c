/* See LICENE file for copyright and license details. */
#include <ctype.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>


#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#ifdef XINERAMA
#include <X11/extensions/Xinerama.h>
#endif
#include <X11/Xft/Xft.h>

#include "drw.h"
#include "util.h"

/* macros */
#define INTERSECT(x,y,w,h,r)  (MAX(0, MIN((x)+(w),(r).x_org+(r).width)  - MAX((x),(r).x_org)) \
                             * MAX(0, MIN((y)+(h),(r).y_org+(r).height) - MAX((y),(r).y_org)))
#define LENGTH(X)             (sizeof X / sizeof X[0])
#define TEXTW(X)              (drw_fontset_getwidth(drw, (X)) + lrpad)
/* enums */
enum { SchemeNorm, SchemeSel, SchemeOut, SchemeSelOut, SchemeNoBg, SchemeLast }; /* color schemes */

struct item {
	char *text;
	struct item *left, *right;
	struct item *m_left, *m_right; //add linkers to marked list inside main item struct itself
	uint8_t marked;
	uint8_t fold;
	int index;
};

static char text[BUFSIZ] = "";
static int flip_slash = 0, flip_d = 0, flip_g = 0, flip_c = 0, flip_p = 0;
static int resize_x, resize_y;
static int debugging = 0;
static char *embed;
static int bh, mw, mh;
static int inputw = 0, promptw;
static int lrpad; /* sum of left and right padding */
static size_t cursor;
static struct item *m_list = NULL, *m_end = NULL;
static struct item *items = NULL;
static struct item *matches, *matchend;
static struct item *prev, *curr, *next, *sel;
static int mon = -1, screen;

static Atom clip, utf8;
static Display *dpy;
static Window root, parentwin, win;
static XIC xic;

static Drw *drw;
static Clr *scheme[SchemeLast];

#include "config.h"

static int (*fstrncmp)(const char *, const char *, size_t) = strncmp;
static char *(*fstrstr)(const char *, const char *) = strstr;

static unsigned int
textw_clamp(const char *str, unsigned int n)
{
	unsigned int w = drw_fontset_getwidth_clamp(drw, str, n) + lrpad;
	return MIN(w, n);
}

static void
appenditem(struct item *item, struct item **list, struct item **last)
{
	if (*last)
		(*last)->right = item;
	else
		*list = item;

	item->left = *last;
	item->right = NULL;
	*last = item;
}

//TODO: got a lot of work

static void
weaver(struct item *pre, struct item *mid, struct item *nex) {

	mid->marked = 1;
	mid->m_left = pre;
	mid->m_right = nex;
	if (nex)
		nex->m_left = mid;
	if (pre)
		pre->m_right = mid;

}

static void
de_weaver(struct item *umark, struct item *ltoumark, struct item *midumark, int all) {

	struct item *f_break, *cur, *nex, *l_break;

	if (!umark && !ltoumark && !(f_break = l_break = NULL)) /* because compiler was screaming */
		fprintf(stderr, "lol, we haven't finished this...\n");
	else if (!umark) {

		/* we are de-weaveing backwards, that means in the direction of m_list
		 * from m_end. But we are keeping f(irst)_break as last break in this
		 * loop, and l(ast)_break as the first, so that when we read from m_list
		 * to m_end, f_break always comes before l_break. So we can glue the two 
		 * break points using the same code at end of this function.
		 * */

		f_break = ltoumark;
		l_break = ltoumark->m_right;
		for (cur = ltoumark; cur && cur->marked; cur = nex) {
			nex = cur->left;
			f_break = cur->m_left;
			cur->marked = 0;
			cur->m_left = cur->m_right = NULL;
		}
	} else if (!ltoumark) {
		l_break = umark;
		f_break = umark->m_left;
		for (cur = umark; cur && cur->marked; cur = nex) {
			nex = cur->right;
			l_break = cur->m_right;
			cur->marked = 0;
			cur->m_left = cur->m_right = NULL;
		}
	} else if (umark == ltoumark) {
		f_break = umark->m_left;
		l_break = umark->m_right;
		umark->marked = 0;
		umark->m_left = umark->m_right = NULL;
	} else
		return;

	if (f_break)
		if (l_break)
			weaver(f_break, l_break, l_break->m_right);
		else {
			m_end = f_break;
			f_break->m_right = NULL;
		}
	else
		if (l_break) {
			m_list = l_break;
			l_break->m_left = NULL;
		} else
			m_list = m_end = NULL;
}


static void
weave_back(struct item **marked_item, struct item **nex_to_mark) {

	/* we are weaving backwards, so m_pre will be m_right to m_item
	 * and m_nex will be m_left.
	 * */

	struct item *m_nex, *m_pre;
	struct item *m_item, *cur;

	m_item = *marked_item;
	cur = *nex_to_mark;

	m_nex = m_item->m_left;
	m_pre = m_item->m_right;

	if (cur == m_item) {
		cur = cur->left;
		if (m_item->m_left)
			*marked_item = m_item->m_left;

	} else if (cur->index > m_item->index && !m_pre) {
		weaver(m_item, cur, NULL);
		m_end = cur;
		*nex_to_mark = cur->left;

	} else if (cur->index > m_item->index && m_pre->index > cur->index) {
		weaver(m_item, cur, m_pre);
		*nex_to_mark = cur->left;

	} else if (m_item->index > cur->index && !m_nex) {
		weaver(NULL, cur, m_item);
		m_list = cur;
		*nex_to_mark = cur->left;
		*marked_item = m_item->m_left;

	} else if (m_item->index > cur->index && cur->index > m_nex->index) {
		weaver(m_nex, cur, m_item);
		*nex_to_mark = cur->left;
		*marked_item = m_item->m_left;

	} else
		*marked_item = m_item->m_left;

}

static void
weave_front(struct item **marked_item, struct item **nex_to_mark) {

	struct item *m_nex, *m_pre;
	struct item *m_item, *cur;


	m_item = *marked_item;
	cur = *nex_to_mark;

	m_nex = m_item->m_right;
	m_pre = m_item->m_left;

	if (cur == m_item) {
		cur = cur->right;
		if (m_item->m_right)
			*marked_item = m_item->m_right;

	} else if (cur->index < m_item->index && !m_pre) {
		weaver(NULL, cur, m_item);
		m_list = cur;
		*nex_to_mark = cur->right;

	} else if (cur->index < m_item->index && m_pre->index < cur->index) {
		weaver(m_pre, cur, m_item);
		*nex_to_mark = cur->right;

	} else if (m_item->index < cur->index && !m_nex) {
		weaver(m_item, cur, NULL);
		m_end = cur;
		*nex_to_mark = cur->right;
		*marked_item = m_item->m_right;

	} else if (m_item->index < cur->index && cur->index < m_nex->index) {
		weaver(m_item, cur, m_nex);
		*nex_to_mark = cur->right;
		*marked_item = m_item->m_right;

	} else
		*marked_item = m_item->m_right;

}

static void
weave_marked(struct item *mark, struct item *ltomark, struct item *midmark, int all) {

	struct item *m_item, *cur;

	if (!m_list) {
		if (mark) {
			m_list = m_end = mark;
			mark->marked = 1;
			if (mark != ltomark)
				mark = (all) ? mark + 1 : mark->right;
			else
				return;
		} else if (ltomark) {
			m_list = m_end = ltomark;
			ltomark->marked = 1;
			if (mark != ltomark)
				ltomark = (all) ? mark - 1 : ltomark->left;
			else
				return;
		} else if (midmark) {
			m_list = m_end = midmark;
			midmark->marked = 1;
		}
	}

	if (!mark && !ltomark) {
		if (midmark && midmark->marked) {
			if (midmark->left)
				for (m_item = midmark, cur = midmark->left; cur && !cur->marked;)
					weave_back(&m_item, &cur);
			if (midmark->right)
				for (m_item = midmark, cur = midmark->right; cur && !cur->marked;)
					weave_front(&m_item, &cur);
		} else {
			for (m_item = m_end, cur = midmark; cur && !cur->marked;)
				weave_back(&m_item, &cur);
			if (midmark->right)
				for (m_item = midmark, cur = midmark->right; cur && !cur->marked;)
					weave_front(&m_item, &cur);
		}
	} else if (!mark) {
		for (m_item = m_end, cur = ltomark; cur && !cur->marked;)
			weave_back(&m_item, &cur);
	} else if (!ltomark) {
		for (m_item = m_list, cur = mark; cur && !cur->marked;)
			weave_front(&m_item, &cur);
	} else if (mark == ltomark)
		for (m_item = m_list, cur = mark; cur && cur->left != ltomark;)
			weave_front(&m_item, &cur);

}

static void
ch_border_col(int flip) {
	if (flip)
		XSetWindowBorder(dpy, win, scheme[SchemeSel][ColBg].pixel);
	else
		XSetWindowBorder(dpy, win, scheme[SchemeOut][ColBg].pixel);
}

static void
calcoffsets(void)
{
	int i, n;

	if (lines > 0)
		n = lines * columns * bh;
	else
		n = mw - (promptw + inputw + TEXTW("<") + TEXTW(">"));
	/* calculate which items will begin the next page and previous page */
	for (i = 0, next = curr; next; next = next->right)
		if ((i += (lines > 0) ? bh : textw_clamp(next->text, n)) > n)
			break;
	for (i = 0, prev = curr; prev && prev->left; prev = prev->left)
		if ((i += (lines > 0) ? bh : textw_clamp(prev->left->text, n)) > n)
			break;
}

static int
max_textw(void)
{
	int len = 0;
	for (struct item *item = items; item && item->text; item++)
		len = MAX(TEXTW(item->text), len);
	return len;
}

static void
cleanup(void)
{
	size_t i;

	XUngrabKey(dpy, AnyKey, AnyModifier, root);
	for (i = 0; i < SchemeLast; i++)
		free(scheme[i]);
	for (i = 0; items && items[i].text; ++i)
		free(items[i].text);
	free(items);
	drw_free(drw);
	XSync(dpy, False);
	XCloseDisplay(dpy);
}

static char *
cistrstr(const char *h, const char *n)
{
	size_t i;

	if (!n[0])
		return (char *)h;

	for (; *h; ++h) {
		for (i = 0; n[i] && tolower((unsigned char)n[i]) ==
		            tolower((unsigned char)h[i]); ++i)
			;
		if (n[i] == '\0')
			return (char *)h;
	}
	return NULL;
}

static int
drawitem(struct item *item, int x, int y, int w)
{
	if (item == sel)
		if (item->marked && !use_dots)
			drw_setscheme(drw, scheme[SchemeSelOut]);
		else
			drw_setscheme(drw, scheme[SchemeSel]);
	else if (item->marked)
		drw_setscheme(drw, scheme[SchemeOut]);
	else
		drw_setscheme(drw, scheme[SchemeNorm]);

	return drw_text(drw, x, y, w, bh, lrpad / 2, item->text, 0);
}

static void
drawmenu(void)
{
	unsigned int curpos;
	struct item *item;
	int x = 0, y = 0, fh = drw->fonts->h, w;

	drw_setscheme(drw, scheme[SchemeNorm]);
	drw_rect(drw, 0, 0, mw, mh, 1, 1);

	if (prompt && *prompt) {
		drw_setscheme(drw, scheme[SchemeSel]);
		x = drw_text(drw, x, 0, promptw, bh, lrpad / 2, prompt, 0);
	}
	/* draw input field */
	w = (lines > 0 || !matches) ? mw - x : inputw;

	if (vi_mode && !ins_mode) {
		y -= bh;
		curpos = 0;
		if ((curpos += lrpad / 2 - 1) < w && !matches) {
			drw_setscheme(drw, scheme[SchemeNorm]);
			drw_text(drw, x, 2 + (bh - fh) / 2, w, bh, lrpad / 2, "We've got nothing, Jesse...", 0);
		}
	} else {
		drw_setscheme(drw, scheme[SchemeNorm]);
		drw_text(drw, x, 0, w, bh, lrpad / 2, text, 0);
		curpos = TEXTW(text) - TEXTW(&text[cursor]);
		if ((curpos += lrpad / 2 - 1) < w) {
			drw_setscheme(drw, scheme[SchemeNorm]);
			drw_rect(drw, x + curpos, 2 + (bh - fh) / 2, 2, fh - 4, 1, 0);
		}
		if (!matches) {
			drw_setscheme(drw, scheme[SchemeNorm]);
			drw_text(drw, x, (2 + (bh - fh) / 2) + bh, w, bh, lrpad / 2, "We've got nothing, Jesse...", 0);
		}

	}

	if (lines > 0) { //TODO: Fix this, use floatttt!!
		/* draw grid */
		Fnt *pre_font;
		int i = 0;
		//int cent = 0;
		//int flip = 0;
		//int col_num = 0;
		//int jfillit = 0; /* giving higher column value (like ~20) will make this */
		float xcord = 0; /* worse 'cause we're just filling the last one */
		float col_width = mw / columns;
		for (item = curr; item != next; item = item->right, i++) {
				
			/*if (i % lines == 0) { //TODO:now we are using a float instead of this hack 
				col_num += 1;       // But we need to test this
				if (flip == 0) 
					jfillit += ftwidth;
			}
			if (col_num == columns) { 
				ftwidth = mw - jfillit;
				flip = 1;
			} else {
				ftwidth = (mw - x) / columns;
			}*/

			if (!(i % lines)) {
				xcord += col_width;
			}

			drawitem(item,
					//x + ((i / lines) * ((mw - x) / columns)) - promptw,
					x + xcord,
					y + (((i % lines) + 1) * bh),
					col_width);

			//drw_setscheme(drw, scheme[SchemeNoBg]); //TODO:
			if (use_dots) { // needs more shaping
				pre_font = drw->fonts;
				drw->fonts = drw->fonts->next;
				drw_text(drw,
						x + ((i / lines) * ((mw - x) / columns)) - promptw,
						y + (((i % lines) + 1) * bh) + 3 * (bh / 4) + border_width,
						//ftwidth , bh / 4, (ftwidth / 2) - (drw_fontset_getwidth(drw, "◉") / 2), "◉", 0);
					ftwidth , bh / 4, (ftwidth / 2) - (drw_fontset_getwidth(drw, dot_char) / 2), dot_char, 0);
				drw->fonts = pre_font;
			}

		}
	} else if (matches) {
		/* draw horizontal list */
		x += inputw;
		w = TEXTW("<");
		if (curr->left) {
			drw_setscheme(drw, scheme[SchemeNorm]);
			drw_text(drw, x, 0, w, bh, lrpad / 2, "<", 0);
		}
		x += w;
		for (item = curr; item != next; item = item->right)
			x = drawitem(item, x, 0, textw_clamp(item->text, mw - x - TEXTW(">")));
		if (next) {
			w = TEXTW(">");
			drw_setscheme(drw, scheme[SchemeNorm]);
			drw_text(drw, mw - w, 0, w, bh, lrpad / 2, ">", 0);
		}
	}
	drw_map(drw, win, 0, 0, mw, mh);
}

static void
resizemenu(int increase) /* 1 for increase and 0 for decrase height <- Now lisen, this is stupid */ 
{
	if (increase) {
		mh += bh;
		if ((!topbar && !centered) || att_edge == 2)
			resize_y -= bh;
		else if (centered || att_edge > 2)
			resize_y -= bh / 2;
	} else {
		mh -= bh;
		if ((!topbar && !centered) || att_edge == 2)
			resize_y += bh;
		else if (centered || att_edge > 2)
			resize_y += bh / 2;
	}
	XMoveResizeWindow(dpy, win, resize_x, resize_y, mw, mh);
	drw_resize(drw, mw, mh);
}

static void
grabfocus(void)
{
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 10000000  };
	Window focuswin;
	int i, revertwin;

	for (i = 0; i < 100; ++i) {
		XGetInputFocus(dpy, &focuswin, &revertwin);
		if (focuswin == win)
			return;
		XSetInputFocus(dpy, win, RevertToParent, CurrentTime);
		nanosleep(&ts, NULL);
	}
	die("cannot grab focus");
}

static void
grabkeyboard(void)
{
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000  };
	int i;

	if (embed)
		return;
	/* try to grab keyboard, we may have to wait for another process to ungrab */
	for (i = 0; i < 1000; i++) {
		if (XGrabKeyboard(dpy, DefaultRootWindow(dpy), True, GrabModeAsync,
		                  GrabModeAsync, CurrentTime) == GrabSuccess)
			return;
		nanosleep(&ts, NULL);
	}
	die("cannot grab keyboard");
}

static void
match(void)
{
	static char **tokv = NULL;
	static int tokn = 0;

	char buf[sizeof text], *s;
	int i, tokc = 0;
	size_t len, textsize;
	struct item *item, *lprefix, *lsubstr, *prefixend, *substrend;

	strcpy(buf, text);
	/* separate input text into tokens to be matched individually */
	for (s = strtok(buf, " "); s; tokv[tokc - 1] = s, s = strtok(NULL, " "))
		if (++tokc > tokn && !(tokv = realloc(tokv, ++tokn * sizeof *tokv)))
			die("cannot realloc %zu bytes:", tokn * sizeof *tokv);
	len = tokc ? strlen(tokv[0]) : 0;

	matches = lprefix = lsubstr = matchend = prefixend = substrend = NULL;
	textsize = strlen(text) + 1;
	for (item = items; item && item->text; item++) {

		if (pre_mark && !strcmp(strtok(item->text, "	"), "---")) {
			strcpy(item->text, strtok(NULL, "	"));
			if (m_list && m_end) {
				weaver(m_end, item, NULL);
				m_end = item;
			} else
				m_list = m_end = item;
			item->marked = 1;
		}

		for (i = 0; i < tokc; i++)
			if (!fstrstr(item->text, tokv[i]))
				break;
		if (i != tokc) /* not all tokens match */
			continue;
		/* exact matches go first, then prefixes, then substrings */
		if (!tokc || !fstrncmp(text, item->text, textsize))
			appenditem(item, &matches, &matchend);
		else if (!fstrncmp(tokv[0], item->text, len))
			appenditem(item, &lprefix, &prefixend);
		else
			appenditem(item, &lsubstr, &substrend);
	}
	pre_mark = 0; /* Disabling it because we want mark only once, at the first call of this function */
	if (lprefix) {
		if (matches) {
			matchend->right = lprefix;
			lprefix->left = matchend;
		} else
			matches = lprefix;
		matchend = prefixend;
	}
	if (lsubstr) {
		if (matches) {
			matchend->right = lsubstr;
			lsubstr->left = matchend;
		} else
			matches = lsubstr;
		matchend = substrend;
	}
	curr = sel = matches;
	calcoffsets();
}

static void
insert(const char *str, ssize_t n)
{
	if (strlen(text) + n > sizeof text - 1)
		return;
	/* move existing text out of the way, insert new text, and update cursor */
	memmove(&text[cursor + n], &text[cursor], sizeof text - cursor - MAX(n, 0));
	if (n > 0)
		memcpy(&text[cursor], str, n);
	cursor += n;
	match();
}

static size_t
nextrune(int inc)
{
	ssize_t n;

	/* return location of next utf8 rune in the given direction (+1 or -1) */
	for (n = cursor + inc; n + inc >= 0 && (text[n] & 0xc0) == 0x80; n += inc)
		;
	return n;
}

static void
movewordedge(int dir)
{
	if (dir < 0) { /* move cursor to the start of the word*/
		while (cursor > 0 && strchr(worddelimiters, text[nextrune(-1)]))
			cursor = nextrune(-1);
		while (cursor > 0 && !strchr(worddelimiters, text[nextrune(-1)]))
			cursor = nextrune(-1);
	} else { /* move cursor to the end of the word */
		while (text[cursor] && strchr(worddelimiters, text[cursor]))
			cursor = nextrune(+1);
		while (text[cursor] && !strchr(worddelimiters, text[cursor]))
			cursor = nextrune(+1);
	}
}

static void
keypress(XKeyEvent *ev)
{
	char buf[32];
	int len;
	KeySym ksym;
	Status status;
	int i, offscreen = 0;
	struct item *tmpsel;

	len = XmbLookupString(xic, ev, buf, sizeof buf, &ksym, &status);
	switch (status) {
	default: /* XLookupNone, XBufferOverflow */
		return;
	case XLookupChars:
		goto insert;
	case XLookupKeySym:
	case XLookupBoth:
		break;
	}

	if (ev->state & ControlMask) {
		switch(ksym) {
		case XK_a: ksym = XK_Home;      break;
		case XK_b: ksym = XK_Left;      break;
		case XK_c: ksym = XK_Escape;    break;
		case XK_d: ksym = XK_Delete;    break;
		case XK_e: ksym = XK_End;       break;
		case XK_f: ksym = XK_Right;     break;
		case XK_g: ksym = XK_Escape;    break;
		case XK_h: ksym = XK_BackSpace; break;
		case XK_i: ksym = XK_Tab;       break;
		case XK_j: /* fallthrough */
		case XK_J: /* fallthrough */
		case XK_m: /* fallthrough */
		case XK_M: ksym = XK_Return; ev->state &= ~ControlMask; break;
		case XK_n: ksym = XK_Down;      break;
		case XK_p: ksym = XK_Up;        break;

		case XK_k: /* delete right */
				   text[cursor] = '\0';
				   match();
				   break;
		case XK_u: /* delete left */
				   insert(NULL, 0 - cursor);
				   break;
		case XK_w: /* delete word */
				   while (cursor > 0 && strchr(worddelimiters, text[nextrune(-1)]))
					   insert(NULL, nextrune(-1) - cursor);
				   while (cursor > 0 && !strchr(worddelimiters, text[nextrune(-1)]))
					   insert(NULL, nextrune(-1) - cursor);
				   break;
		case XK_y: /* paste selection */
		case XK_Y:
				   XConvertSelection(dpy, (ev->state & ShiftMask) ? clip : XA_PRIMARY,
						   utf8, utf8, win, CurrentTime);
				   return;
		case XK_Left:
		case XK_KP_Left:
				   movewordedge(-1);
				   goto draw;
		case XK_Right:
		case XK_KP_Right:
				   movewordedge(+1);
				   goto draw;
		case XK_Return:
		case XK_KP_Enter:
				   break;
		case XK_bracketleft:
				   cleanup();
				   exit(1);
		default:
				   return;
		}
	} else if (ev->state & Mod1Mask) {
		switch(ksym) {
			case XK_b:
				movewordedge(-1);
				goto draw;
			case XK_f:
				movewordedge(+1);
				goto draw;
			case XK_g: ksym = XK_Home;  break;
			case XK_G: ksym = XK_End;   break;
			case XK_h: ksym = XK_Up;    break;
			case XK_j: ksym = XK_Next;  break;
			case XK_k: ksym = XK_Prior; break;
			case XK_l: ksym = XK_Down;  break;
			default:
					   return;
		}
	} else if (vi_mode && !ins_mode) {
		switch(ksym) {
			case XK_h:
				if (columns > 0)
					ksym = XK_Left;
				break;
			case XK_l:
				if (columns > 0)
					ksym = XK_Right;
				break;
			case XK_K:
			case XK_k:
				if (lines > 1) {
					if (ev->state & ShiftMask && vi_mark) {
						if (sel->marked)
							de_weaver(NULL, sel, NULL, 0);
						else
							weave_marked(NULL, sel, NULL, 0);
						drawmenu();
						return;
					}
					ksym = XK_Up;
				} else
					return;
				break;
			case XK_J:
			case XK_j:
				if (lines > 1) {
					if (ev->state & ShiftMask && vi_mark) {
						if (sel->marked)
							de_weaver(sel, NULL, NULL, 0);
						else
							weave_marked(sel, NULL, NULL, 0);
						drawmenu();
						return;
					}
					ksym = XK_Down;
				} else
					return;
				break;
				//case XK_E:
				//	if (vi_mark && lastmarked) { //TODO
				//		struct item *itemmark;
				//		for (itemmark = lastmarked; itemmark != sel; itemmark++) {
				//			itemmark->marked = 1;
				//		}
				//		sel->marked = 1;
				//	}
			case XK_w:
				if (flip_g) {
					ch_border_col(0);
					flip_g = 0;
				} else {
					ch_border_col(1);
					flip_g = 1;
				}
				return;
			case XK_f:
				ksym = XK_m;
			case XK_m:
				if (vi_mark) {
					if (flip_g) {
						weave_marked(NULL, NULL, sel, 0);
						flip_g = 0;
					} else
						if (sel->marked)
							de_weaver(sel, sel, NULL, 0);
						else
							weave_marked(sel, sel, NULL, 0);
				}
				drawmenu();
				return;
			case XK_g:
			case XK_G:
				if (flip_g && vi_insert) {
					flip_g = 0;
					sel = curr = matches;
					calcoffsets();
					drawmenu();
				} else if (ev->state & ShiftMask) {
					if (next) {
						/* jump to end of list and position items in reverse */
						curr = matchend;
						calcoffsets();
						curr = prev;
						calcoffsets();
						while (next && (curr = curr->right))
							calcoffsets();
					}
					sel = matchend;
					drawmenu();
					flip_g = 0;
				} else
					flip_g = 1;
				return;
			case XK_d:
				if (flip_d && (strlen(text) > 0) && vi_insert) {
					insert(NULL, 0 - cursor);
					drawmenu();
					flip_d = 0;
				} else if (strlen(text) > 0 && vi_insert)
					flip_d = 1;
				return;
			case XK_p:
			case XK_P:
				if ((flip_p || (ev->state & ShiftMask))&& vi_insert) {
					XConvertSelection(dpy, (flip_g && !(flip_g = 0)) ? XA_PRIMARY : clip,
							utf8, utf8, win, CurrentTime);
					if (!(ev->state & ShiftMask))
						ksym = XK_i;
					break;
				} else {
					flip_p = 1;
					return;
				}
			case XK_c:
				if (flip_c && vi_insert) {
					if (strlen(text) > 0) 
						insert(NULL, 0 - cursor);
					flip_c = 0;
					ksym = XK_i;
					break;
				} else if (vi_insert) {
					flip_c = 1;
					return;
				}
			case XK_slash:
				if (vi_insert) {
					flip_slash = 1;
					if (strlen(text) > 0) 
						insert(NULL, 0 - cursor);
					ksym = XK_i;
					break;
				} else
					return;
			case XK_i: break;
			case XK_Return: break;
			case XK_Escape: break;
			default:
							return;
		}
		switch(ksym) {
			case XK_i:
				if (vi_insert) {
					resizemenu(1);
					ins_mode = 1;
					drawmenu();
				}
				return;
			default:
				break;
		}
		switch(ksym) {
		}
	}
	switch(ksym) {
		default:
insert:
			if (!iscntrl((unsigned char)*buf))
				insert(buf, len);
			break;
		case XK_Delete:
		case XK_KP_Delete:
			if (text[cursor] == '\0')
				return;
			cursor = nextrune(+1);
			/* fallthrough */
		case XK_BackSpace:
			if (cursor == 0)
				return;
			insert(NULL, nextrune(-1) - cursor);
			break;
		case XK_End:
		case XK_KP_End:
			if (text[cursor] != '\0') {
				cursor = strlen(text);
				break;
			}
			if (next) {
				/* jump to end of list and position items in reverse */
				curr = matchend;
				calcoffsets();
				curr = prev;
				calcoffsets();
				while (next && (curr = curr->right))
					calcoffsets();
			}
			sel = matchend;
			break;
		case XK_Escape:
			if (flip_slash) {
				insert(NULL, 0 - cursor); //uncomment this if you want this feature, idk Aaaaah!!!
				flip_slash = 0;
			}
			if (flip_p) {
				insert(NULL, 0 - cursor); //uncomment this if you want this feature, idk Aaaaah!!!
				flip_p = 0;
			}
			if (ins_mode) {
				resizemenu(0);
				ins_mode = 0;
				break;
			}
			cleanup();
			exit(1);
		case XK_Home:
		case XK_KP_Home:
			if (sel == matches) {
				cursor = 0;
				break;
			}
			sel = curr = matches;
			calcoffsets();
			break;
		case XK_Left:
			if (columns > 1) {
				if (!sel)
					return;
				tmpsel = sel;
				for (i = 0; i < lines; i++) {
					if (!tmpsel->left || tmpsel->left->right != tmpsel) {
						if (offscreen)
							break;
						return;
					}
					if (tmpsel == curr)
						offscreen = 1;
					tmpsel = tmpsel->left;
				}
				sel = tmpsel;
				if (offscreen) {
					curr = prev;
					calcoffsets();
				}
				break;
			}
		case XK_KP_Left:
			if (cursor > 0 && (!sel || !sel->left || lines > 0)) {
				cursor = nextrune(-1);
				break;
			}
			if (lines > 0)
				return;
			/* fallthrough */
		case XK_Up:
		case XK_KP_Up:
			if (sel && sel->left && (sel = sel->left)->right == curr) {
				curr = prev;
				calcoffsets();
			}
			break;
		case XK_Next:
		case XK_KP_Next:
			if (!next)
				return;
			sel = curr = next;
			calcoffsets();
			break;
		case XK_Prior:
		case XK_KP_Prior:
			if (!prev)
				return;
			sel = curr = prev;
			calcoffsets();
			break;
		case XK_Return:
		case XK_KP_Enter:
			if (debugging) {
				break;
			}
			if (flip_slash || flip_p) {
				resizemenu(0);
				flip_p = flip_slash = 0;
				ins_mode = 0;
				break;
			} else if (vi_mode) {
				if (vi_mark && m_list) {
					struct item *item;
					for (item = m_list; item; item = item->m_right) {
						if (!item->marked)
							continue;
						if (print_only_index)
							printf("%d\n", item->index);
						else {
							if (print_index)
								printf("%d\t", item->index);
							puts(item->text);
						}
					}
				} else if (!m_list && sel) {
					if (print_only_index) 
						printf("%d\n", sel->index);
					else {
						if (print_index)
							printf("%d\t", sel->index);
						puts(sel->text);
					}
				}
			} else {
				if (print_only_index && !gtyped)
					printf("%d\n", (sel && !(ev->state & ShiftMask)) ? sel->index : -1); 
				else if (sel) {
					if (print_index)
						printf("%d\t", sel->index);
					puts((!(ev->state & ShiftMask) || gtyped) ? sel->text : text);
				}
			}

			if (!(ev->state & ControlMask)) {
				cleanup();
				exit(0);
			}
			if (sel)
				sel->marked = 1;
			break;
		case XK_Right:
			if (columns > 1) {
				if (!sel)
					return;
				tmpsel = sel;
				for (i = 0; i < lines; i++) {
					if (!tmpsel->right ||  tmpsel->right->left != tmpsel) {
						if (offscreen)
							break;
						return;
					}
					tmpsel = tmpsel->right;
					if (tmpsel == next)
						offscreen = 1;
				}
				sel = tmpsel;
				if (offscreen) {
					curr = next;
					calcoffsets();
				}
				break;
			}
		case XK_KP_Right:
			if (text[cursor] != '\0') {
				cursor = nextrune(+1);
				break;
			}
			if (lines > 0)
				return;
			/* fallthrough */
		case XK_Down:
		case XK_KP_Down:
			if (sel && sel->right && (sel = sel->right) == next) {
				curr = next;
				calcoffsets();
			}
			break;
		case XK_Tab:
			if (!sel)
				return;
			strncpy(text, sel->text, sizeof text - 1);
			text[sizeof text - 1] = '\0';
			cursor = strlen(text);
			match();
			break;
	}

draw:
	drawmenu();
}

static void
paste(void)
{
	char *p, *q;
	int di;
	unsigned long dl;
	Atom da;

	/* we have been given the current selection, now insert it into input */
	if (XGetWindowProperty(dpy, win, utf8, 0, (sizeof text / 4) + 1, False,
	                   utf8, &da, &di, &dl, &dl, (unsigned char **)&p)
	    == Success && p) {
		insert(p, (q = strchr(p, '\n')) ? q - p : (ssize_t)strlen(p));
		XFree(p);
	}
	drawmenu();
}

static void
readstdin(void)
{
	char buf[sizeof text], *p;
	size_t i, size = 0;

	/* read each line from stdin and add it to the item list */
	for (i = 0; fgets(buf, sizeof buf, stdin); i++) {
		if (i + 1 >= size / sizeof *items) {
			if (!(items = realloc(items, (size += BUFSIZ))))
				die("cannot realloc %zu bytes:", size);
		}
		if ((p = strchr(buf, '\n')))
			*p = '\0';
		if (!(items[i].text = strdup(buf)))
			die("cannot strdup %zu bytes:", strlen(buf) + 1);
		items[i].marked = 0;
		items[i].index = i;
		items[i].m_right = items[i].m_left = NULL;
	}
	if (items)
		items[i].text = NULL;
	lines = MIN(lines, i);
}

static void
run(void)
{
	XEvent ev;

	while (!XNextEvent(dpy, &ev)) {
		if (XFilterEvent(&ev, win))
			continue;
		switch(ev.type) {
		case DestroyNotify:
			if (ev.xdestroywindow.window != win)
				break;
			cleanup();
			exit(1);
		case Expose:
			if (ev.xexpose.count == 0)
				drw_map(drw, win, 0, 0, mw, mh);
			break;
		case FocusIn:
			/* regrab focus from parent window */
			if (ev.xfocus.window != win)
				grabfocus();
			break;
		case KeyPress:
			keypress(&ev.xkey);
			break;
		case SelectionNotify:
			if (ev.xselection.property == utf8)
				paste();
			break;
		case VisibilityNotify:
			if (ev.xvisibility.state != VisibilityUnobscured)
				XRaiseWindow(dpy, win);
			break;
		}
	}
}

static void
setup(void)
{
	int x, y, i, j;
	unsigned int du;
	XSetWindowAttributes swa;
	XIM xim;
	Window w, dw, *dws;
	XWindowAttributes wa;
	XClassHint ch = {"dmenu", "dmenu"};
#ifdef XINERAMA
	XineramaScreenInfo *info;
	Window pw;
	int a, di, n, area = 0;
#endif
	/* init appearance */
	for (j = 0; j < SchemeLast; j++)
		scheme[j] = drw_scm_create(drw, colors[j], 2);

	clip = XInternAtom(dpy, "CLIPBOARD",   False);
	utf8 = XInternAtom(dpy, "UTF8_STRING", False);

	/* calculate menu geometry */
	bh = drw->fonts->h + 2;
	bh = MAX(bh,lineheight);	/* make a menu line AT LEAST 'lineheight' tall */
	lines = MAX(lines, 0);
	mh = (lines + 1) * bh;
	promptw = (prompt && *prompt) ? TEXTW(prompt) - lrpad / 4 : 0;

	// removing input box if in vi mode
	if (vi_mode && lines > 0) {
		mh -= bh;
		if (!topbar && !centered)
			y += bh;
	}

#ifdef XINERAMA
	i = 0;
	if (parentwin == root && (info = XineramaQueryScreens(dpy, &n))) {
		XGetInputFocus(dpy, &w, &di);
		if (mon >= 0 && mon < n)
			i = mon;
		else if (w != root && w != PointerRoot && w != None) {
			/* find top-level window containing current input focus */
			do {
				if (XQueryTree(dpy, (pw = w), &dw, &w, &dws, &du) && dws)
					XFree(dws);
			} while (w != root && w != pw);
			/* find xinerama screen with which the window intersects most */
			if (XGetWindowAttributes(dpy, pw, &wa))
				for (j = 0; j < n; j++)
					if ((a = INTERSECT(wa.x, wa.y, wa.width, wa.height, info[j])) > area) {
						area = a;
						i = j;
					}
		}
		/* no focused window is on screen, so use pointer location instead */
		if (mon < 0 && !area && XQueryPointer(dpy, root, &dw, &dw, &x, &y, &di, &di, &du))
			for (i = 0; i < n; i++)
				if (INTERSECT(x, y, 1, 1, info[i]) != 0)
					break;

		if (dmenu_width) {
			mw = dmenu_width - 2 * border_width;
		} else if (linewidth) {
			mw = linewidth * columns;
		} else if  (centered) {
			mw = MIN(MAX(max_textw() + promptw, min_width), info[i].width) - 2 * border_width;
		} else {
			mw = info[i].width - 2 * border_width;
		}

		if (centered) {
			x = info[i].x_org + ((info[i].width  - mw) / 2) - border_width;
			y = info[i].y_org + ((info[i].height - mh) / 2) - border_width;
		}else if (att_edge == 1) {
			x = info[i].x_org + ((info[i].width  - mw) / 2) - border_width;
			y = info[i].y_org + egapp;
		}else if (att_edge == 2) {
			x = info[i].x_org + ((info[i].width  - mw) / 2) - border_width;
			y = info[i].y_org + (info[i].height - egapp - mh - 2 * border_width);
		}else if (att_edge == 3) {
			x = info[i].x_org + (info[i].width  - mw - egapp - 2 * border_width);
			y = info[i].y_org + ((info[i].height - mh) / 2) - border_width;
		}else if (att_edge == 4) {
			x = info[i].x_org + egapp;
			y = info[i].y_org + ((info[i].height - mh) / 2) - border_width;
		} else if (att_cor == 1) {
			x = info[i].x_org + gappx;
			y = info[i].y_org + gappy;
		} else if (att_cor == 2) {
			x = info[i].x_org + (info[i].width  - mw - gappx - 2 * border_width);
			y = info[i].y_org + (info[i].height - gappy - mh - 2 * border_width);
		} else if (att_cor == 3) {
			x = info[i].x_org + (info[i].width  - mw - gappx - 2 * border_width);
			y = info[i].y_org + gappy;
		} else if (att_cor == 4) {
			x = info[i].x_org + gappx;
			y = info[i].y_org + (info[i].height - gappy - mh - 2 * border_width);
		} else {
			x = info[i].x_org;
			y = info[i].y_org + (topbar ? 0 : info[i].height - mh - 2 * border_width);
		}

		XFree(info);
	} else
#endif
	{
		if (!XGetWindowAttributes(dpy, parentwin, &wa))
			die("could not get embedding window attributes: 0x%lx",
			    parentwin);
		if (dmenu_width) {
			mw = dmenu_width - 2 * border_width;
		} else if  (centered) {
			mw = MIN(MAX(max_textw() + promptw, min_width), info[i].width) - 2 * border_width;
		} else {
			mw = info[i].width - 2 * border_width;
		}

		if (centered) {
			x = info[i].x_org + ((info[i].width  - mw) / 2) - border_width;
			y = info[i].y_org + ((info[i].height - mh) / 2) - border_width;
		} else if (att_edge == 1) {
			x = info[i].x_org + ((info[i].width  - mw) / 2) - border_width;
			y = info[i].y_org + egapp;
		} else if (att_edge == 2) {
			x = info[i].x_org + ((info[i].width  - mw) / 2) - border_width;
			y = info[i].y_org + (info[i].height - egapp - mh - 2 * border_width);
		} else if (att_edge == 3) {
			x = info[i].x_org + (info[i].width  - mw - egapp - 2 * border_width);
			y = info[i].y_org + ((info[i].height - mh) / 2) - border_width;
		} else if (att_edge == 4) {
			x = info[i].x_org + egapp;
			y = info[i].y_org + ((info[i].height - mh) / 2) - border_width;
		} else if (att_cor == 1) {
			x = info[i].x_org + gappx;
			y = info[i].y_org + gappy;
		} else if (att_cor == 2) {
			x = info[i].x_org + (info[i].width  - mw - gappx - 2 * border_width);
			y = info[i].y_org + (info[i].height - gappy - mh - 2 * border_width);
		} else if (att_cor == 3) {
			x = info[i].x_org + (info[i].width  - mw - gappx - 2 * border_width);
			y = info[i].y_org + gappy;
		} else if (att_cor == 4) {
			x = info[i].x_org + gappx;
			y = info[i].y_org + (info[i].height - gappy - mh - 2 * border_width);
		} else {
			x = info[i].x_org;
			y = info[i].y_org + (topbar ? 0 : info[i].height - mh - 2 * border_width);
		}

	}
	inputw = mw / 3; /* input width: ~33% of monitor width */
	match();

	//saving x and y values for resizing in vi mode
	resize_x = x;
	resize_y = y;

	/* create menu window */
	swa.override_redirect = True;
	swa.background_pixel = scheme[SchemeNorm][ColBg].pixel;
	swa.event_mask = ExposureMask | KeyPressMask | VisibilityChangeMask;
	win = XCreateWindow(dpy, parentwin, x, y, mw, mh, border_width,
	                    CopyFromParent, CopyFromParent, CopyFromParent,
	                    CWOverrideRedirect | CWBackPixel | CWEventMask, &swa);
	XSetWindowBorder(dpy, win, scheme[SchemeSel][ColBg].pixel);
	XSetClassHint(dpy, win, &ch);

	/* input methods */
	if ((xim = XOpenIM(dpy, NULL, NULL, NULL)) == NULL)
		die("XOpenIM failed: could not open input device");

	xic = XCreateIC(xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
	                XNClientWindow, win, XNFocusWindow, win, NULL);

	XMapRaised(dpy, win);
	if (embed) {
		XSelectInput(dpy, parentwin, FocusChangeMask | SubstructureNotifyMask);
		if (XQueryTree(dpy, parentwin, &dw, &w, &dws, &du) && dws) {
			for (i = 0; i < du && dws[i] != win; ++i)
				XSelectInput(dpy, dws[i], FocusChangeMask);
			XFree(dws);
		}
		grabfocus();
	}
	drw_resize(drw, mw, mh);
	drawmenu();
}

static void
usage(void)
{
	fputs("usage: dmenu [-bfiv] [-l lines] [-h height] [-p prompt] [-fn font] [-m monitor]\n"
	      "             [-nb color] [-nf color] [-sb color] [-sf color] [-w windowid]\n"
		  "             [-o outputs marked in batch (use -vi with this)] [-vi for vi mode]\n"
		  "             [-gt returns what you typed]\n", stderr);
	exit(1);
}

int
main(int argc, char *argv[])
{
	XWindowAttributes wa;
	int i, fast = 0;

	for (i = 1; i < argc; i++)
		/* these options take no arguments */
		if (!strcmp(argv[i], "-v")) {       /* prints version information */
			puts("dmenu-"VERSION);
			exit(0);
		} else if (!strcmp(argv[i], "-b"))  /* appears at the bottom of the screen */
			topbar = 0;
		else if (!strcmp(argv[i], "-gt"))   /* returns only what you typed */
			gtyped = 1;
		else if (!strcmp(argv[i], "-ni"))   /* disables insert mode in vi mode*/
			vi_insert = 0;
		else if (!strcmp(argv[i], "-f"))    /* grabs keyboard before reading stdin */
			fast = 1;
		else if (!strcmp(argv[i], "-db"))    /* grabs keyboard before reading stdin */
			debugging = 1;
		else if (!strcmp(argv[i], "-c"))    /* centers dmenu on screen */
			centered = 1;
		else if (!strcmp(argv[i], "-vi")) { /* enables vi mode */
			vi_mode = 1;
			if (lines == 0)
				lines = 1;
			if (columns == 0)
				columns = 1;
		} else if (!strcmp(argv[i], "-nm")) /* disables marking ability */
			vi_mark = 0;
		else if (!strcmp(argv[i], "-i")) {  /* case-insensitive item matching */
			fstrncmp = strncasecmp;
			fstrstr = cistrstr;
		} else if (!strcmp(argv[i], "-ix")) /* adds ability to return index in list */
			print_index = 1;
		else if (!strcmp(argv[i], "-oix")) /* adds ability to return index in list */
			print_only_index = print_index = 1;
		else if (!strcmp(argv[i], "-dt")) /* uses dots instead of a different color */
			use_dots = 1;
		else if (i + 1 == argc)
			usage();
		/* these options take one argument */
		else if (!strcmp(argv[i], "-dw")) {   /* sets dmenu width */
			dmenu_width = strtol(argv[++i], NULL, 10);
		} else if (!strcmp(argv[i], "-qy")) {
			strncpy(text, argv[++i], sizeof text - 1);
			cursor = sizeof argv[i];
		} else if (!strcmp(argv[i], "-ae")) {   /* attach to edge */
			att_edge = strtol(argv[++i], NULL, 10);
			if (att_edge > 4 || att_edge < 0)
				usage();
		} else if (!strcmp(argv[i], "-ac")) {   /* attach to corner */
			att_cor = strtol(argv[++i], NULL, 10);
			if (att_cor > 4 || att_cor < 0)
				usage();
		} else if (!strcmp(argv[i], "-egx")) {   /* gapp between edge parallel to x axis and menu */
			gappx = strtol(argv[++i], NULL, 10);
		} else if (!strcmp(argv[i], "-egy")) {   /* gapp between edge parallel to y axis and menu */
			gappy = strtol(argv[++i], NULL, 10);
		} else if (!strcmp(argv[i], "-eg")) {   /* gapp between edge and menu */
			egapp = strtol(argv[++i], NULL, 10);
		} else if (!strcmp(argv[i], "-g")) {   /* number of columns in grid */
			columns = atoi(argv[++i]);
			if (lines == 0) lines = 1;
		} else if (!strcmp(argv[i], "-w")) {   /* minimum width of one menu line */
			linewidth = strtol(argv[++i], NULL, 10);
			linewidth = MAX(linewidth, min_linewidth);
		} else if (!strcmp(argv[i], "-l")) { /* number of lines in grid */
			lines = atoi(argv[++i]);
			if (columns == 0) columns = 1;
		} else if (!strcmp(argv[i], "-h")) { /* minimum height of one menu line */
			lineheight = atoi(argv[++i]);
			lineheight = MAX(lineheight, min_lineheight);
		} else if (!strcmp(argv[i], "-m"))
			mon = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-p"))   /* adds prompt to left of input field */
			prompt = argv[++i];
		else if (!strcmp(argv[i], "-dc"))   /* dot charecter or charecters */
			dot_char = argv[++i];
		else if (!strcmp(argv[i], "-fn"))  /* font or font set */
			fonts[0] = argv[++i];
		else if (!strcmp(argv[i], "-nb"))  /* normal background color */
			colors[SchemeNorm][ColBg] = argv[++i];
		else if (!strcmp(argv[i], "-nf"))  /* normal foreground color */
			colors[SchemeNorm][ColFg] = argv[++i];
		else if (!strcmp(argv[i], "-sb"))  /* selected background color */
			colors[SchemeSel][ColBg] = argv[++i];
		else if (!strcmp(argv[i], "-sf"))  /* selected foreground color */
			colors[SchemeSel][ColFg] = argv[++i];
		else if (!strcmp(argv[i], "-w"))   /* embedding window id */
			embed = argv[++i];
		else
			usage();

	if (!setlocale(LC_CTYPE, "") || !XSupportsLocale())
		fputs("warning: no locale support\n", stderr);
	if (!(dpy = XOpenDisplay(NULL)))
		die("cannot open display");
	screen = DefaultScreen(dpy);
	root = RootWindow(dpy, screen);
	if (!embed || !(parentwin = strtol(embed, NULL, 0)))
		parentwin = root;
	if (!XGetWindowAttributes(dpy, parentwin, &wa))
		die("could not get embedding window attributes: 0x%lx",
		    parentwin);
	drw = drw_create(dpy, screen, root, wa.width, wa.height);
	if (!drw_fontset_create(drw, fonts, LENGTH(fonts)))
		die("no fonts could be loaded.");
	lrpad = drw->fonts->h;

#ifdef __OpenBSD__
	if (pledge("stdio rpath", NULL) == -1)
		die("pledge");
#endif

	if (fast && !isatty(0)) {
		grabkeyboard();
		readstdin();
	} else {
		readstdin();
		grabkeyboard();
	}
	setup();
	run();

	return 1; /* unreachable */
}
