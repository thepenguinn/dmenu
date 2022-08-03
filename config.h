/* See LICENSE file for copyright and license details. */
/* Default settings; can be overriden by command line. */

static int topbar = 1;       /* -b  option; if 0, dmenu appears at bottom     */
static int vi_mode = 0;      /* -n option; if 0, no vi mode */
static int ins_mode = 0;     /* for insert mode */
static int no_input = 0;     /* -ni option; if 1 disables input in vi mode */
static int no_out_marked = 0;/* -no option; if 1, disables marking in vi mode */
static int centered = 0;     /* -c option; centers dmenu on screen */
static int att_edge = 0;     /* 0 disabled, 1 top, 2 bottom, 3 right, 4 left NOTE: Doest not work if -c is enabled*/
static int egapp = 0;        /* gap between edge and menu when att_edge is enabled*/
static int att_cor = 0;      /* 0 disabled, 1 top-left, 2 bottom-right, 3 bottom-left, 4 top-right NOTE: Doest not work if -c or -at is enabled*/
static int gappx = 0;        /* gap between nearest edge parallel to x axis and dmenu */
static int gappy = 0;        /* gap between nearest edge parallel to y axis and dmenu */
static int min_width = 500;  /* minimum width when centered */
static int dmenu_width = 0;  /* dmenu width */
static int gtyped = 0;       /* returns what you typed */
/*static int min_width = 500;                     minimum width when centered */
/* -fn option overrides fonts[0]; default X11 font or font set */
static const char *fonts[] = {
	"Source Code Pro:style=Medium:size=11",
	"Source Code Pro:style=Medium:size=16"
};

static int dots = 1;
static const char *fonts_dots[] = {
	"Source Code Pro:style=Medium:size=16"
};

static const char *prompt = NULL;      /* -p  option; prompt to the left of input field */
static const char *colors[SchemeLast][2] = {
	          /*     fg         bg       */
	[SchemeNorm]   =  { "#cba6f7", "#1e1e2e" },
	[SchemeSel]    =  { "#1e1e2e", "#cba6f7" },
	[SchemeOut]    =  { "#1e1e2e", "#fab387" },
	[SchemeSelOut] =  { "#1e1e2e", "#f9e2af" },
	[SchemeNoBg]   =  { "#cba6f7", "#00ffff" },
};

//static const char col_dmenu[]       = "#a885dd";
//[SchemeSel] = { "#DDB6F2" },

/* -l and -g options; controls number of lines and columns in grid if > 0 */
static unsigned int lines      = 0;
static unsigned int columns    = 0;

/* -h option; minimum height of a menu line */
static unsigned int lineheight = 0;
static unsigned int min_lineheight = 8;

/* -w option; minimum width of a menu line */
static unsigned int linewidth = 0;
static unsigned int min_linewidth = 8;

/* draw dot for some reason */
// static int dot_rad = 0;


/*
 * Characters not considered part of a word while deleting words
 * for example: " /?\"&[]"
 */
static const char worddelimiters[] = " ";

/* Size of the window border */
static const unsigned int border_width = 3;
