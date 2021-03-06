/* Copyright (c) 2008, 2009
 *      Juergen Weigert (jnweiger@immd4.informatik.uni-erlangen.de)
 *      Michael Schroeder (mlschroe@immd4.informatik.uni-erlangen.de)
 *      Micah Cowan (micah@cowan.name)
 *      Sadrul Habib Chowdhury (sadrul@users.sourceforge.net)
 * Copyright (c) 1993-2002, 2003, 2005, 2006, 2007
 *      Juergen Weigert (jnweiger@immd4.informatik.uni-erlangen.de)
 *      Michael Schroeder (mlschroe@immd4.informatik.uni-erlangen.de)
 * Copyright (c) 1987 Oliver Laumann
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program (see the file COPYING); if not, see
 * http://www.gnu.org/licenses/, or contact Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02111-1301  USA
 *
 ****************************************************************
 */

#include "config.h"

#include "ansi.h"

#include <sys/types.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#include "screen.h"
#include "winmsg.h"

#include "encoding.h"
#include "fileio.h"
#include "help.h"
#include "logfile.h"
#include "mark.h"
#include "misc.h"
#include "process.h"
#include "resize.h"

/* widths for Z0/Z1 switching */
const int Z0width = 132;
const int Z1width = 80;

/* globals set in WriteString */
static Window *curr;	/* window we are working on */
static int rows, cols;		/* window size of the curr window */

bool use_altscreen = false;	/* enable alternate screen support? */
bool use_hardstatus = true;	/* display status line in hs */
bool visual_bell = 0;

char *printcmd = 0;

uint32_t *blank;		/* line filled with spaces */
uint32_t *null;			/* line filled with '\0' */

struct mline mline_old;
struct mline mline_blank;
struct mline mline_null;

struct mchar mchar_null;
struct mchar mchar_blank = { ' ', 0, 0, 0, 0, 0, 0 };
struct mchar mchar_so = { ' ', A_SO, 0, 0, 0, 0, 0};

uint64_t renditions[NUM_RENDS] = { 65529 /* =ub */ , 65531 /* =b */ , 65533 /* =u */  };

/* keep string_t and string_t_string in sync! */
static char *string_t_string[] = {
	"NONE",
	"DCS",			/* Device control string */
	"OSC",			/* Operating system command */
	"APC",			/* Application program command */
	/*  - used for status change */
	"PM",			/* Privacy message */
	"AKA",			/* title for current screen */
	"GM",			/* Global message to every display */
	"STATUS"		/* User hardstatus line */
};

/* keep state_t and state_t_string in sync! */
static char *state_t_string[] = {
	"LIT",			/* Literal input */
	"ESC",			/* Start of escape sequence */
	"ASTR",			/* Start of control string */
	"STRESC",		/* ESC seen in control string */
	"CSI",			/* Reading arguments in "CSI Pn ;... */
	"PRIN",			/* Printer mode */
	"PRINESC",		/* ESC seen in printer mode */
	"PRINCSI",		/* CSI seen in printer mode */
	"PRIN4"			/* CSI 4 seen in printer mode */
};

static int Special(int);
static void DoESC(int, int);
static void DoCSI(int, int);
static void StringStart(enum string_t);
static void StringChar(int);
static int StringEnd(void);
static void PrintStart(void);
static void PrintChar(int);
static void PrintFlush(void);
static void DesignateCharset(int, int);
static void MapCharset(int);
static void MapCharsetR(int);
static void SaveCursor(struct cursor *);
static void RestoreCursor(struct cursor *);
static void BackSpace(void);
static void Return(void);
static void LineFeed(int);
static void ReverseLineFeed(void);
static void InsertChar(int);
static void DeleteChar(int);
static void DeleteLine(int);
static void InsertLine(int);
static void Scroll(char *, int, int, char *);
static void ForwardTab(void);
static void BackwardTab(void);
static void ClearScreen(void);
static void ClearFromBOS(void);
static void ClearToEOS(void);
static void ClearLineRegion(int, int);
static void CursorRight(int);
static void CursorUp(int);
static void CursorDown(int);
static void CursorLeft(int);
static void ASetMode(bool);
static void SelectRendition(void);
static void RestorePosRendition(void);
static void FillWithEs(void);
static void FindAKA(void);
static void Report(char *, int, int);
static void ScrollRegion(int);
static void WAddLineToHist(Window *, struct mline *);
static void WLogString(Window *, char *, size_t);
static void WReverseVideo(Window *, int);
static void MFixLine(Window *, int, struct mchar *);
static void MScrollH(Window *, int, int, int, int, int);
static void MScrollV(Window *, int, int, int, int);
static void MClearArea(Window *, int, int, int, int, int);
static void MInsChar(Window *, struct mchar *, int, int);
static void MPutChar(Window *, struct mchar *, int, int);
static void MWrapChar(Window *, struct mchar *, int, int, int, bool);
static void MBceLine(Window *, int, int, int, int);

void ResetAnsiState(Window *win)
{
	win->w_state = LIT;
	win->w_StringType = NONE;
}

/* adds max 22 bytes */
int GetAnsiStatus(Window *win, char *buf)
{
	char *p = buf;

	if (win->w_state == LIT)
		return 0;

	strcpy(p, state_t_string[win->w_state]);
	p += strlen(p);
	if (win->w_intermediate) {
		*p++ = '-';
		if (win->w_intermediate > 0xff)
			p += AddXChar(p, win->w_intermediate >> 8);
		p += AddXChar(p, win->w_intermediate & 0xff);
		*p = 0;
	}
	if (win->w_state == ASTR || win->w_state == STRESC)
		sprintf(p, "-%s", string_t_string[win->w_StringType]);
	p += strlen(p);
	return p - buf;
}

void ResetCharsets(Window *win)
{
	win->w_gr = nwin_default.gr;
	win->w_c1 = nwin_default.c1;
	SetCharsets(win, "BBBB02");
	if (nwin_default.charset)
		SetCharsets(win, nwin_default.charset);
	ResetEncoding(win);
}

void SetCharsets(Window *win, char *s)
{
	for (int i = 0; i < 4 && *s; i++, s++)
		if (*s != '.')
			win->w_charsets[i] = ((*s == 'B') ? ASCII : *s);
	if (*s && *s++ != '.')
		win->w_Charset = s[-1] - '0';
	if (*s && *s != '.')
		win->w_CharsetR = *s - '0';
	win->w_ss = 0;
	win->w_FontL = win->w_charsets[win->w_Charset];
	win->w_FontR = win->w_charsets[win->w_CharsetR];
}

/*****************************************************************/

/*
 *  Here comes the vt100 emulator
 *  - writes logfiles,
 *  - sets timestamp and flags activity in window.
 *  - record program output in window scrollback
 *  - translate program output for the display and put it into the obuf.
 *
 */
void WriteString(Window *win, char *buf, size_t len)
{
	int c;
	int font;
	Canvas *cv;

	if (len == 0)
		return;
	if (win->w_log)
		WLogString(win, buf, len);

	/* set global variables (yuck!) */
	curr = win;
	cols = curr->w_width;
	rows = curr->w_height;

	if (curr->w_silence)
		SetTimeout(&curr->w_silenceev, curr->w_silencewait * 1000);

	if (curr->w_monitor == MON_ON) {
		curr->w_monitor = MON_FOUND;
	}

	if (cols > 0 && rows > 0) {
		do {
			c = (unsigned char)*buf++;
			if (!curr->w_mbcs)
				curr->w_rend.font = curr->w_FontL;	/* Default: GL */

			if (curr->w_encoding == UTF8) {
				c = FromUtf8(c, &curr->w_decodestate);
				if (c == -1)
					continue;
				if (c == -2) {
					c = UCS_REPL;
					/* try char again */
					buf--;
					len++;
				}
			}

 tryagain:
			switch (curr->w_state) {
			case PRIN:
				switch (c) {
				case '\033':
					curr->w_state = PRINESC;
					break;
				default:
					PrintChar(c);
				}
				break;
			case PRINESC:
				switch (c) {
				case '[':
					curr->w_state = PRINCSI;
					break;
				default:
					PrintChar('\033');
					PrintChar(c);
					curr->w_state = PRIN;
				}
				break;
			case PRINCSI:
				switch (c) {
				case '4':
					curr->w_state = PRIN4;
					break;
				default:
					PrintChar('\033');
					PrintChar('[');
					PrintChar(c);
					curr->w_state = PRIN;
				}
				break;
			case PRIN4:
				switch (c) {
				case 'i':
					curr->w_state = LIT;
					PrintFlush();
					if (curr->w_pdisplay && curr->w_pdisplay->d_printfd >= 0) {
						close(curr->w_pdisplay->d_printfd);
						curr->w_pdisplay->d_printfd = -1;
					}
					curr->w_pdisplay = 0;
					break;
				default:
					PrintChar('\033');
					PrintChar('[');
					PrintChar('4');
					PrintChar(c);
					curr->w_state = PRIN;
				}
				break;
			case ASTR:
				if (c == 0)
					break;
				if (c == '\033') {
					curr->w_state = STRESC;
					break;
				}
				/* special xterm hack: accept SetStatus sequence. Yucc! */
				/* allow ^E for title escapes */
				if (!(curr->w_StringType == OSC && c < ' ' && c != '\005'))
					if (!curr->w_c1 || c != ('\\' ^ 0xc0)) {
						StringChar(c);
						break;
					}
				c = '\\';
				/* FALLTHROUGH */
			case STRESC:
				switch (c) {
				case '\\':
					if (StringEnd() == 0 || len <= 1)
						break;
					/* check if somewhere a status is displayed */
					for (cv = curr->w_layer.l_cvlist; cv; cv = cv->c_lnext) {
						display = cv->c_display;
						if (D_status == STATUS_ON_WIN)
							break;
					}
					if (cv) {
						if (len > IOSIZE + 1)
							len = IOSIZE + 1;
						curr->w_outlen = len - 1;
						memmove(curr->w_outbuf, buf, len - 1);
						return;	/* wait till status is gone */
					}
					break;
				case '\033':
					StringChar('\033');
					break;
				default:
					curr->w_state = ASTR;
					StringChar('\033');
					StringChar(c);
					break;
				}
				break;
			case ESC:
				switch (c) {
				case '[':
					curr->w_NumArgs = 0;
					curr->w_intermediate = 0;
					memset((char *)curr->w_args, 0, MAXARGS * sizeof(int));
					curr->w_state = CSI;
					break;
				case ']':
					StringStart(OSC);
					break;
				case '_':
					StringStart(APC);
					break;
				case 'P':
					StringStart(DCS);
					break;
				case '^':
					StringStart(PM);
					break;
				case '!':
					StringStart(GM);
					break;
				case '"':
				case 'k':
					StringStart(AKA);
					break;
				default:
					if (Special(c)) {
						curr->w_state = LIT;
						break;
					}
					if (c >= ' ' && c <= '/') {
						if (curr->w_intermediate) {
							if (curr->w_intermediate == '$')
								c |= '$' << 8;
							else
								c = -1;
						}
						curr->w_intermediate = c;
					} else if (c >= '0' && c <= '~') {
						DoESC(c, curr->w_intermediate);
						curr->w_state = LIT;
					} else {
						curr->w_state = LIT;
						goto tryagain;
					}
				}
				break;
			case CSI:
				switch (c) {
				case '0':
				case '1':
				case '2':
				case '3':
				case '4':
				case '5':
				case '6':
				case '7':
				case '8':
				case '9':
					if (curr->w_NumArgs >= 0 && curr->w_NumArgs < MAXARGS) {
						if (curr->w_args[curr->w_NumArgs] < 100000000)
							curr->w_args[curr->w_NumArgs] =
							    10 * curr->w_args[curr->w_NumArgs] + (c - '0');
					}
					break;
				case ';':
				case ':':
					if (curr->w_NumArgs < MAXARGS)
						curr->w_NumArgs++;
					break;
				default:
					if (Special(c))
						break;
					if (c >= '@' && c <= '~') {
						if (curr->w_NumArgs < MAXARGS)
							curr->w_NumArgs++;
						DoCSI(c, curr->w_intermediate);
						if (curr->w_state != PRIN)
							curr->w_state = LIT;
					} else if ((c >= ' ' && c <= '/') || (c >= '<' && c <= '?'))
						curr->w_intermediate = curr->w_intermediate ? -1 : c;
					else {
						curr->w_state = LIT;
						goto tryagain;
					}
				}
				break;
			case LIT:
			default:
				if (curr->w_mbcs)
					if (c <= ' ' || c == 0x7f || (c >= 0x80 && c < 0xa0 && curr->w_c1))
						curr->w_mbcs = 0;
				if (c < ' ') {
					if (c == '\033') {
						curr->w_intermediate = 0;
						curr->w_state = ESC;
						if (curr->w_autoaka < 0)
							curr->w_autoaka = 0;
					} else
						Special(c);
					break;
				}
				if (c >= 0x80 && c < 0xa0 && curr->w_c1)
					if ((curr->w_FontR & 0xf0) != 0x20 || curr->w_encoding == UTF8) {
						switch (c) {
						case 0xc0 ^ 'D':
						case 0xc0 ^ 'E':
						case 0xc0 ^ 'H':
						case 0xc0 ^ 'M':
						case 0xc0 ^ 'N':	/* SS2 */
						case 0xc0 ^ 'O':	/* SS3 */
							DoESC(c ^ 0xc0, 0);
							break;
						case 0xc0 ^ '[':
							if (curr->w_autoaka < 0)
								curr->w_autoaka = 0;
							curr->w_NumArgs = 0;
							curr->w_intermediate = 0;
							memset((char *)curr->w_args, 0, MAXARGS * sizeof(int));
							curr->w_state = CSI;
							break;
						case 0xc0 ^ 'P':
							StringStart(DCS);
							break;
						default:
							break;
						}
						break;
					}

				if (!curr->w_mbcs) {
					if (c < 0x80 || curr->w_gr == 0)
						curr->w_rend.font = curr->w_FontL;
					else if (curr->w_gr == 2 && !curr->w_ss)
						curr->w_rend.font = curr->w_FontE;
					else
						curr->w_rend.font = curr->w_FontR;
				}
				if (curr->w_encoding == UTF8) {
					if (curr->w_rend.font == '0') {
						struct mchar mc, *mcp;

						mc.image = c;
						mc.mbcs = 0;
						mc.font = '0';
						mc.fontx = 0;
						mcp = recode_mchar(&mc, 0, UTF8);
						c = mcp->image | mcp->font << 8;
					}
					curr->w_rend.font = 0;
				}
				if (curr->w_encoding == UTF8 && utf8_isdouble(c))
					curr->w_mbcs = 0xff;
				if (curr->w_encoding == UTF8 && c >= 0x0300 && utf8_iscomb(c)) {
					int ox, oy;
					struct mchar omc;

					ox = curr->w_x - 1;
					oy = curr->w_y;
					if (ox < 0) {
						ox = curr->w_width - 1;
						oy--;
					}
					if (oy < 0)
						oy = 0;
					copy_mline2mchar(&omc, &curr->w_mlines[oy], ox);
					if (omc.image == 0xff && omc.font == 0xff && omc.fontx == 0) {
						ox--;
						if (ox >= 0) {
							copy_mline2mchar(&omc, &curr->w_mlines[oy], ox);
							omc.mbcs = 0xff;
						}
					}
					if (ox >= 0) {
						utf8_handle_comb(c, &omc);
						MFixLine(curr, oy, &omc);
						copy_mchar2mline(&omc, &curr->w_mlines[oy], ox);
						LPutChar(&curr->w_layer, &omc, ox, oy);
						LGotoPos(&curr->w_layer, curr->w_x, curr->w_y);
					}
					break;
				}
				font = curr->w_rend.font;
				if (font == KANA && curr->w_encoding == SJIS && curr->w_mbcs == 0) {
					/* Lets see if it is the first byte of a kanji */
					if ((0x81 <= c && c <= 0x9f) || (0xe0 <= c && c <= 0xef)) {
						curr->w_mbcs = c;
						break;
					}
				}
				if (font == 031 && c == 0x80 && !curr->w_mbcs)
					font = curr->w_rend.font = 0;
				if (is_dw_font(font) && c == ' ')
					font = curr->w_rend.font = 0;
				if (is_dw_font(font) || curr->w_mbcs) {
					int t = c;
					if (curr->w_mbcs == 0) {
						curr->w_mbcs = c;
						break;
					}
					if (curr->w_x == cols - 1) {
						curr->w_x += curr->w_wrap ? true : false;
					}
					if (curr->w_encoding != UTF8) {
						c = curr->w_mbcs;
						if (font == KANA && curr->w_encoding == SJIS) {
							/*
							 * SJIS -> EUC mapping:
							 *   First byte:
							 *     81,82...9f -> 21,23...5d
							 *     e0,e1...ef -> 5f,61...7d
							 *   Second byte:
							 *     40-7e -> 21-5f
							 *     80-9e -> 60-7e
							 *     9f-fc -> 21-7e (increment first byte!)
							 */
							if (0x40 <= t && t <= 0xfc && t != 0x7f) {
								if (c <= 0x9f)
									c = (c - 0x81) * 2 + 0x21;
								else
									c = (c - 0xc1) * 2 + 0x21;
								if (t <= 0x7e)
									t -= 0x1f;
								else if (t <= 0x9e)
									t -= 0x20;
								else
									t -= 0x7e, c++;
								curr->w_rend.font = KANJI;
							} else {
								/* Incomplete shift-jis - skip first byte */
								c = t;
								t = 0;
							}
						}
						if (t && curr->w_gr && font != 030 && font != 031) {
							t &= 0x7f;
							if (t < ' ')
								goto tryagain;
						}
						if (t == '\177')
							break;
						curr->w_mbcs = t;
					}
				}
				if (font == '<' && c >= ' ') {
					curr->w_rend.font = 0;
					c |= 0x80;
				} else if (curr->w_gr && curr->w_encoding != UTF8) {
					if (c == 0x80 && font == 0 && curr->w_encoding == GBK)
						c = 0xa4;
					else
						c &= 0x7f;
					if (c < ' ' && font != 031)
						goto tryagain;
				}
				if (c == '\177')
					break;
				curr->w_rend.image = c;
				if (curr->w_encoding == UTF8) {
					curr->w_rend.font = c >> 8;
					curr->w_rend.fontx = c >> 16;
				}
				curr->w_rend.mbcs = curr->w_mbcs;
				if (curr->w_x < cols - 1) {
					if (curr->w_insert) {
						save_mline(&curr->w_mlines[curr->w_y], cols);
						MInsChar(curr, &curr->w_rend, curr->w_x, curr->w_y);
						LInsChar(&curr->w_layer, &curr->w_rend, curr->w_x, curr->w_y,
							 &mline_old);
						curr->w_x++;
					} else {
						MPutChar(curr, &curr->w_rend, curr->w_x, curr->w_y);
						LPutChar(&curr->w_layer, &curr->w_rend, curr->w_x, curr->w_y);
						curr->w_x++;
					}
				} else if (curr->w_x == cols - 1) {
					MPutChar(curr, &curr->w_rend, curr->w_x, curr->w_y);
					LPutChar(&curr->w_layer, &curr->w_rend, curr->w_x, curr->w_y);
					if (curr->w_wrap)
						curr->w_x++;
				} else {
					MWrapChar(curr, &curr->w_rend, curr->w_y, curr->w_top, curr->w_bot,
						  curr->w_insert);
					LWrapChar(&curr->w_layer, &curr->w_rend, curr->w_y, curr->w_top, curr->w_bot,
						  curr->w_insert);
					if (curr->w_y != curr->w_bot && curr->w_y != curr->w_height - 1)
						curr->w_y++;
					curr->w_x = 1;
				}
				if (curr->w_mbcs) {
					curr->w_rend.mbcs = curr->w_mbcs = 0;
					curr->w_x++;
				}
				if (curr->w_ss) {
					curr->w_FontL = curr->w_charsets[curr->w_Charset];
					curr->w_FontR = curr->w_charsets[curr->w_CharsetR];
					curr->w_rend.font = curr->w_FontL;
					LSetRendition(&curr->w_layer, &curr->w_rend);
					curr->w_ss = 0;
				}
				break;
			}
		}
		while (--len);
	}
	if (!printcmd && curr->w_state == PRIN)
		PrintFlush();
}

static void WLogString(Window *win, char *buf, size_t len)
{
	if (!win->w_log)
		return;
	if (logtstamp_on && win->w_logsilence >= logtstamp_after * 2) {
		char *t = MakeWinMsg(logtstamp_string, win, '%');
		logfwrite(win->w_log, t, strlen(t));	/* long time no write */
	}
	win->w_logsilence = 0;
	if (logfwrite(win->w_log, buf, len) < 1) {
		WMsg(win, errno, "Error writing logfile");
		logfclose(win->w_log);
		win->w_log = 0;
	}
	if (!log_flush)
		logfflush(win->w_log);
}

static int Special(int c)
{
	switch (c) {
	case '\b':
		BackSpace();
		return 1;
	case '\r':
		Return();
		return 1;
	case '\n':
		if (curr->w_autoaka)
			FindAKA();
	case '\013':		/* Vertical tab is the same as Line Feed */
		LineFeed(0);
		return 1;
	case '\007':
		WBell(curr, visual_bell);
		return 1;
	case '\t':
		ForwardTab();
		return 1;
	case '\017':		/* SI */
		MapCharset(G0);
		return 1;
	case '\016':		/* SO */
		MapCharset(G1);
		return 1;
	}
	return 0;
}

static void DoESC(int c, int intermediate)
{
	switch (intermediate) {
	case 0:
		switch (c) {
		case 'E':
			LineFeed(1);
			break;
		case 'D':
			LineFeed(0);
			break;
		case 'M':
			ReverseLineFeed();
			break;
		case 'H':
			curr->w_tabs[curr->w_x] = 1;
			break;
		case 'Z':	/* jph: Identify as VT100 */
			Report("\033[?%d;%dc", 1, 2);
			break;
		case '7':
			SaveCursor(&curr->w_saved);
			break;
		case '8':
			RestoreCursor(&curr->w_saved);
			break;
		case 'c':
			ClearScreen();
			ResetWindow(curr);
			LKeypadMode(&curr->w_layer, 0);
			LCursorkeysMode(&curr->w_layer, 0);
#ifndef TIOCPKT
			WNewAutoFlow(curr, 1);
#endif
			/* XXX
			   SetRendition(&mchar_null);
			   InsertMode(false);
			   ChangeScrollRegion(0, rows - 1);
			 */
			LGotoPos(&curr->w_layer, curr->w_x, curr->w_y);
			break;
		case '=':
			LKeypadMode(&curr->w_layer, curr->w_keypad = 1);
#ifndef TIOCPKT
			WNewAutoFlow(curr, 0);
#endif				/* !TIOCPKT */
			break;
		case '>':
			LKeypadMode(&curr->w_layer, curr->w_keypad = 0);
#ifndef TIOCPKT
			WNewAutoFlow(curr, 1);
#endif				/* !TIOCPKT */
			break;
		case 'n':	/* LS2 */
			MapCharset(G2);
			break;
		case 'o':	/* LS3 */
			MapCharset(G3);
			break;
		case '~':
			MapCharsetR(G1);	/* LS1R */
			break;
			/* { */
		case '}':
			MapCharsetR(G2);	/* LS2R */
			break;
		case '|':
			MapCharsetR(G3);	/* LS3R */
			break;
		case 'N':	/* SS2 */
			if (curr->w_charsets[curr->w_Charset] != curr->w_charsets[G2]
			    || curr->w_charsets[curr->w_CharsetR] != curr->w_charsets[G2])
				curr->w_FontR = curr->w_FontL = curr->w_charsets[curr->w_ss = G2];
			else
				curr->w_ss = 0;
			break;
		case 'O':	/* SS3 */
			if (curr->w_charsets[curr->w_Charset] != curr->w_charsets[G3]
			    || curr->w_charsets[curr->w_CharsetR] != curr->w_charsets[G3])
				curr->w_FontR = curr->w_FontL = curr->w_charsets[curr->w_ss = G3];
			else
				curr->w_ss = 0;
			break;
		case 'g':	/* VBELL, private screen sequence */
			WBell(curr, true);
			break;
		}
		break;
	case '#':
		switch (c) {
		case '8':
			FillWithEs();
			break;
		}
		break;
	case '(':
		DesignateCharset(c, G0);
		break;
	case ')':
		DesignateCharset(c, G1);
		break;
	case '*':
		DesignateCharset(c, G2);
		break;
	case '+':
		DesignateCharset(c, G3);
		break;
/*
 * ESC $ ( Fn: invoke multi-byte charset, Fn, to G0
 * ESC $ Fn: same as above.  (old sequence)
 * ESC $ ) Fn: invoke multi-byte charset, Fn, to G1
 * ESC $ * Fn: invoke multi-byte charset, Fn, to G2
 * ESC $ + Fn: invoke multi-byte charset, Fn, to G3
 */
	case '$':
	case '$' << 8 | '(':
		DesignateCharset(c & 037, G0);
		break;
	case '$' << 8 | ')':
		DesignateCharset(c & 037, G1);
		break;
	case '$' << 8 | '*':
		DesignateCharset(c & 037, G2);
		break;
	case '$' << 8 | '+':
		DesignateCharset(c & 037, G3);
		break;
	}
}

static void DoCSI(int c, int intermediate)
{
	int i, a1 = curr->w_args[0], a2 = curr->w_args[1];

	if (curr->w_NumArgs > MAXARGS)
		curr->w_NumArgs = MAXARGS;
	switch (intermediate) {
	case 0:
		switch (c) {
		case 'H':
		case 'f':
			if (a1 < 1)
				a1 = 1;
			if (curr->w_origin)
				a1 += curr->w_top;
			if (a1 > rows)
				a1 = rows;
			if (a2 < 1)
				a2 = 1;
			if (a2 > cols)
				a2 = cols;
			LGotoPos(&curr->w_layer, --a2, --a1);
			curr->w_x = a2;
			curr->w_y = a1;
			if (curr->w_autoaka)
				curr->w_autoaka = a1 + 1;
			break;
		case 'J':
			if (a1 < 0 || a1 > 2)
				a1 = 0;
			switch (a1) {
			case 0:
				ClearToEOS();
				break;
			case 1:
				ClearFromBOS();
				break;
			case 2:
				ClearScreen();
				LGotoPos(&curr->w_layer, curr->w_x, curr->w_y);
				break;
			}
			break;
		case 'K':
			if (a1 < 0 || a1 > 2)
				a1 %= 3;
			switch (a1) {
			case 0:
				ClearLineRegion(curr->w_x, cols - 1);
				break;
			case 1:
				ClearLineRegion(0, curr->w_x);
				break;
			case 2:
				ClearLineRegion(0, cols - 1);
				break;
			}
			break;
		case 'X':
			a1 = curr->w_x + (a1 ? a1 - 1 : 0);
			ClearLineRegion(curr->w_x, a1 < cols ? a1 : cols - 1);
			break;
		case 'A':
			CursorUp(a1 ? a1 : 1);
			break;
		case 'B':
			CursorDown(a1 ? a1 : 1);
			break;
		case 'C':
			CursorRight(a1 ? a1 : 1);
			break;
		case 'D':
			CursorLeft(a1 ? a1 : 1);
			break;
		case 'E':
			curr->w_x = 0;
			CursorDown(a1 ? a1 : 1);	/* positions cursor */
			break;
		case 'F':
			curr->w_x = 0;
			CursorUp(a1 ? a1 : 1);	/* positions cursor */
			break;
		case 'G':
		case '`':	/* HPA */
			curr->w_x = a1 ? a1 - 1 : 0;
			if (curr->w_x >= cols)
				curr->w_x = cols - 1;
			LGotoPos(&curr->w_layer, curr->w_x, curr->w_y);
			break;
		case 'd':	/* VPA */
			curr->w_y = a1 ? a1 - 1 : 0;
			if (curr->w_y >= rows)
				curr->w_y = rows - 1;
			LGotoPos(&curr->w_layer, curr->w_x, curr->w_y);
			break;
		case 'm':
			SelectRendition();
			break;
		case 'g':
			if (a1 == 0)
				curr->w_tabs[curr->w_x] = 0;
			else if (a1 == 3)
				memset(curr->w_tabs, 0, cols);
			break;
		case 'r':
			if (!a1)
				a1 = 1;
			if (!a2)
				a2 = rows;
			if (a1 < 1 || a2 > rows || a1 >= a2)
				break;
			curr->w_top = a1 - 1;
			curr->w_bot = a2 - 1;
			/* ChangeScrollRegion(curr->w_top, curr->w_bot); */
			if (curr->w_origin) {
				curr->w_y = curr->w_top;
				curr->w_x = 0;
			} else
				curr->w_y = curr->w_x = 0;
			LGotoPos(&curr->w_layer, curr->w_x, curr->w_y);
			break;
		case 's':
			SaveCursor(&curr->w_saved);
			break;
		case 't':
			switch (a1) {
			case 11:
				if (curr->w_layer.l_cvlist)
					Report("\033[1t", 0, 0);
				else
					Report("\033[2t", 0, 0);
				break;
			case 7:
				LRefreshAll(&curr->w_layer, 0);
				break;
			case 21:
				a1 = strlen(curr->w_title);
				if ((unsigned)(curr->w_inlen + 5 + a1) <= sizeof(curr->w_inbuf)) {
					memmove(curr->w_inbuf + curr->w_inlen, "\033]l", 3);
					memmove(curr->w_inbuf + curr->w_inlen + 3, curr->w_title, a1);
					memmove(curr->w_inbuf + curr->w_inlen + 3 + a1, "\033\\", 2);
					curr->w_inlen += 5 + a1;
				}
				break;
			case 8:
				a1 = curr->w_args[2];
				if (a1 < 1)
					a1 = curr->w_width;
				if (a2 < 1)
					a2 = curr->w_height;
				if (a1 > 10000 || a2 > 10000)
					break;
				WChangeSize(curr, a1, a2);
				cols = curr->w_width;
				rows = curr->w_height;
				break;
			default:
				break;
			}
			break;
		case 'u':
			RestoreCursor(&curr->w_saved);
			break;
		case 'I':
			if (!a1)
				a1 = 1;
			while (a1--)
				ForwardTab();
			break;
		case 'Z':
			if (!a1)
				a1 = 1;
			while (a1--)
				BackwardTab();
			break;
		case 'L':
			InsertLine(a1 ? a1 : 1);
			break;
		case 'M':
			DeleteLine(a1 ? a1 : 1);
			break;
		case 'P':
			DeleteChar(a1 ? a1 : 1);
			break;
		case '@':
			InsertChar(a1 ? a1 : 1);
			break;
		case 'h':
			ASetMode(true);
			break;
		case 'l':
			ASetMode(false);
			break;
		case 'i':	/* MC Media Control */
			if (a1 == 5)
				PrintStart();
			break;
		case 'n':
			if (a1 == 5)	/* Report terminal status */
				Report("\033[0n", 0, 0);
			else if (a1 == 6)	/* Report cursor position */
				Report("\033[%d;%dR", curr->w_y + 1, curr->w_x + 1);
			break;
		case 'c':	/* Identify as VT100 */
			if (a1 == 0)
				Report("\033[?%d;%dc", 1, 2);
			break;
		case 'x':	/* decreqtparm */
			if (a1 == 0 || a1 == 1)
				Report("\033[%d;1;1;112;112;1;0x", a1 + 2, 0);
			break;
		case 'p':	/* obscure code from a 97801 term */
			if (a1 == 6 || a1 == 7) {
				curr->w_curinv = 7 - a1;
				LCursorVisibility(&curr->w_layer, curr->w_curinv ? -1 : curr->w_curvvis);
			}
			break;
		case 'S':	/* code from a 97801 term / DEC vt400 */
			ScrollRegion(a1 ? a1 : 1);
			break;
		case 'T':	/* code from a 97801 term / DEC vt400 */
		case '^':	/* SD as per ISO 6429 */
			ScrollRegion(a1 ? -a1 : -1);
			break;
		}
		break;
	case ' ':
		if (c == 'q') {
			curr->w_cursorstyle = a1;
			LCursorStyle(&curr->w_layer, curr->w_cursorstyle);
		}
		break;
	case '?':
		for (a2 = 0; a2 < curr->w_NumArgs; a2++) {
			a1 = curr->w_args[a2];
			if (c != 'h' && c != 'l')
				break;
			i = (c == 'h');
			switch (a1) {
			case 1:	/* CKM:  cursor key mode */
				LCursorkeysMode(&curr->w_layer, curr->w_cursorkeys = i);
#ifndef TIOCPKT
				WNewAutoFlow(curr, !i);
#endif				/* !TIOCPKT */
				break;
			case 2:	/* ANM:  ansi/vt52 mode */
				if (i) {
					if (curr->w_encoding)
						break;
					curr->w_charsets[0] = curr->w_charsets[1] =
					    curr->w_charsets[2] = curr->w_charsets[3] =
					    curr->w_FontL = curr->w_FontR = ASCII;
					curr->w_Charset = 0;
					curr->w_CharsetR = 2;
					curr->w_ss = 0;
				}
				break;
			case 3:	/* COLM: column mode */
				i = (i ? Z0width : Z1width);
				ClearScreen();
				curr->w_x = 0;
				curr->w_y = 0;
				WChangeSize(curr, i, curr->w_height);
				cols = curr->w_width;
				rows = curr->w_height;
				break;
				/* case 4:        SCLM: scrolling mode */
			case 5:	/* SCNM: screen mode */
				if (i != curr->w_revvid)
					WReverseVideo(curr, i);
				curr->w_revvid = i;
				break;
			case 6:	/* OM:   origin mode */
				if ((curr->w_origin = i) != 0) {
					curr->w_y = curr->w_top;
					curr->w_x = 0;
				} else
					curr->w_y = curr->w_x = 0;
				LGotoPos(&curr->w_layer, curr->w_x, curr->w_y);
				break;
			case 7:	/* AWM:  auto wrap mode */
				curr->w_wrap = i;
				break;
				/* case 8:        ARM:  auto repeat mode */
				/* case 9:        INLM: interlace mode */
			case 9:	/* X10 mouse tracking */
				curr->w_mouse = i ? 9 : 0;
				LMouseMode(&curr->w_layer, curr->w_mouse);
				break;
				/* case 10:       EDM:  edit mode */
				/* case 11:       LTM:  line transmit mode */
				/* case 13:       SCFDM: space compression / field delimiting */
				/* case 14:       TEM:  transmit execution mode */
				/* case 16:       EKEM: edit key execution mode */
				/* case 18:       PFF:  Printer term form feed */
				/* case 19:       PEX:  Printer extend screen / scroll. reg */
			case 25:	/* TCEM: text cursor enable mode */
				curr->w_curinv = !i;
				LCursorVisibility(&curr->w_layer, curr->w_curinv ? -1 : curr->w_curvvis);
				break;
				/* case 34:       RLM:  Right to left mode */
				/* case 35:       HEBM: hebrew keyboard map */
				/* case 36:       HEM:  hebrew encoding */
				/* case 38:             TeK Mode */
				/* case 40:             132 col enable */
				/* case 42:       NRCM: 7bit NRC character mode */
				/* case 44:             margin bell enable */
			case 47:	/*       xterm-like alternate screen */
			case 1047:	/*       xterm-like alternate screen */
			case 1049:	/*       xterm-like alternate screen */
				if (use_altscreen) {
					if (i) {
						if (!curr->w_alt.on) {
							SaveCursor(&curr->w_alt.cursor);
							EnterAltScreen(curr);
						}
					} else {
						if (curr->w_alt.on) {
							RestoreCursor(&curr->w_alt.cursor);
							LeaveAltScreen(curr);
						}
					}
					if (a1 == 47 && !i)
						curr->w_saved.on = 0;
					LRefreshAll(&curr->w_layer, 0);
					LGotoPos(&curr->w_layer, curr->w_x, curr->w_y);
				}
				break;
			case 1048:
				if (i)
					SaveCursor(&curr->w_saved);
				else
					RestoreCursor(&curr->w_saved);
				break;
				/* case 66:       NKM:  Numeric keypad appl mode */
				/* case 68:       KBUM: Keyboard usage mode (data process) */
			case 1000:	/* VT200 mouse tracking */
			case 1001:	/* VT200 highlight mouse */
			case 1002:	/* button event mouse */
			case 1003:	/* any event mouse */
				curr->w_mouse = i ? a1 : 0;
				LMouseMode(&curr->w_layer, curr->w_mouse);
				break;
			case 2004:	/* bracketed paste mode */
				curr->w_bracketed = i ? true : false;
				LBracketedPasteMode(&curr->w_layer, curr->w_bracketed);
				break;
			}
		}
		break;
	case '>':
		switch (c) {
		case 'c':	/* secondary DA */
			if (a1 == 0)
				Report("\033[>%d;%d;0c", 83, nversion);	/* 83 == 'S' */
			break;
		}
		break;
	}
}

static void StringStart(enum string_t type)
{
	curr->w_StringType = type;
	curr->w_stringp = curr->w_string;
	curr->w_state = ASTR;
}

static void StringChar(int c)
{
	if (curr->w_stringp >= curr->w_string + MAXSTR - 1)
		curr->w_state = LIT;
	else
		*(curr->w_stringp)++ = c;
}

/*
 * Do string processing. Returns -1 if output should be suspended
 * until status is gone.
 */
static int StringEnd()
{
	Canvas *cv;
	char *p;
	int typ;

	curr->w_state = LIT;
	*curr->w_stringp = '\0';
	switch (curr->w_StringType) {
	case OSC:		/* special xterm compatibility hack */
		if (curr->w_string[0] == ';' || (p = strchr(curr->w_string, ';')) == 0)
			break;
		typ = atoi(curr->w_string);
		p++;
		if (typ == 83) {	/* 83 = 'S' */
			/* special execute commands sequence */
			char *args[MAXARGS];
			int argl[MAXARGS];
			struct acluser *windowuser;

			windowuser = *FindUserPtr(":window:");
			if (windowuser && Parse(p, sizeof(curr->w_string) - (p - curr->w_string), args, argl)) {
				for (display = displays; display; display = display->d_next)
					if (D_forecv->c_layer->l_bottom == &curr->w_layer)
						break;	/* found it */
				if (display == 0 && curr->w_layer.l_cvlist)
					display = curr->w_layer.l_cvlist->c_display;
				if (display == 0)
					display = displays;
				EffectiveAclUser = windowuser;
				fore = curr;
				flayer = fore->w_savelayer ? fore->w_savelayer : &fore->w_layer;
				DoCommand(args, argl);
				EffectiveAclUser = 0;
				fore = 0;
				flayer = 0;
			}
			break;
		}
		if (typ == 0 || typ == 1 || typ == 2 || typ == 20 || typ == 39 || typ == 49) {
			int typ2;
			typ2 = typ / 10;
			if (--typ2 < 0)
				typ2 = 0;
			if (strcmp(curr->w_xtermosc[typ2], p)) {
				strncpy(curr->w_xtermosc[typ2], p, sizeof(curr->w_xtermosc[typ2]) - 1);
				curr->w_xtermosc[typ2][sizeof(curr->w_xtermosc[typ2]) - 1] = 0;

				for (display = displays; display; display = display->d_next) {
					if (!D_CXT)
						continue;
					if (D_forecv->c_layer->l_bottom == &curr->w_layer)
						SetXtermOSC(typ2, curr->w_xtermosc[typ2]);
					if ((typ2 == 2 || typ2 == 3) && D_xtermosc[typ2])
						Redisplay(0);
				}
			}
		}
		if (typ != 0 && typ != 2)
			break;

		curr->w_stringp -= p - curr->w_string;
		if (curr->w_stringp > curr->w_string)
			memmove(curr->w_string, p, curr->w_stringp - curr->w_string);
		*curr->w_stringp = '\0';
		/* FALLTHROUGH */
	case APC:
		if (curr->w_hstatus) {
			if (strcmp(curr->w_hstatus, curr->w_string) == 0)
				break;	/* not changed */
			free(curr->w_hstatus);
			curr->w_hstatus = 0;
		}
		if (curr->w_string != curr->w_stringp)
			curr->w_hstatus = SaveStr(curr->w_string);
		WindowChanged(curr, WINESC_HSTATUS);
		break;
	case PM:
	case GM:
		for (display = displays; display; display = display->d_next) {
			for (cv = D_cvlist; cv; cv = cv->c_next)
				if (cv->c_layer->l_bottom == &curr->w_layer)
					break;
			if (cv || curr->w_StringType == GM)
				MakeStatus(curr->w_string);
		}
		return -1;
	case DCS:
		LAY_DISPLAYS(&curr->w_layer, AddStr(curr->w_string));
		break;
	case AKA:
		if (curr->w_title == curr->w_akabuf && !*curr->w_string)
			break;
		ChangeAKA(curr, curr->w_string, strlen(curr->w_string));
		if (!*curr->w_string)
			curr->w_autoaka = curr->w_y + 1;
		break;
	default:
		break;
	}
	return 0;
}

static void PrintStart()
{
	curr->w_pdisplay = 0;

	/* find us a nice display to print on, fore prefered */
	display = curr->w_lastdisp;
	if (!(display && curr == D_fore && (printcmd || D_PO)))
		for (display = displays; display; display = display->d_next)
			if (curr == D_fore && (printcmd || D_PO))
				break;
	if (!display) {
		Canvas *cv;
		for (cv = curr->w_layer.l_cvlist; cv; cv = cv->c_lnext) {
			display = cv->c_display;
			if (printcmd || D_PO)
				break;
		}
		if (!cv) {
			display = displays;
			if (!display || display->d_next || !(printcmd || D_PO))
				return;
		}
	}
	curr->w_pdisplay = display;
	curr->w_stringp = curr->w_string;
	curr->w_state = PRIN;
	if (printcmd && curr->w_pdisplay->d_printfd < 0)
		curr->w_pdisplay->d_printfd = printpipe(curr, printcmd);
}

static void PrintChar(int c)
{
	if (curr->w_stringp >= curr->w_string + MAXSTR - 1)
		PrintFlush();
	*(curr->w_stringp)++ = c;
}

static void PrintFlush()
{
	display = curr->w_pdisplay;
	if (display && printcmd) {
		char *bp = curr->w_string;
		int len = curr->w_stringp - curr->w_string;
		int r;
		while (len && display->d_printfd >= 0) {
			r = write(display->d_printfd, bp, len);
			if (r <= 0) {
				WMsg(curr, errno, "printing aborted");
				close(display->d_printfd);
				display->d_printfd = -1;
				break;
			}
			bp += r;
			len -= r;
		}
	} else if (display && curr->w_stringp > curr->w_string) {
		AddCStr(D_PO);
		AddStrn(curr->w_string, curr->w_stringp - curr->w_string);
		AddCStr(D_PF);
		Flush(3);
	}
	curr->w_stringp = curr->w_string;
}

void WNewAutoFlow(Window *win, int on)
{
	if (win->w_flow & FLOW_AUTOFLAG)
		win->w_flow = FLOW_AUTOFLAG | (FLOW_AUTO | FLOW_ON) * on;
	else
		win->w_flow = (win->w_flow & ~FLOW_AUTO) | FLOW_AUTO * on;
	LSetFlow(&win->w_layer, win->w_flow & FLOW_ON);
}

static void DesignateCharset(int c, int n)
{
	curr->w_ss = 0;
	if (c == ('@' & 037))	/* map JIS 6226 to 0208 */
		c = KANJI;
	if (c == 'B')
		c = ASCII;
	if (curr->w_charsets[n] != c) {
		curr->w_charsets[n] = c;
		if (curr->w_Charset == n) {
			curr->w_FontL = c;
			curr->w_rend.font = curr->w_FontL;
			LSetRendition(&curr->w_layer, &curr->w_rend);
		}
		if (curr->w_CharsetR == n)
			curr->w_FontR = c;
	}
}

static void MapCharset(int n)
{
	curr->w_ss = 0;
	if (curr->w_Charset != n) {
		curr->w_Charset = n;
		curr->w_FontL = curr->w_charsets[n];
		curr->w_rend.font = curr->w_FontL;
		LSetRendition(&curr->w_layer, &curr->w_rend);
	}
}

static void MapCharsetR(int n)
{
	curr->w_ss = 0;
	if (curr->w_CharsetR != n) {
		curr->w_CharsetR = n;
		curr->w_FontR = curr->w_charsets[n];
	}
	curr->w_gr = 1;
}

static void SaveCursor(struct cursor *cursor)
{
	cursor->on = 1;
	cursor->x = curr->w_x;
	cursor->y = curr->w_y;
	cursor->Rend = curr->w_rend;
	cursor->Charset = curr->w_Charset;
	cursor->CharsetR = curr->w_CharsetR;
	memmove((char *)cursor->Charsets, (char *)curr->w_charsets, 4 * sizeof(int));
}

static void RestoreCursor(struct cursor *cursor)
{
	if (!cursor->on)
		return;
	LGotoPos(&curr->w_layer, cursor->x, cursor->y);
	curr->w_x = cursor->x;
	curr->w_y = cursor->y;
	curr->w_rend = cursor->Rend;
	memmove((char *)curr->w_charsets, (char *)cursor->Charsets, 4 * sizeof(int));
	curr->w_Charset = cursor->Charset;
	curr->w_CharsetR = cursor->CharsetR;
	curr->w_ss = 0;
	curr->w_FontL = curr->w_charsets[curr->w_Charset];
	curr->w_FontR = curr->w_charsets[curr->w_CharsetR];
	LSetRendition(&curr->w_layer, &curr->w_rend);
}

static void BackSpace()
{
	if (curr->w_x > 0) {
		curr->w_x--;
	} else if (curr->w_wrap && curr->w_y > 0) {
		curr->w_x = cols - 1;
		curr->w_y--;
	}
	LGotoPos(&curr->w_layer, curr->w_x, curr->w_y);
}

static void Return()
{
	if (curr->w_x == 0)
		return;
	curr->w_x = 0;
	LGotoPos(&curr->w_layer, curr->w_x, curr->w_y);
}

static void LineFeed(int out_mode)
{
	/* out_mode: 0=lf, 1=cr+lf */
	if (out_mode)
		curr->w_x = 0;
	if (curr->w_y != curr->w_bot) {	/* Don't scroll */
		if (curr->w_y < rows - 1)
			curr->w_y++;
		LGotoPos(&curr->w_layer, curr->w_x, curr->w_y);
		return;
	}
	if (curr->w_autoaka > 1)
		curr->w_autoaka--;
	MScrollV(curr, 1, curr->w_top, curr->w_bot, curr->w_rend.colorbg);
	LScrollV(&curr->w_layer, 1, curr->w_top, curr->w_bot, curr->w_rend.colorbg);
	LGotoPos(&curr->w_layer, curr->w_x, curr->w_y);
}

static void ReverseLineFeed()
{
	if (curr->w_y == curr->w_top) {
		MScrollV(curr, -1, curr->w_top, curr->w_bot, curr->w_rend.colorbg);
		LScrollV(&curr->w_layer, -1, curr->w_top, curr->w_bot, curr->w_rend.colorbg);
		LGotoPos(&curr->w_layer, curr->w_x, curr->w_y);
	} else if (curr->w_y > 0)
		CursorUp(1);
}

static void InsertChar(int n)
{
	int y = curr->w_y, x = curr->w_x;

	if (n <= 0)
		return;
	if (x == cols)
		x--;
	save_mline(&curr->w_mlines[y], cols);
	MScrollH(curr, -n, y, x, curr->w_width - 1, curr->w_rend.colorbg);
	LScrollH(&curr->w_layer, -n, y, x, curr->w_width - 1, curr->w_rend.colorbg, &mline_old);
	LGotoPos(&curr->w_layer, x, y);
}

static void DeleteChar(int n)
{
	int y = curr->w_y, x = curr->w_x;

	if (x == cols)
		x--;
	save_mline(&curr->w_mlines[y], cols);
	MScrollH(curr, n, y, x, curr->w_width - 1, curr->w_rend.colorbg);
	LScrollH(&curr->w_layer, n, y, x, curr->w_width - 1, curr->w_rend.colorbg, &mline_old);
	LGotoPos(&curr->w_layer, x, y);
}

static void DeleteLine(int n)
{
	if (curr->w_y < curr->w_top || curr->w_y > curr->w_bot)
		return;
	if (n > curr->w_bot - curr->w_y + 1)
		n = curr->w_bot - curr->w_y + 1;
	MScrollV(curr, n, curr->w_y, curr->w_bot, curr->w_rend.colorbg);
	LScrollV(&curr->w_layer, n, curr->w_y, curr->w_bot, curr->w_rend.colorbg);
	LGotoPos(&curr->w_layer, curr->w_x, curr->w_y);
}

static void InsertLine(int n)
{
	if (curr->w_y < curr->w_top || curr->w_y > curr->w_bot)
		return;
	if (n > curr->w_bot - curr->w_y + 1)
		n = curr->w_bot - curr->w_y + 1;
	MScrollV(curr, -n, curr->w_y, curr->w_bot, curr->w_rend.colorbg);
	LScrollV(&curr->w_layer, -n, curr->w_y, curr->w_bot, curr->w_rend.colorbg);
	LGotoPos(&curr->w_layer, curr->w_x, curr->w_y);
}

static void ScrollRegion(int n)
{
	MScrollV(curr, n, curr->w_top, curr->w_bot, curr->w_rend.colorbg);
	LScrollV(&curr->w_layer, n, curr->w_top, curr->w_bot, curr->w_rend.colorbg);
	LGotoPos(&curr->w_layer, curr->w_x, curr->w_y);
}

static void ForwardTab()
{
	int x = curr->w_x;

	if (x == cols) {
		LineFeed(1);
		x = 0;
	}
	if (curr->w_tabs[x] && x < cols - 1)
		x++;
	while (x < cols - 1 && !curr->w_tabs[x])
		x++;
	curr->w_x = x;
	LGotoPos(&curr->w_layer, curr->w_x, curr->w_y);
}

static void BackwardTab()
{
	int x = curr->w_x;

	if (curr->w_tabs[x] && x > 0)
		x--;
	while (x > 0 && !curr->w_tabs[x])
		x--;
	curr->w_x = x;
	LGotoPos(&curr->w_layer, curr->w_x, curr->w_y);
}

static void ClearScreen()
{
	LClearArea(&curr->w_layer, 0, 0, curr->w_width - 1, curr->w_height - 1, curr->w_rend.colorbg, 1);
	MScrollV(curr, curr->w_height, 0, curr->w_height - 1, curr->w_rend.colorbg);
}

static void ClearFromBOS()
{
	int y = curr->w_y, x = curr->w_x;

	LClearArea(&curr->w_layer, 0, 0, x, y, curr->w_rend.colorbg, 1);
	MClearArea(curr, 0, 0, x, y, curr->w_rend.colorbg);
	RestorePosRendition();
}

static void ClearToEOS()
{
	int y = curr->w_y, x = curr->w_x;

	if (x == 0 && y == 0) {
		ClearScreen();
		RestorePosRendition();
		return;
	}
	LClearArea(&curr->w_layer, x, y, cols - 1, rows - 1, curr->w_rend.colorbg, 1);
	MClearArea(curr, x, y, cols - 1, rows - 1, curr->w_rend.colorbg);
	RestorePosRendition();
}

static void ClearLineRegion(int from, int to)
{
	int y = curr->w_y;
	LClearArea(&curr->w_layer, from, y, to, y, curr->w_rend.colorbg, 1);
	MClearArea(curr, from, y, to, y, curr->w_rend.colorbg);
	RestorePosRendition();
}

static void CursorRight(int n)
{
	int x = curr->w_x;

	if (x == cols)
		LineFeed(1);
	if ((curr->w_x += n) >= cols)
		curr->w_x = cols - 1;
	LGotoPos(&curr->w_layer, curr->w_x, curr->w_y);
}

static void CursorUp(int n)
{
	if (curr->w_y < curr->w_top) {	/* if above scrolling rgn, */
		if ((curr->w_y -= n) < 0)	/* ignore its limits      */
			curr->w_y = 0;
	} else if ((curr->w_y -= n) < curr->w_top)
		curr->w_y = curr->w_top;
	LGotoPos(&curr->w_layer, curr->w_x, curr->w_y);
}

static void CursorDown(int n)
{
	if (curr->w_y > curr->w_bot) {	/* if below scrolling rgn, */
		if ((curr->w_y += n) > rows - 1)	/* ignore its limits      */
			curr->w_y = rows - 1;
	} else if ((curr->w_y += n) > curr->w_bot)
		curr->w_y = curr->w_bot;
	LGotoPos(&curr->w_layer, curr->w_x, curr->w_y);
}

static void CursorLeft(int n)
{
	if ((curr->w_x -= n) < 0)
		curr->w_x = 0;
	LGotoPos(&curr->w_layer, curr->w_x, curr->w_y);
}

static void ASetMode(bool on)
{
	for (int i = 0; i < curr->w_NumArgs; ++i) {
		switch (curr->w_args[i]) {
			/* case 2:            KAM: Lock keyboard */
		case 4:	/* IRM: Insert mode */
			curr->w_insert = on;
			LAY_DISPLAYS(&curr->w_layer, InsertMode(on));
			break;
			/* case 12:           SRM: Echo mode on */
		case 20:	/* LNM: Linefeed mode */
			curr->w_autolf = on;
			break;
		case 34:
			curr->w_curvvis = !on;
			LCursorVisibility(&curr->w_layer, curr->w_curinv ? -1 : curr->w_curvvis);
			break;
		default:
			break;
		}
	}
}

static char rendlist[] = {
	~((1 << NATTR) - 1), A_BD, A_DI, A_SO, A_US, A_BL, 0, A_RV, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, ~(A_BD | A_SO | A_DI), ~A_SO, ~A_US, ~A_BL, 0, ~A_RV
};

static void SelectRendition()
{
	int j, i = 0;
	int attr = curr->w_rend.attr;
	int colorbg = curr->w_rend.colorbg;
	int colorfg = curr->w_rend.colorfg;

	do {
		j = curr->w_args[i];
		/* indexed colour space aka 256 colours; example escape \e[48;2;12m */
		if ((j == 38 || j == 48) && i + 2 < curr->w_NumArgs && curr->w_args[i + 1] == 5) {
			int jj;

			i += 2;
			jj = curr->w_args[i];
			if (jj < 0 || jj > 255)
				continue;
			if (j == 38) {
				colorfg = jj | 0x01000000;
			} else {
				colorbg = jj | 0x01000000;
			}
			continue;
		}
		/* truecolor (24bit) colour space; example escape \e[48;5;12;13;14m 
		 * where 12;13;14 are rgb values */
		if ((j == 38 || j == 48) && i + 4 < curr->w_NumArgs && curr->w_args[i + 1] == 2) {
			uint8_t r, g, b;

			r = curr->w_args[i + 2];
			g = curr->w_args[i + 3];
			b = curr->w_args[i + 4];

			if (j == 38) {
				colorfg = 0x02000000 | (r << 16) | (g << 8) | b;
			} else {
				colorbg = 0x02000000 | (r << 16) | (g << 8) | b;
			}
			i += 4;
			continue;
		}
		if (j >= 90 && j <= 97)
			colorfg = (j - 90 + 8) | 0x01000000;
		if (j >= 100 && j <= 107)
			colorbg = (j - 100 + 8) | 0x01000000;
		if (j >= 30 && j < 38)
			colorfg = (j - 30) | 0x01000000;
		if (j >= 40 && j < 48)
			colorbg = (j - 40) | 0x01000000;
		if (j == 39)
			colorfg = 0;
		if (j == 49)
			colorbg = 0;
		if (j == 0) {
			attr = 0;
			/* will be xored to 0 */
			colorbg = 0;
			colorfg = 0;
		}

		if (j < 0 || j >= (int)(sizeof(rendlist)/sizeof(*rendlist)))
			continue;
		j = rendlist[j];
		if (j & (1 << NATTR))
			attr &= j;
		else
			attr |= j;
	} while (++i < curr->w_NumArgs);
	
	curr->w_rend.attr = attr;
	
	curr->w_rend.colorbg = colorbg;
	curr->w_rend.colorfg = colorfg;
	LSetRendition(&curr->w_layer, &curr->w_rend);
}

static void FillWithEs()
{
	uint32_t *p, *ep;

	LClearAll(&curr->w_layer, 1);
	curr->w_y = curr->w_x = 0;
	for (int i = 0; i < rows; ++i) {
		clear_mline(&curr->w_mlines[i], 0, cols + 1);
		p = curr->w_mlines[i].image;
		ep = p + cols;
		while (p < ep)
			*p++ = 'E';
	}
	LRefreshAll(&curr->w_layer, 1);
}

/*
 *  Ugly autoaka hack support:
 *    ChangeAKA() sets a new aka
 *    FindAKA() searches for an autoaka match
 */

void ChangeAKA(Window *win, char *s, size_t len)
{
	int i, c;

	for (i = 0; len > 0; len--) {
		if (win->w_akachange + i == win->w_akabuf + sizeof(win->w_akabuf) - 1)
			break;
		c = (unsigned char)*s++;
		if (c == 0)
			break;
		if (c < 32 || c == 127 || (c >= 128 && c < 160 && win->w_c1))
			continue;
		win->w_akachange[i++] = c;
	}
	win->w_akachange[i] = 0;
	win->w_title = win->w_akachange;
	if (win->w_akachange != win->w_akabuf)
		if (win->w_akachange[0] == 0 || win->w_akachange[-1] == ':')
			win->w_title = win->w_akabuf + strlen(win->w_akabuf) + 1;
	WindowChanged(win, WINESC_WIN_TITLE);
	WindowChanged((Window *)0, WINESC_WIN_NAMES);
	WindowChanged((Window *)0, WINESC_WIN_NAMES_NOCUR);
}

static void FindAKA()
{
	uint32_t *cp, *line;
	Window *win = curr;
	int len = strlen(win->w_akabuf);
	int y;

	y = (win->w_autoaka > 0 && win->w_autoaka <= win->w_height) ? win->w_autoaka - 1 : win->w_y;
	cols = win->w_width;
 try_line:
	cp = line = win->w_mlines[y].image;
	if (win->w_autoaka > 0 && *win->w_akabuf != '\0') {
		for (;;) {
			if (cp - line >= cols - len) {
				if (++y == win->w_autoaka && y < rows)
					goto try_line;
				return;
			}
			if (strncmp((char *)cp, win->w_akabuf, len) == 0)
				break;
			cp++;
		}
		cp += len;
	}
	for (len = cols - (cp - line); len && *cp == ' '; len--, cp++) ;
	if (len) {
		if (win->w_autoaka > 0 && (*cp == '!' || *cp == '%' || *cp == '^'))
			win->w_autoaka = -1;
		else
			win->w_autoaka = 0;
		line = cp;
		while (len && *cp != ' ') {
			if (*cp++ == '/')
				line = cp;
			len--;
		}
		ChangeAKA(win, (char *)line, cp - line);
	} else
		win->w_autoaka = 0;
}

static void RestorePosRendition()
{
	LGotoPos(&curr->w_layer, curr->w_x, curr->w_y);
	LSetRendition(&curr->w_layer, &curr->w_rend);
}

/* Send a terminal report as if it were typed. */
static void Report(char *fmt, int n1, int n2)
{
	int len;
	char rbuf[40];		/* enough room for all replys */

	sprintf(rbuf, fmt, n1, n2);
	len = strlen(rbuf);

	if (W_UWP(curr)) {
		if ((unsigned)(curr->w_pwin->p_inlen + len) <= sizeof(curr->w_pwin->p_inbuf)) {
			memmove(curr->w_pwin->p_inbuf + curr->w_pwin->p_inlen, rbuf, len);
			curr->w_pwin->p_inlen += len;
		}
	} else {
		if ((unsigned)(curr->w_inlen + len) <= sizeof(curr->w_inbuf)) {
			memmove(curr->w_inbuf + curr->w_inlen, rbuf, len);
			curr->w_inlen += len;
		}
	}
}

/*
 *====================================================================*
 *====================================================================*
 */

/**********************************************************************
 *
 * Memory subsystem.
 *
 */

static void MFixLine(Window *win, int y, struct mchar *mc)
{
	struct mline *ml = &win->w_mlines[y];
	if (mc->attr && ml->attr == null) {
		if ((ml->attr = calloc(win->w_width + 1, 4)) == 0) {
			ml->attr = null;
			mc->attr = win->w_rend.attr = 0;
			WMsg(win, 0, "Warning: no space for attr - turned off");
		}
	}
	if (mc->font && ml->font == null) {
		if ((ml->font = calloc(win->w_width + 1, 4)) == 0) {
			ml->font = null;
			win->w_FontL = win->w_charsets[win->w_ss ? win->w_ss : win->w_Charset] = 0;
			win->w_FontR = win->w_charsets[win->w_ss ? win->w_ss : win->w_CharsetR] = 0;
			mc->font = mc->fontx = win->w_rend.font = 0;
			WMsg(win, 0, "Warning: no space for font - turned off");
		}
	}
	if (mc->fontx && ml->fontx == null) {
		if ((ml->fontx = calloc(win->w_width + 1, 4)) == 0) {
			ml->fontx = null;
			mc->fontx = 0;
		}
	}
	if (mc->colorbg && ml->colorbg == null) {
		if ((ml->colorbg = calloc(win->w_width + 1, 4)) == 0) {
			ml->colorbg = null;
			mc->colorbg = win->w_rend.colorbg = 0;
			WMsg(win, 0, "Warning: no space for color background - turned off");
		}
	}
	if (mc->colorfg && ml->colorfg == null) {
		if ((ml->colorfg = calloc(win->w_width + 1, 4)) == 0) {
			ml->colorfg = null;
			mc->colorfg = win->w_rend.colorfg = 0;
			WMsg(win, 0, "Warning: no space for color foreground - turned off");
		}
	}
}

/*****************************************************************/

#define MKillDwRight(p, ml, x)					\
  if (dw_right(ml, x, p->w_encoding))				\
    {								\
      if (x > 0)						\
	copy_mchar2mline(&mchar_blank, ml, x - 1);		\
      copy_mchar2mline(&mchar_blank, ml, x);			\
    }

#define MKillDwLeft(p, ml, x)					\
  if (dw_left(ml, x, p->w_encoding))				\
    {								\
      copy_mchar2mline(&mchar_blank, ml, x);			\
      copy_mchar2mline(&mchar_blank, ml, x + 1);		\
    }

static void MScrollH(Window *win, int n, int y, int xs, int xe, int bce)
{
	struct mline *ml;

	if (n == 0)
		return;
	ml = &win->w_mlines[y];
	MKillDwRight(win, ml, xs);
	MKillDwLeft(win, ml, xe);
	if (n > 0) {
		if (xe - xs + 1 > n) {
			MKillDwRight(win, ml, xs + n);
			copy_mline(ml, xs + n, xs, xe + 1 - xs - n);
		} else
			n = xe - xs + 1;
		clear_mline(ml, xe + 1 - n, n);
		if (bce)
			MBceLine(win, y, xe + 1 - n, n, bce);
	} else {
		n = -n;
		if (xe - xs + 1 > n) {
			MKillDwLeft(win, ml, xe - n);
			copy_mline(ml, xs, xs + n, xe + 1 - xs - n);
		} else
			n = xe - xs + 1;
		clear_mline(ml, xs, n);
		if (bce)
			MBceLine(win, y, xs, n, bce);
	}
}

static void MScrollV(Window *win, int n, int ys, int ye, int bce)
{
	int cnt1, cnt2;
	struct mline tmp[256];
	struct mline *ml;

	if (n == 0)
		return;
	if (n > 0) {
		if (ye - ys + 1 < n)
			n = ye - ys + 1;
		if (n > 256) {
			MScrollV(win, n - 256, ys, ye, bce);
			n = 256;
		}
		if (compacthist) {
			ye = MFindUsedLine(win, ye, ys);
			if (ye - ys + 1 < n)
				n = ye - ys + 1;
			if (n <= 0)
				return;
		}
		/* Clear lines */
		ml = win->w_mlines + ys;
		for (int i = ys; i < ys + n; i++, ml++) {
			if (ys == win->w_top)
				WAddLineToHist(win, ml);
			if (ml->attr != null)
				free(ml->attr);
			ml->attr = null;
			if (ml->font != null)
				free(ml->font);
			ml->font = null;
			if (ml->fontx != null)
				free(ml->fontx);
			ml->fontx = null;
			if (ml->colorbg != null)
				free(ml->colorbg);
			ml->colorbg = null;
			if (ml->colorfg != null)
				free(ml->colorfg);
			ml->colorfg = null;
			memmove(ml->image, blank, (win->w_width + 1) * 4);
			if (bce)
				MBceLine(win, i, 0, win->w_width, bce);
		}
		/* switch 'em over */
		cnt1 = n * sizeof(struct mline);
		cnt2 = (ye - ys + 1 - n) * sizeof(struct mline);
		if (cnt1 && cnt2)
			Scroll((char *)(win->w_mlines + ys), cnt1, cnt2, (char *)tmp);
	} else {
		n = -n;
		if (ye - ys + 1 < n)
			n = ye - ys + 1;
		if (n > 256) {
			MScrollV(win, - (n - 256), ys, ye, bce);
			n = 256;
		}

		ml = win->w_mlines + ye;
		/* Clear lines */
		for (int i = ye; i > ye - n; i--, ml--) {
			if (ml->attr != null)
				free(ml->attr);
			ml->attr = null;
			if (ml->font != null)
				free(ml->font);
			ml->font = null;
			if (ml->fontx != null)
				free(ml->fontx);
			ml->fontx = null;
			if (ml->colorbg != null)
				free(ml->colorbg);
			ml->colorbg = null;
			if (ml->colorfg != null)
				free(ml->colorfg);
			ml->colorfg = null;
			memmove(ml->image, blank, (win->w_width + 1) * 4);
			if (bce)
				MBceLine(win, i, 0, win->w_width, bce);
		}
		cnt1 = n * sizeof(struct mline);
		cnt2 = (ye - ys + 1 - n) * sizeof(struct mline);
		if (cnt1 && cnt2)
			Scroll((char *)(win->w_mlines + ys), cnt2, cnt1, (char *)tmp);
	}
}

static void Scroll(char *cp, int cnt1, int cnt2, char *tmp)
{
	if (!cnt1 || !cnt2)
		return;
	if (cnt1 <= cnt2) {
		memmove(tmp, cp, cnt1);
		memmove(cp, cp + cnt1, cnt2);
		memmove(cp + cnt2, tmp, cnt1);
	} else {
		memmove(tmp, cp + cnt1, cnt2);
		memmove(cp + cnt2, cp, cnt1);
		memmove(cp, tmp, cnt2);
	}
}

static void MClearArea(Window *win, int xs, int ys, int xe, int ye, int bce)
{
	int n;
	int xxe;
	struct mline *ml;

	/* Check for zero-height window */
	if (ys < 0 || ye < ys)
		return;

	/* check for magic margin condition */
	if (xs >= win->w_width)
		xs = win->w_width - 1;
	if (xe >= win->w_width)
		xe = win->w_width - 1;

	MKillDwRight(win, win->w_mlines + ys, xs);
	MKillDwLeft(win, win->w_mlines + ye, xe);

	ml = win->w_mlines + ys;
	for (int y = ys; y <= ye; y++, ml++) {
		xxe = (y == ye) ? xe : win->w_width - 1;
		n = xxe - xs + 1;
		if (n > 0)
			clear_mline(ml, xs, n);
		if (n > 0 && bce)
			MBceLine(win, y, xs, xs + n - 1, bce);
		xs = 0;
	}
}

static void MInsChar(Window *win, struct mchar *c, int x, int y)
{
	int n;
	struct mline *ml;

	MFixLine(win, y, c);
	ml = win->w_mlines + y;
	n = win->w_width - x - 1;
	MKillDwRight(win, ml, x);
	if (n > 0) {
		MKillDwRight(win, ml, win->w_width - 1);
		copy_mline(ml, x, x + 1, n);
	}
	copy_mchar2mline(c, ml, x);
	if (c->mbcs) {
		if (--n > 0) {
			MKillDwRight(win, ml, win->w_width - 1);
			copy_mline(ml, x + 1, x + 2, n);
		}
		copy_mchar2mline(c, ml, x + 1);
		ml->image[x + 1] = c->mbcs;
		if (win->w_encoding != UTF8)
			ml->font[x + 1] |= 0x80;
		else if (win->w_encoding == UTF8 && c->mbcs) {
			ml->font[x + 1] = c->mbcs;
			ml->fontx[x + 1] = 0;
		}
	}
}

static void MPutChar(Window *win, struct mchar *c, int x, int y)
{
	struct mline *ml;

	MFixLine(win, y, c);
	ml = &win->w_mlines[y];
	MKillDwRight(win, ml, x);
	MKillDwLeft(win, ml, x);
	copy_mchar2mline(c, ml, x);
	if (c->mbcs) {
		MKillDwLeft(win, ml, x + 1);
		copy_mchar2mline(c, ml, x + 1);
		ml->image[x + 1] = c->mbcs;
		if (win->w_encoding != UTF8)
			ml->font[x + 1] |= 0x80;
		else if (win->w_encoding == UTF8 && c->mbcs) {
			ml->font[x + 1] = c->mbcs;
			ml->fontx[x + 1] = 0;
		}
	}
}

static void MWrapChar(Window *win, struct mchar *c, int y, int top, int bot, bool ins)
{
	struct mline *ml;
	int bce;

	bce = c->colorbg;
	MFixLine(win, y, c);
	ml = &win->w_mlines[y];
	copy_mchar2mline(&mchar_null, ml, win->w_width);
	if (y == bot)
		MScrollV(win, 1, top, bot, bce);
	else if (y < win->w_height - 1)
		y++;
	if (ins)
		MInsChar(win, c, 0, y);
	else
		MPutChar(win, c, 0, y);
}

static void MBceLine(Window *win, int y, int xs, int xe, int bce)
{
	struct mchar mc;
	struct mline *ml;

	mc = mchar_null;
	mc.colorbg = bce;
	MFixLine(win, y, &mc);
	ml = win->w_mlines + y;
	if (mc.attr)
		for (int x = xs; x <= xe; x++)
			ml->attr[x] = mc.attr;
	if (mc.colorbg)
		for (int x = xs; x <= xe; x++)
			ml->colorbg[x] = mc.colorbg;
	if (mc.colorfg)
		for (int x = xs; x <= xe; x++)
			ml->colorfg[x] = mc.colorfg;
}

static void WAddLineToHist(Window *win, struct mline *ml)
{
	uint32_t *q, *o;
	struct mline *hml;

	if (win->w_histheight == 0)
		return;
	hml = &win->w_hlines[win->w_histidx];
	q = ml->image;
	ml->image = hml->image;
	hml->image = q;

	q = ml->attr;
	o = hml->attr;
	hml->attr = q;
	ml->attr = null;
	if (o != null)
		free(o);
	q = ml->font;
	o = hml->font;
	hml->font = q;
	ml->font = null;
	if (o != null)
		free(o);
	q = ml->fontx;
	o = hml->fontx;
	hml->fontx = q;
	ml->fontx = null;
	if (o != null)
		free(o);
	q = ml->colorbg;
	o = hml->colorbg;
	hml->colorbg = q;
	ml->colorbg = null;
	if (o != null)
		free(o);
	q = ml->colorfg;
	o = hml->colorfg;
	hml->colorfg = q;
	ml->colorfg = null;
	if (o != null)
		free(o);

	if (++win->w_histidx >= win->w_histheight)
		win->w_histidx = 0;
}

int MFindUsedLine(Window *win, int ye, int ys)
{
	int y;
	struct mline *ml = win->w_mlines + ye;

	for (y = ye; y >= ys; y--, ml--) {
		if (memcmp(ml->image, blank, win->w_width * 4))
			break;
		if (ml->attr != null && memcmp(ml->attr, null, win->w_width * 4))
			break;
		if (ml->colorbg != null && memcmp(ml->colorbg, null, win->w_width * 4))
			break;
		if (ml->colorfg != null && memcmp(ml->colorfg, null, win->w_width * 4))
			break;
		if (win->w_encoding == UTF8) {
			if (ml->font != null && memcmp(ml->font, null, win->w_width))
				break;
			if (ml->fontx != null && memcmp(ml->fontx, null, win->w_width))
				break;
		}
	}
	return y;
}

/*
 *====================================================================*
 *====================================================================*
 */

/*
 * Tricky: send only one bell even if the window is displayed
 * more than once.
 */
void WBell(Window *win, bool visual)
{
	Canvas *cv;
	if (displays == NULL)
		win->w_bell = BELL_DONE;
	for (display = displays; display; display = display->d_next) {
		for (cv = D_cvlist; cv; cv = cv->c_next)
			if (cv->c_layer->l_bottom == &win->w_layer)
				break;
		if (cv && !visual)
			AddCStr(D_BL);
		else if (cv && D_VB)
			AddCStr(D_VB);
		else
			win->w_bell = visual ? BELL_VISUAL : BELL_FOUND;
	}
}

/*
 * This should be reverse video.
 * Only change video if window is fore.
 * Because it is used in some termcaps to emulate
 * a visual bell we do this hack here.
 * (screen uses \Eg as special vbell sequence)
 */
static void WReverseVideo(Window *win, int on)
{
	for (Canvas *cv = win->w_layer.l_cvlist; cv; cv = cv->c_lnext) {
		display = cv->c_display;
		if (cv != D_forecv)
			continue;
		ReverseVideo(on);
		if (!on && win->w_revvid && !D_CVR) {
			if (D_VB)
				AddCStr(D_VB);
			else
				win->w_bell = BELL_VISUAL;
		}
	}
}

void WMsg(Window *win, int err, char *str)
{
	Layer *oldflayer = flayer;
	flayer = &win->w_layer;
	LMsg(err, "%s", str);
	flayer = oldflayer;
}

void WChangeSize(Window *win, int w, int h)
{
	int wok = 0;
	Canvas *cv;

	if (win->w_layer.l_cvlist == 0) {
		/* window not displayed -> works always */
		ChangeWindowSize(win, w, h, win->w_histheight);
		return;
	}
	for (cv = win->w_layer.l_cvlist; cv; cv = cv->c_lnext) {
		display = cv->c_display;
		if (win != D_fore)
			continue;	/* change only fore */
		if (D_CWS)
			break;
		if (D_CZ0 && (w == Z0width || w == Z1width))
			wok = 1;
	}
	if (cv == 0 && wok == 0)	/* can't change any display */
		return;
	if (!D_CWS)
		h = win->w_height;
	ChangeWindowSize(win, w, h, win->w_histheight);
	for (display = displays; display; display = display->d_next) {
		if (win == D_fore) {
			if (D_cvlist && D_cvlist->c_next == 0)
				ResizeDisplay(w, h);
			else
				ResizeDisplay(w, D_height);
			ResizeLayersToCanvases();	/* XXX Hmm ? */
			continue;
		}
		for (cv = D_cvlist; cv; cv = cv->c_next)
			if (cv->c_layer->l_bottom == &win->w_layer)
				break;
		if (cv)
			Redisplay(0);
	}
}

