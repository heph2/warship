#define _POSIX_C_SOURCE 200809L // termios/unistd under -std=c11

#include "ui.h"

#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>

// Every frame is positioned with explicit ESC[row;colH and contains no
// newlines, so the terminal can never scroll and OPOST stays irrelevant.
#define GRID_ROW 3               // 1-indexed screen row of grid line 0
#define LEFT_COL 4               // leaves room for the row labels
#define GRID_W (SIZE * 3)        // each cell renders as " x "
#define RIGHT_COL (LEFT_COL + GRID_W + 6)
#define STATUS_ROW (GRID_ROW + SIZE + 1)
#define HELP_ROW (STATUS_ROW + 1)

#define RESET "\x1b[0m"
#define RED "\x1b[31m"
#define GREEN "\x1b[32m"
#define BLUE "\x1b[34m"
#define MAGENTA "\x1b[35m"
#define REVERSE "\x1b[7m"

static struct termios orig_termios;

static void restore_terminal(void) {
  (void)!write(STDOUT_FILENO, "\x1b[?25h\x1b[2J\x1b[H", 13);
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

// tcsetattr and write are async-signal-safe, so restoring from a handler is
// legal. Without this, Ctrl-C during a network wait would leave the terminal in
// raw mode with no cursor.
static void on_signal(int sig) {
  restore_terminal();
  _exit(128 + sig);
}

void ui_init(void) {
  // Save the original settings first so atexit can always put them back.
  if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) {
    perror("tcgetattr");
    exit(1);
  }
  atexit(restore_terminal);

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  struct termios raw = orig_termios;
  // ECHO: don't print typed keys. ICANON: deliver bytes without waiting for
  // Enter. ISIG stays ON so Ctrl-C still works while we are blocked waiting for
  // an opponent -- the handler above puts the terminal back.
  raw.c_lflag &= ~(ECHO | ICANON);
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
    perror("tcsetattr");
    exit(1);
  }
  (void)!write(STDOUT_FILENO, "\x1b[?25l", 6); // hide hardware cursor
}

// Read one byte, but only if one is already waiting. Distinguishing an arrow
// key from someone pressing Escape means asking "is there more?", and the
// answer has to time out or a bare Escape would hang the game.
static int ui_key_pending(int timeout_ms) {
  struct pollfd p = {.fd = STDIN_FILENO, .events = POLLIN};
  if (poll(&p, 1, timeout_ms) != 1)
    return -1;
  unsigned char c;
  return (read(STDIN_FILENO, &c, 1) == 1) ? (int)c : -1;
}

int ui_key(void) {
  unsigned char c;
  if (read(STDIN_FILENO, &c, 1) != 1)
    return -1;
  if (c != 0x1b)
    return c;

  // ESC [ A  in normal mode, ESC O A in application cursor mode. Terminals
  // use both, and which one you get depends on the terminal's current state
  // rather than on anything we control, so accept either.
  int second = ui_key_pending(30);
  if (second != '[' && second != 'O')
    return 0x1b; // a real Escape, or something we do not handle

  switch (ui_key_pending(30)) {
  case 'A': return UI_KEY_UP;
  case 'B': return UI_KEY_DOWN;
  case 'C': return UI_KEY_RIGHT;
  case 'D': return UI_KEY_LEFT;
  default:  return 0x1b;
  }
}

// Bounded append, never writes past cap-1. Returns the new length.
static int append(char *buf, int len, int cap, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf + len, (size_t)(cap - len), fmt, ap);
  va_end(ap);
  if (n < 0)
    return len;
  return (len + n > cap - 1) ? cap - 1 : len + n;
}

// Title, A..J header and the 1..10 row labels for a grid at `origin`.
static int chrome(char *buf, int n, int cap, int origin, const char *title) {
  n = append(buf, n, cap, "\x1b[%d;%dH%s", GRID_ROW - 2, origin, title);
  n = append(buf, n, cap, "\x1b[%d;%dH", GRID_ROW - 1, origin);
  for (int c = 0; c < SIZE; c++)
    n = append(buf, n, cap, " %c ", 'A' + c);
  for (int r = 0; r < SIZE; r++)
    n = append(buf, n, cap, "\x1b[%d;%dH%2d", GRID_ROW + r, origin - 3, r + 1);
  return n;
}

static int at(char *buf, int n, int cap, int row, int origin) {
  return append(buf, n, cap, "\x1b[%d;%dH", GRID_ROW + row, origin);
}

// One cell of my own board: ships visible, enemy shots overlaid.
static int own_cell(char *buf, int n, int cap, const Board *b, int r, int c) {
  int ship = b->cell[r][c];
  if (b->shot[r][c])
    return append(buf, n, cap, "%s %c " RESET, ship ? RED : BLUE,
                  ship ? 'X' : 'o');
  return append(buf, n, cap, " %c ", ship ? '#' : '.');
}

void ui_draw_placement(const Board *b, int row, int col, int vert) {
  char buf[8192];
  const int cap = (int)sizeof buf;
  int n = 0;

  int idx = b->placed;
  int len = (idx < NSHIPS) ? SHIP_LEN[idx] : 0;
  int ok = len && board_can_place(b, row, col, len, vert);

  n = append(buf, n, cap, "\x1b[H\x1b[2J");
  n = chrome(buf, n, cap, LEFT_COL, "YOUR FLEET");

  for (int r = 0; r < SIZE; r++) {
    n = at(buf, n, cap, r, LEFT_COL);
    for (int c = 0; c < SIZE; c++) {
      if (len && ship_covers(r, c, row, col, len, vert))
        n = append(buf, n, cap, "%s # " RESET, ok ? GREEN : RED);
      else
        n = append(buf, n, cap, " %c ", b->cell[r][c] ? '#' : '.');
    }
  }

  if (idx < NSHIPS)
    n = append(buf, n, cap, "\x1b[%d;1Hplacing %s (%d)  at %c%d", STATUS_ROW,
               SHIP_NAME[idx], len, 'A' + col, row + 1);
  else
    n = append(buf, n, cap, "\x1b[%d;1Hfleet ready -- press enter to fight",
               STATUS_ROW);

  n = append(buf, n, cap,
             "\x1b[%d;1Hwasd/arrows move  r rotate  space place  u undo  q quit",
             HELP_ROW);

  (void)!write(STDOUT_FILENO, buf, (size_t)n);
}

void ui_draw_battle(const Board *own, const Track *t, int row, int col,
                    const char *status) {
  char buf[8192];
  const int cap = (int)sizeof buf;
  int n = 0;

  n = append(buf, n, cap, "\x1b[H\x1b[2J");
  n = chrome(buf, n, cap, LEFT_COL, "YOUR FLEET");
  n = chrome(buf, n, cap, RIGHT_COL, "ENEMY WATERS");

  for (int r = 0; r < SIZE; r++) {
    n = at(buf, n, cap, r, LEFT_COL);
    for (int c = 0; c < SIZE; c++)
      n = own_cell(buf, n, cap, own, r, c);

    n = at(buf, n, cap, r, RIGHT_COL);
    for (int c = 0; c < SIZE; c++) {
      int here = (r == row && c == col);
      const char *color = "", *mark = " . ";
      switch (t->cell[r][c]) {
      case TRACK_UNKNOWN: break;
      case TRACK_MISS: color = BLUE;    mark = " o "; break;
      case TRACK_HIT:  color = RED;     mark = " X "; break;
      case TRACK_SUNK: color = MAGENTA; mark = " # "; break;
      }
      n = append(buf, n, cap, "%s%s%s%s", here ? REVERSE : "", color, mark,
                 (here || *color) ? RESET : "");
    }
  }

  n = append(buf, n, cap, "\x1b[%d;1H%s", STATUS_ROW, status);
  n = append(buf, n, cap,
             "\x1b[%d;1Hwasd/arrows move  space fire  q quit   [%c%d]",
             HELP_ROW, 'A' + col, row + 1);

  (void)!write(STDOUT_FILENO, buf, (size_t)n);
}

void ui_message(const char *title, const char *body) {
  char buf[2048];
  const int cap = (int)sizeof buf;
  int n = 0;
  n = append(buf, n, cap, "\x1b[H\x1b[2J");
  n = append(buf, n, cap, "\x1b[%d;%dH%s", GRID_ROW + 2, LEFT_COL, title);
  if (body)
    n = append(buf, n, cap, "\x1b[%d;%dH%s", GRID_ROW + 4, LEFT_COL, body);
  (void)!write(STDOUT_FILENO, buf, (size_t)n);
}
