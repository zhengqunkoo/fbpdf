/*
 * FBPDF LINUX FRAMEBUFFER PDF VIEWER
 *
 * Copyright (C) 2009-2016 Ali Gholami Rudi <ali at rudi dot ir>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include <linux/input.h>
#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include "draw.h"
#include "doc.h"
#include "dev-input-mice/mouse.h"

#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define MAX(a, b)	((a) > (b) ? (a) : (b))

#define PAGESTEPS	8
#define MINZOOM		10
#define MAXZOOM		100
#define MARGIN		1
#define CTRLKEY(x)	((x) - 96)
#define ISMARK(x)	(isalpha(x) || (x) == '\'' || (x) == '`')

static struct doc *doc;
static fbval_t *pbuf;		/* current page */
static int srows, scols;	/* screen dimentions */
static int prows, pcols;	/* current page dimensions */
static int prow, pcol;		/* page position */
static int srow, scol;		/* screen position */

static struct termios termios;
static char filename[256];
static int mark[128];		/* mark page number */
static int mark_row[128];	/* mark head position */
static int num = 1;		/* page number */
static int numdiff;		/* G command page number difference */
static int zoom = 15;
static int zoom_def = 15;	/* default zoom */
static int rotate;
static int count;
static int invert;		/* invert colors? */

static void draw(void)
{
	int i;
	fbval_t *rbuf = malloc(scols * sizeof(rbuf[0]));
	for (i = srow; i < srow + srows; i++) {
		int cbeg = MAX(scol, pcol);
		int cend = MIN(scol + scols, pcol + pcols);
		memset(rbuf, 0, scols * sizeof(rbuf[0]));
		if (i >= prow && i < prow + prows && cbeg < cend) {
			memcpy(rbuf + cbeg - scol,
				pbuf + (i - prow) * pcols + cbeg - pcol,
				(cend - cbeg) * sizeof(rbuf[0]));
		}
		fb_set(i - srow, 0, rbuf, scols);
	}
	free(rbuf);
}

static int loadpage(int p)
{
	int i;
	if (p < 1 || p > doc_pages(doc))
		return 1;
	prows = 0;
	free(pbuf);
	pbuf = doc_draw(doc, p, zoom, rotate, &prows, &pcols);
	if (invert) {
		for (i = 0; i < prows * pcols; i++)
			pbuf[i] = pbuf[i] ^ 0xffffffff;
	}
	prow = -prows / 2;
	pcol = -pcols / 2;
	num = p;
	return 0;
}

static void zoom_page(int z)
{
	int _zoom = MAX(MINZOOM, zoom);
	zoom = MIN(MAXZOOM, MAX(1, z));
	if (!loadpage(num))
		srow = srow * zoom / _zoom;
}

static void setmark(int c)
{
	if (ISMARK(c)) {
		mark[c] = num;
		mark_row[c] = srow / zoom;
	}
}

static void jmpmark(int c, int offset)
{
	if (c == '`')
		c = '\'';
	if (ISMARK(c) && mark[c]) {
		int dst = mark[c];
		int dst_row = offset ? mark_row[c] * zoom : 0;
		setmark('\'');
		if (!loadpage(dst))
			srow = offset ? dst_row : prow;
	}
}

static unsigned char readkey(void)
{
	unsigned char b;
	if (read(0, &b, 1) <= 0)
		return -1;
	return b;
}

static int getcount(int def)
{
	int result = count ? count : def;
	count = 0;
	return result;
}

static void printinfo(void)
{
	printf("\x1b[H");
	printf("FBPDF:     file:%s  page:%d(%d)  zoom:%d%% \x1b[K\r",
		filename, num, doc_pages(doc), zoom * 10);
	fflush(stdout);
}

static void term_setup(void)
{
	struct termios newtermios;
	tcgetattr(0, &termios);
	newtermios = termios;
	newtermios.c_lflag &= ~ICANON;
	newtermios.c_lflag &= ~ECHO;
	tcsetattr(0, TCSAFLUSH, &newtermios);
	printf("\x1b[?25l");		/* hide the cursor */
	printf("\x1b[2J");		/* clear the screen */
	fflush(stdout);
}

static void term_cleanup(void)
{
	tcsetattr(0, 0, &termios);
	printf("\x1b[?25h\n");		/* show the cursor */
}

static void sigcont(int sig)
{
	term_setup();
}

static int reload(void)
{
	doc_close(doc);
	doc = doc_open(filename);
	if (!doc || !doc_pages(doc)) {
		fprintf(stderr, "\nfbpdf: cannot open <%s>\n", filename);
		return 1;
	}
	if (!loadpage(num))
		draw();
	return 0;
}

static int rmargin(void)
{
	int ret = 0;
	int i, j;
	for (i = 0; i < prows; i++) {
		j = pcols - 1;
		while (j > ret && pbuf[i * pcols + j] == FB_VAL(255, 255, 255))
			j--;
		if (ret < j)
			ret = j;
	}
	return ret;
}

static int lmargin(void)
{
	int ret = pcols;
	int i, j;
	for (i = 0; i < prows; i++) {
		j = 0;
		while (j < ret && pbuf[i * pcols + j] == FB_VAL(255, 255, 255))
			j++;
		if (ret > j)
			ret = j;
	}
	return ret;
}

static void safe_pipe(int p[2])
{
	if (pipe(p)) {
		fprintf(stderr, "Pipe failed\n");
		exit(EXIT_FAILURE);
	}
}

static void safe_dup2(int fildes, int fildes2)
{
	if (dup2(fildes, fildes2) == -1) {
		fprintf(stderr, "Dup failed\n");
		exit(EXIT_FAILURE);
	}
}

static void safe_pthread_create(pthread_t *restrict thread, const pthread_attr_t *restrict attr, void *(*start_routine)(void*), void *restrict arg)
{
	if (pthread_create(thread, attr, start_routine, arg) == -1) {
		fprintf(stderr, "Pthread failed\n");
		exit(EXIT_FAILURE);
	}
}

static long safe_strtol(const char *restrict str, char **restrict endptr, int base)
{
	long res = strtol(str, endptr, base);
	if (str == *endptr) {
		fprintf(stderr, "No digits found\n");
		exit(EXIT_FAILURE);
	}
	if (res == 0 && errno != 0) {
		fprintf(stderr, "Wrong escape code\n");
		exit(EXIT_FAILURE);
	}
	return res;
}

static void *mouse_loop(void *arg)
{
	int mouse_f;
	struct packet p;
	char *s = malloc(14*sizeof(char));
	mouse_f = safe_open_mousefile();
	init_mouse(mouse_f);
	while (1) {
		safe_read(mouse_f, &p, sizeof(p));
		if (p.m) {
			sprintf(s, "\x1b[m%d;%df", p.x, p.y);
			safe_write(STDOUT_FILENO, s, strlen(s));
		}
		if (p.b)
			safe_write(STDOUT_FILENO, "K", 1);
		if (p.f)
			safe_write(STDOUT_FILENO, "J", 1);
		switch (p.z) {
		case 1:
			safe_write(STDOUT_FILENO, "j", 1);
			break;
		case 2:
			safe_write(STDOUT_FILENO, "l", 1);
			break;
		case 0xE:
			safe_write(STDOUT_FILENO, "h", 1);
			break;
		case 0xF:
			safe_write(STDOUT_FILENO, "k", 1);
			break;
		default:
			break;
		}
	}
	safe_close(mouse_f);
	free(s);
	return NULL;
}

static void keyboard_loop(void)
{
	int c;
	while ((c = readkey()) != -1) {
		if (c == 'q')
			break;
		safe_write(STDOUT_FILENO, &c, 1);
	}
	return;
}

static void mainloop(void)
{
	int step = srows / PAGESTEPS;
	int hstep = scols / PAGESTEPS;
	char c;
	int j;
	char *s = malloc(10*sizeof(char));
	char *t;
	signal(SIGCONT, sigcont);
	loadpage(num);
	srow = prow;
	scol = -scols / 2;
	draw();
	while ((c = readkey()) != -1) {
		if (c == 'q')
			break;
		if (c == 'e' && reload())
			break;
		switch (c) {	/* commands that do not require redrawing */
		case 'o':
			numdiff = num - getcount(num);
			break;
		case 'Z':
			zoom_def = getcount(zoom);
			break;
		case 'i':
			printinfo();
			break;
		case 27:
			count = 0;
			break;
		case 'm':
			setmark(readkey());
			break;
		case 'd':
			sleep(getcount(1));
			break;
		default:
			if (isdigit(c))
				count = count * 10 + c - '0';
		}
		if (c == '\x1b') {	/* terminal input sequence */
			readkey();
			switch (readkey()) {
			case 'm':	/* assume mouse escape code is well-formed */
				j = 0;
				while ((c = readkey()) != -1) {
					if (c == 'f') {
						s[j] = '\0';
						break;
					}
					s[j] = c;
					j++;
				}
				scol += safe_strtol(s, &t, 0);
				srow -= safe_strtol(t+1, &t, 0);	/* skip ';' */
				c = CTRLKEY('l');	/* redraw */
				break;
			}
		}
		switch (c) {	/* commands that require redrawing */
		case CTRLKEY('f'):
		case 'J':
			if (!loadpage(num + getcount(1)))
				srow = prow;
			break;
		case CTRLKEY('b'):
		case 'K':
			if (!loadpage(num - getcount(1)))
				srow = prow;
			break;
		case 'G':
			setmark('\'');
			if (!loadpage(getcount(doc_pages(doc) - numdiff) + numdiff))
				srow = prow;
			break;
		case 'O':
			numdiff = num - getcount(num);
			setmark('\'');
			if (!loadpage(num + numdiff))
				srow = prow;
			break;
		case 'z':
			zoom_page(getcount(zoom_def));
			break;
		case 'w':
			zoom_page(pcols ? zoom * scols / pcols : zoom);
			break;
		case 'W':
			if (lmargin() < rmargin())
				zoom_page(zoom * (scols - hstep) /
					(rmargin() - lmargin()));
			break;
		case 'f':
			zoom_page(prows ? zoom * srows / prows : zoom);
			break;
		case 'r':
			rotate = getcount(0);
			if (!loadpage(num))
				srow = prow;
			break;
		case '`':
		case '\'':
			jmpmark(readkey(), c == '`');
			break;
		case 'j':
			srow += step * getcount(1);
			break;
		case 'k':
			srow -= step * getcount(1);
			break;
		case 'l':
			scol += hstep * getcount(1);
			break;
		case 'h':
			scol -= hstep * getcount(1);
			break;
		case 'H':
			srow = prow;
			break;
		case 'L':
			srow = prow + prows - srows;
			break;
		case 'M':
			srow = prow + prows / 2 - srows / 2;
			break;
		case 'C':
			scol = -scols / 2;
			break;
		case ' ':
		case CTRLKEY('d'):
			srow += srows * getcount(1) - step;
			break;
		case 127:
		case CTRLKEY('u'):
			srow -= srows * getcount(1) - step;
			break;
		case '[':
			scol = pcol;
			break;
		case ']':
			scol = pcol + pcols - scols;
			break;
		case '{':
			scol = pcol + lmargin() - hstep / 2;
			break;
		case '}':
			scol = pcol + rmargin() + hstep / 2 - scols;
			break;
		case CTRLKEY('l'):
			break;
		case 'I':
			invert = !invert;
			loadpage(num);
			break;
		default:	/* no need to redraw */
			continue;
		}
		srow = MAX(prow - srows + MARGIN, MIN(prow + prows - MARGIN, srow));
		scol = MAX(pcol - scols + MARGIN, MIN(pcol + pcols - MARGIN, scol));
		draw();
	}
	free(s);
}

static char *usage =
	"usage: fbpdf [-r rotation] [-z zoom x10] [-p page] filename\n";

int main(int argc, char *argv[])
{
	int i = 1;
	int mousekey[2];
	pthread_t mouse_thread;
	if (argc < 2) {
		printf(usage);
		return 1;
	}
	strcpy(filename, argv[argc - 1]);
	doc = doc_open(filename);
	if (!doc || !doc_pages(doc)) {
		fprintf(stderr, "fbpdf: cannot open <%s>\n", filename);
		return 1;
	}
	for (i = 1; i < argc && argv[i][0] == '-'; i++) {
		switch (argv[i][1]) {
		case 'r':
			rotate = atoi(argv[i][2] ? argv[i] + 2 : argv[++i]);
			break;
		case 'z':
			zoom = atoi(argv[i][2] ? argv[i] + 2 : argv[++i]);
			break;
		case 'p':
			num = atoi(argv[i][2] ? argv[i] + 2 : argv[++i]);
			break;
		}
	}
	printinfo();
	safe_pipe(mousekey);
	if (fork() > 0) {
		term_setup();
		int stdout_old = dup(STDOUT_FILENO);
		safe_close(mousekey[0]);
		safe_dup2(mousekey[1], STDOUT_FILENO);
		safe_close(mousekey[1]);
		safe_pthread_create(&mouse_thread, NULL, &mouse_loop, NULL);
		keyboard_loop();
		safe_dup2(stdout_old, STDOUT_FILENO);
		close(stdout_old);
		term_cleanup();
	} else {
		safe_close(mousekey[1]);
		safe_dup2(mousekey[0], STDIN_FILENO);
		safe_close(mousekey[0]);
		if (fb_init())
			return 1;
		srows = fb_rows();
		scols = fb_cols();
		if (FBM_BPP(fb_mode()) != sizeof(fbval_t))
			fprintf(stderr, "fbpdf: fbval_t doesn't match fb depth\n");
		else
			mainloop();
		pthread_kill(mouse_thread, SIGINT);
		fb_free();
		free(pbuf);
		if (doc)
			doc_close(doc);
	}
	return 0;
}
