/*
 * Direct video memory display driver
 * Saku Airila 1996
 * Color originally written by Mbit and Davey
 * Re-wrote for new ntty iface
 * Al Riddoch  1999
 *
 * Rewritten by Greg Haerr <greg@censoft.com> July 1999
 * added reverse video, cleaned up code, reduced size
 * added enough ansi escape sequences for visual editing
 */
#include <linuxmt/types.h>
#include <linuxmt/config.h>
#include <linuxmt/errno.h>
#include <linuxmt/fcntl.h>
#include <linuxmt/fs.h>
#include <linuxmt/kernel.h>
#include <linuxmt/major.h>
#include <linuxmt/mm.h>
#include <linuxmt/sched.h>
#include <linuxmt/chqueue.h>
#include <linuxmt/ntty.h>

#include <arch/io.h>
#include <arch/keyboard.h>

#ifdef CONFIG_CONSOLE_DIRECT

#include "console.h"

/* Assumes ASCII values. */
#define isalpha(c) (((unsigned char)(((c) | 0x20) - 'a')) < 26)

#if 0
/* public interface of dircon.c: */

void con_charout(char Ch);
int Console_write(struct tty *tty);
void Console_release(struct tty *tty);
int Console_open(struct tty *tty);
struct tty_ops dircon_ops;
void init_console(void);
#endif

#define MAX_ATTR 	5
#define A_DEFAULT 	0x07
#define A_BOLD 		0x08
#define A_BLINK 	0x80
#define A_REVERSE	0x70
#define A_BLANK		0x00

/* character definitions*/
#define BS		'\b'
#define NL		'\n'
#define CR		'\r'
#define TAB		'\t'
#define ESC		27
#define BEL		0x07

/* console states*/
#define ST_NORMAL	0	/* no ESC seen */
#define ST_ESCSEEN	1	/* ESC seen */
#define ST_ANSI		2	/* ESC [ seen */
#define ST_ESCP		3	/* ESC P seen */
#define ST_ESCQ		4	/* ESC Q seen */
#define	ST_ESCY		5	/* ESC Y seen */
#define ST_ESCY2	6	/* ESC Y ch seen */

#define MAXPARMS	10
typedef struct {
    int cx, cy;			/* cursor position */
    int state;			/* escape state */
    unsigned int vseg;		/* video segment for page */
    int pageno;			/* video ram page # */
    unsigned char attr;		/* current attribute */
#ifdef CONFIG_DCON_VT52
    unsigned char tmp;		/* ESC Y ch save */
#ifdef CONFIG_DCON_ANSI
    int savex, savey;		/* saved cursor position */
    unsigned char *parmptr;	/* ptr to params */
    unsigned char params[MAXPARMS];	/* ANSI params */
#endif
#endif
} Console;

static struct wait_queue glock_wait;
static Console Con[MAX_CONSOLES], *Visible;
static Console *glock;		/* Which console owns the graphics hardware */
static void *CCBase;
static int Width, Height, MaxRow, MaxCol;
static unsigned VideoSeg, PageSize;
static unsigned short int NumConsoles = MAX_CONSOLES;

/* from keyboard.c */
extern int Current_VCminor;
extern int kraw;

#ifdef CONFIG_DCON_VT52
static unsigned AttrArry[MAX_ATTR] = {
    A_DEFAULT,
    A_BOLD,
    A_BLINK,
    A_REVERSE,
    A_BLANK
};
#endif

static void ScrollUp(register Console * C, int st, int en);
static void ClearRange(register Console * C, int x, int y, int xx, int yy);

#ifdef CONFIG_DCON_VT52

static void ScrollDown(register Console * C, int st, int en);
void Vt52Cmd(register Console * C, char c);
void Vt52CmdEx(register Console * C, char c);
static void Console_gotoxy(register Console * C, int x, int y);

#ifdef CONFIG_DCON_ANSI

static void AnsiCmd(register Console * C, char c);

#endif

#endif

extern void AddQueue(unsigned char Key);	/* From xt_key.c */

static void PositionCursor(register Console * C)
{
    register char *CCBasep = (char *) CCBase;
    int Pos;

    if (C == Visible) {
	Pos = C->cx + Width * C->cy + (C->pageno * PageSize >> 1);
	outb(14, CCBasep);
	outb((unsigned char) ((Pos >> 8) & 0xFF), CCBasep + 1);
	outb(15, CCBasep);
	outb((unsigned char) (Pos & 0xFF), CCBasep + 1);
    }
}

void WriteChar(register Console * C, char c)
{
    unsigned int offset;

    /* check for graphics lock */
    while (glock) {
	if (glock == C)
	    return;
	sleep_on(&glock_wait);
    }

#ifdef CONFIG_DCON_ANSI
    /* ANSI param gathering and processing */
    if (C->state == ST_ANSI) {
	if (C->parmptr < &C->params[MAXPARMS])
	    *C->parmptr++ = (unsigned char) c;
	if (isalpha(c)) {
	    *C->parmptr = 0;
	    AnsiCmd(C, c);
	}
	return;
    }
#endif

#ifdef CONFIG_DCON_VT52
    /* VT52 command processing */
    if (C->state >= ST_ESCSEEN) {
#ifdef CONFIG_DCON_ANSI
	if (c == '[') {
	    C->state = ST_ANSI;
	    C->parmptr = C->params;
	} else
#endif
	    Vt52Cmd(C, c);
	return;
    }
#endif

    /* normal character processing */
    switch (c) {
    case BEL:
	bell();
	return;
#ifdef CONFIG_DCON_VT52
    case ESC:
	C->state = ST_ESCSEEN;
	return;
#endif
    case BS:
	if (C->cx > 0) {
	    --C->cx;
	    WriteChar(C, ' ');
	    --C->cx;
	}
	return;
    case NL:
	++C->cy;
	break;
    case CR:
	C->cx = 0;
	break;
    default:
	offset = ((unsigned int) (C->cx + C->cy * Width)) << 1;
	pokew((__u16) C->vseg, (__u16) offset, ((__u16)C->attr << 8) | ((__u16)c));
	C->cx++;
    }

    /* autowrap and/or scroll */
    if (C->cx > MaxCol) {
	C->cx = 0;
	C->cy++;
    }
    if (C->cy >= Height) {
	ScrollUp(C, 1, Height);
	C->cy--;
    }
}

void con_charout(char Ch)
{
    if (Ch == '\n')
	WriteChar(Visible, '\r');
    WriteChar(Visible, Ch);
    PositionCursor(Visible);
}

static void ScrollUp(register Console * C, int st, int en)
{
    unsigned rdofs, wrofs;

    if (st >= 1 && st < en) {
	wrofs = (rdofs = (unsigned int) ((Width << 1) * st)) - (Width << 1);
	far_memmove(C->vseg, rdofs, C->vseg, wrofs,
		    ((unsigned) (Width * (en - st))) << 1);
	en--;
	ClearRange(C, 0, en, Width, en);
    }
}

#ifdef CONFIG_DCON_VT52

static void ScrollDown(register Console * C, int st, int en)
{
    unsigned rdofs, wrofs;

    if (st <= en && en <= Height) {
	wrofs = (rdofs = (unsigned int) ((Width << 1) * st)) + (Width << 1);
	far_memmove(C->vseg, rdofs, C->vseg, wrofs,
		    ((unsigned) (Width * (en - st))) << 1);
	ClearRange(C, 0, st, Width, st);
    }
}

#endif

static void ClearRange(register Console * C, int x, int y, int xx, int yy)
{
    __u16 en, ClrW;
    register char *ofsp;

    ClrW = (__u16) ((A_DEFAULT << 8) + ' ');
    en = (__u16) ((xx + yy * Width) << 1);
    ofsp = (char *)((__u16) ((x + y * Width) << 1));
    while (((__u16)ofsp) < en) {
	pokew((__u16) C->vseg, (__u16) ofsp, ClrW);
	ofsp += 2;
    }
}

#ifdef CONFIG_DCON_VT52

void Vt52Cmd(register Console * C, char c)
{
    /* process multi character ESC sequences */
    if (C->state > ST_ESCSEEN) {
	Vt52CmdEx(C, c);
	return;
    }

    /* process single ESC char sequences */
    switch (c) {
    case 'H':			/* home */
	C->cx = C->cy = 0;
	break;
    case 'A':			/* up */
	if (C->cy)
	    --C->cy;
	break;
    case 'B':			/* down */
	if (C->cy < MaxRow)
	    ++C->cy;
	break;
    case 'C':			/* right */
	if (C->cx < MaxCol)
	    ++C->cx;
	break;
    case 'D':			/* left */
	if (C->cx)
	    --C->cx;
	break;
    case 'K':			/* clear to eol */
	ClearRange(C, C->cx, C->cy, Width, C->cy);
	break;
    case 'J':			/* clear to eoscreen */
	ClearRange(C, 0, 0, Width, Height);
	break;
    case 'I':			/* linefeed reverse */
	ScrollDown(C, 0, MaxRow);
	break;
    case 'P':			/* NOT VT52. insert/delete line */
	C->state = ST_ESCP;
	return;
    case 'Q':			/* NOT VT52. My own additions. Attributes... */
	C->state = ST_ESCQ;
	return;
    case 'Y':			/* cursor move */
	C->state = ST_ESCY;
	return;

#if 0

    case 'Z':			/* identify */
	/* Problem here */
	AddQueue(27);
	AddQueue('Z');
	break;

#endif

    }
    C->state = ST_NORMAL;
}

void Vt52CmdEx(register Console * C, char c)
{
    switch (C->state) {
    case ST_ESCY:
	C->tmp = (unsigned char) (c - ' ');
	C->state = ST_ESCY2;
	return;
    case ST_ESCY2:
	Console_gotoxy(C, c - ' ', C->tmp);
	break;
    case ST_ESCP:
	switch (c) {
	case 'f':		/* insert line at crsr */
	    ScrollDown(C, C->cy, MaxRow);
	    break;
	case 'd':		/* delete line at crsr */
	    ScrollUp(C, C->cy + 1, Height);
	    break;
	}
	break;
    case ST_ESCQ:
	c -= ' ';
	if (c > 0 && c < MAX_ATTR)
	    C->attr |= AttrArry[c - 1];
	if (c == 0)
	    C->attr = A_DEFAULT;
	break;
    }
    C->state = ST_NORMAL;
}

#endif

#ifdef CONFIG_DCON_ANSI

static int parm1(unsigned char *buf)
{
    register char *np;

    np = (char *) atoi((char *) buf);
    return (np) ? ((int) np) : 1;

}

static int parm2(register unsigned char *buf)
{
    while (*buf != ';' && *buf)
	buf++;
    if (*buf)
	buf++;
    return parm1(buf);
}

static void AnsiCmd(register Console * C, char c)
{
    register unsigned char *p;
    int n;

    switch (c) {
    case 's':			/* Save the current location */
	C->savex = C->cx;
	C->savey = C->cy;
	break;
    case 'u':			/* Restore saved location */
	C->cx = C->savex;
	C->cy = C->savey;
	break;
    case 'A':			/* Move up n lines */
	n = parm1(C->params);
	C->cy -= n;
	if (C->cy < 0)
	    C->cy = 0;
	break;
    case 'B':			/* Move down n lines */
	n = parm1(C->params);
	C->cy += n;
	if (C->cy > MaxRow)
	    C->cy = MaxRow;
	break;
    case 'C':			/* Move right n characters */
	n = parm1(C->params);
	C->cx += n;
	if (C->cx > MaxCol)
	    C->cx = MaxCol;
	break;
    case 'D':			/* Move left n characters */
	n = parm1(C->params);
	C->cx -= n;
	if (C->cx < 0)
	    C->cx = 0;
	break;
    case 'H':			/* cursor move */
	Console_gotoxy(C, parm2(C->params) - 1, parm1(C->params) - 1);
	break;
    case 'J':			/* erase */
	if (parm1(C->params) == 2)
	    ClearRange(C, 0, 0, Width, Height);
	break;
    case 'K':			/* Clear to EOL */
	ClearRange(C, C->cx, C->cy, Width, C->cy);
	break;
    case 'm':			/* ansi color */
	C->state = ST_NORMAL;
	p = C->params;
	for (;;) {
	    switch (*p++) {
	    case 'm':
		if (p == &C->params[1])
		    C->attr = A_DEFAULT;
		return;
	    case ';':
		continue;
	    case '0':
		C->attr = A_DEFAULT;
		continue;
	    case '1':
		C->attr |= A_BOLD;
		continue;
	    case '5':
		C->attr |= A_BLINK;
		continue;
	    case '7':
		C->attr = A_REVERSE;
		continue;
	    case '8':
		C->attr = A_BLANK;
		continue;

	    case '3':
		if (*p >= '0' && *p <= '7') {
		    C->attr &= 0xf8;
		    C->attr |= *p++ & 0x07;
		    continue;
		}
		C->attr = A_DEFAULT;
		return;
	    case '4':
		if (*p >= '0' && *p <= '7') {
		    C->attr &= 0x8f;
		    C->attr |= (*p++ << 4) & 0x70;
		    continue;
		}
		/* fall thru */
	    default:
		C->attr = A_DEFAULT;
		return;
	    }
	}
	break;
    }
    C->state = ST_NORMAL;
}

#endif

/* This also tells the keyboard driver which tty to direct it's output to...
 * CAUTION: It *WILL* break if the console driver doesn't get tty0-X. 
 */

void Console_set_vc(unsigned int N)
{
    register char *CCBasep;
    unsigned int offset;

    if ((N >= NumConsoles)
	|| (Visible == &Con[N])
	|| (glock))
	return;
    Visible = &Con[N];

#if 0
    far_memmove(Visible->vseg, 0, VideoSeg, 0, (Width * Height) << 1);
#endif

    CCBasep = (char *) CCBase;
    offset = N * PageSize >> 1;
    outw((unsigned short int) ((offset & 0xff00) | 0x0c), CCBasep);
    outw((unsigned short int) (((offset & 0xff) << 8) | 0x0d), CCBasep);
    PositionCursor(Visible);
    Current_VCminor = (int) N;
}

#ifdef CONFIG_DCON_VT52

static void Console_gotoxy(register Console * C, int x, int y)
{
    {
	register char *xp = (char *)x;
	C->cx = ((((int) xp) >= MaxCol) ? MaxCol : ((((int)xp) < 0) ? 0 : (int)x));
    }
    {
	register char *yp = (char *)y;
	C->cy = ((((int) yp) >= MaxRow) ? MaxRow : ((((int)yp) < 0) ? 0 : (int)y));
    }
}

#endif

int Console_ioctl(register struct tty *tty, int cmd, char *arg)
{
    switch (cmd) {
    case DCGET_GRAPH:
	if (!glock) {
	    glock = &Con[tty->minor];
	    return 0;
	}
	return -EBUSY;
    case DCREL_GRAPH:
	if (glock == &Con[tty->minor]) {
	    glock = NULL;
	    wake_up(&glock_wait);
	    return 0;
	}
	break;
    case DCSET_KRAW:
	if (glock == &Con[tty->minor]) {
	    kraw = 1;
	    return 0;
	}
	break;
    case DCREL_KRAW:
	if ((glock == &Con[tty->minor]) && (kraw == 1)) {
	    kraw = 0;
	    return 1;
	}
/*  	break; */
    }

    return -EINVAL;
}

int Console_write(register struct tty *tty)
{
    register Console *C = &Con[tty->minor];
    int cnt = 0;
    unsigned char ch;

    while (tty->outq.len != 0) {
	chq_getch(&tty->outq, &ch, 0);
	WriteChar(C, (char) ch);
	cnt++;
    }
    PositionCursor(C);
    return cnt;
}

void Console_release(struct tty *tty)
{
/* Do nothing */
}

int Console_open(struct tty *tty)
{
    return (tty->minor >= NumConsoles) ? -ENODEV : 0;
}

/*@-type@*/

struct tty_ops dircon_ops = {
    Console_open,
    Console_release,
    Console_write,
    NULL,
    Console_ioctl,
};

/*@+type@*/

void init_console(void)
{
    register Console *C;

    MaxCol = (Width = peekb(0x40, 0x4a)) - 1;

    /* Trust this. Cga does not support peeking at 0x40:0x84. */
    MaxRow = (Height = 25) - 1;
    CCBase = (void *) peekw(0x40, 0x63);
    PageSize = (unsigned int) peekw(0x40, 0x4C);

    VideoSeg = 0xb800;
    if (peekb(0x40, 0x49) == 7) {
	VideoSeg = 0xB000;
	NumConsoles = 1;
    }

    {
	register char *pi;
	for (pi = 0; ((unsigned int)pi) < NumConsoles; pi++) {
	    C = &Con[(unsigned int)pi];
	    C->cx = C->cy = 0;
	    C->state = ST_NORMAL;
	    C->vseg = VideoSeg + (PageSize >> 4) * ((unsigned int)pi);
	    C->pageno = (int) pi;
	    C->attr = A_DEFAULT;

#ifdef CONFIG_DCON_ANSI

	    C->savex = C->savey = 0;

#endif

	    if (pi)
		ClearRange(C, 0, 0, Width, Height);
	}
    }

    C = &Con[0];
    C->cx = peekb(0x40, 0x50);
    C->cy = peekb(0x40, 0x51);
    Visible = C;

#ifdef CONFIG_DCON_VT52

    printk("Console: Direct %ux%u emulating vt52 (%u virtual consoles)\n",
	   Width, Height, NumConsoles);

#else

    printk("Console: Direct %ux%u dumb (%u virtual consoles)\n",
	   Width, Height, NumConsoles);

#endif
}

#endif
