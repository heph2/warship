#ifndef UI_H
#define UI_H

#include "board.h"

// Arrow keys arrive as multi-byte escape sequences, so they cannot be single
// bytes. Values above 255 keep them distinct from every real character.
#define UI_KEY_UP 1000
#define UI_KEY_DOWN 1001
#define UI_KEY_RIGHT 1002
#define UI_KEY_LEFT 1003

void ui_init(void); // raw mode + hidden cursor; restores itself via atexit
int ui_key(void);   // blocking; one key, -1 on EOF

void ui_draw_placement(const Board *b, int row, int col, int vert);
void ui_draw_battle(const Board *own, const Track *t, int row, int col,
                    const char *status);

// Full-screen notice: connecting, waiting for an opponent, fatal errors.
void ui_message(const char *title, const char *body);

#endif
