#ifndef UI_H
#define UI_H

#include "board.h"

void ui_init(void); // raw mode + hidden cursor; restores itself via atexit
int ui_key(void);   // blocking single byte, -1 on EOF

void ui_draw_placement(const Board *b, int row, int col, int vert);
void ui_draw_battle(const Board *own, const Track *t, int row, int col,
                    const char *status);

// Full-screen notice: connecting, waiting for an opponent, fatal errors.
void ui_message(const char *title, const char *body);

#endif
