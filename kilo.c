/* KILO EDITOR IMPLEMENTED FROM THIS GUIDE: https://viewsourcecode.org/snaptoken/kilo/03.rawInputAndOutput.html
 *
 * TO FIND OUT HOW THIS SHIT WORKS */

/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/

#define KILO_VERSION "0.0.1"

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
	ARROW_LEFT = 1000,
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN,
	HOME_KEY,
	END_KEY,
	PAGE_UP,
	PAGE_DOWN
};

/*** data ***/

struct editorConfig {
	int cx, cy;
	int screenrows;
	int screencols;
	struct termios orig_termios;
};

struct editorConfig E;

/*** terminal ***/

void die(const char *s) {
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[2J", 3);

	perror(s);
	exit(1);
}

void disableRawMode() {
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
		die("tcsetattr");
}

void enableRawMode() {
	if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
	atexit(disableRawMode);

	struct termios raw = E.orig_termios;

	// fildes = file descriptor
	// STDIN_FILENO should be 0
	// &raw is the pointer of the termios struct created previously
	// tcgetattr(int fildes, struct termios *termios_p);
	
	// ICRNL interpret \r as \n, IXON ctrl-s/q XOFF/XON
	// ECHO, ICANON canonical mode, IEXTEN send char literally, ISIG signal interrupt
	// BRKINT, INPCK, ISTRIP, CS8 are apparently outdated flags
	// BRKINT causes a signal interrupt on a break condition
	// INPCK enables parity checking whatever that is (no longer applies apparently)
	// ISTRIP causes the 8th bit of each input to be stripped (set to 0)
	// CS8 is a bit mask which sets the character size to 8 bits per byte
	// c_lflag is a member of the termios struct generally controlling higher-level aspects of input processing
	// than the input modes flags such as echoing, signals, the choice of canonical/noncanonical input
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON); // c_iflag input flags c_lflag kinda like misc. flags
	raw.c_oflag &= ~(OPOST); // disable output processing. in practice just \n to \r\n
	raw.c_cflag |= (CS8);
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG); 
	// &= bitwise AND and assign bitwise NOT(ECHO) to raw.c_lflag, turn off echo
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;

	// TCSAFLUSH, the change shall occur after all output written to fildes is transmitted and all input so far received
	// but not read shall be discared before the change is made.
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr"); // tcsetattr(int fildes, int option_actions, const struct termios *termios_p);:w
	
}

int editorReadKey() {
	int nread;
	char c;
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
		if (nread == -1 && errno != EAGAIN) die("read");
	}

	if (c== '\x1b') {
		char seq[3];

		if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
		if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

		if (seq[0] == '[') {
			if (seq[0] == '[') {
				if (seq[1] >= '0' && seq[1] <= '9') {
					if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
					if (seq[2] == '~') {
						switch (seq[1]) {
							case '5': return PAGE_UP;
							case '6': return PAGE_DOWN;
						}
					}
				} else {
					switch (seq[1]) {
						case 'A': return ARROW_UP;
						case 'B': return ARROW_DOWN;
						case 'C': return ARROW_RIGHT;
						case 'D': return ARROW_LEFT;
					}
				}
			}
		return '\x1b';
	} else {
		return c;
	}
	}
}

int getCursorPosition(int *rows, int *cols) {
	char buf[32];
	unsigned int i = 0;

	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;
	
	while (i < sizeof(buf) - 1) {
		if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
		if (buf[i] == 'R') break;
		i++;
	}
	buf[i] = '\0';
	
	if (buf[0] != '\x1b' || buf[1] != '[') return -1;
	if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

	return 0;
}

int getWindowSize(int *rows, int *cols) {
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
		return getCursorPosition(rows, cols);
	} else {
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}

/*** append buffer ***/

struct abuf {
	char *b;
	int len;
};

void abAppend(struct abuf *ab, const char *s, int len) {
	char *new = realloc(ab->b, ab->len + len);

	if (new == NULL) return;
	memcpy(&new[ab->len], s, len);
	ab->b = new;
	ab->len += len;
}

void abFree(struct abuf *ab) {
	free(ab->b);
}
#define ABUF_INIT {NULL, 0}

/*** output ***/

void editorDrawRows(struct abuf *ab) {
	int y;
	for (y = 0; y < E.screenrows; y++) {
		if(y == E.screenrows / 3) {
			char welcome[80];
			int welcomelen = snprintf(welcome, sizeof(welcome),
					"Kilo editor -- version %s", KILO_VERSION);
			if (welcomelen > E.screencols) welcomelen = E.screencols;
			int padding = (E.screencols - welcomelen) / 2;
			if (padding) {
				abAppend(ab, "~", 1);
				padding--;
			}
			while (padding--) abAppend(ab, " ", 1);
			abAppend(ab, welcome, welcomelen);
		} else {
			abAppend(ab, "~", 1);
		}
		abAppend(ab, "~", 1);

		abAppend(ab, "\x1b[K", 3);
		if (y < E.screenrows - 1) {
			abAppend(ab, "\r\n", 2);
		}
	}
}

void editorRefreshScreen() {
	struct abuf ab = ABUF_INIT;

	abAppend(&ab, "\x1b[?25l", 6);
	abAppend(&ab, "\x1b[H", 3);

	editorDrawRows(&ab);

	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
	abAppend(&ab, buf, strlen(buf));

	abAppend(&ab, "\x1b[?25h", 6);

	write(STDOUT_FILENO, ab.b, ab.len);
	abFree(&ab);
}

/*** input ***/

void editorMoveCursor(int key) {
	switch (key) {
		case ARROW_LEFT:
			if (E.cx != 0) {
				E.cx--;
			}
			break;
		case ARROW_RIGHT:
			if (E.cx != E.screencols - 1) {
				E.cx++;
			}
			break;
		case ARROW_UP:
			if (E.cy != 0) {
				E.cy--;
			}
			break;
		case ARROW_DOWN:
			if (E.cy != E.screenrows - 1) {
				E.cy++;
			}
			break;
		}
}

void editorProcessKeypress() {
	int c = editorReadKey();
	
	switch(c) {
		case CTRL_KEY('q'):
			write(STDOUT_FILENO, "\x1b[2J", 4);
			write(STDOUT_FILENO, "\x1b[H", 3);
			exit(0);
			break;
		case PAGE_UP:
		case PAGE_DOWN:
			{
				int times = E.screenrows;
				while (times--)
					editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
			}
			break;

		case ARROW_UP:
		case ARROW_DOWN:
		case ARROW_LEFT:
		case ARROW_RIGHT:
			editorMoveCursor(c);
			break;
	}
}

/*** init ***/

void initEditor() {
	E.cx = 0;
	E.cy = 0;

	if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main() {
	enableRawMode();
	initEditor();

	while (1) {
		editorRefreshScreen();
		editorProcessKeypress();	
	}
	return 0;
}
