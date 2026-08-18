#include "board.h"

const int SHIP_LEN[NSHIPS] = {5, 4, 3, 3, 2};
const char *const SHIP_NAME[NSHIPS] = {"Carrier", "Battleship", "Cruiser",
                                       "Submarine", "Destroyer"};

int ship_covers(int row, int col, int bow_row, int bow_col, int len, int vert) {
  return vert ? (col == bow_col && row >= bow_row && row < bow_row + len)
              : (row == bow_row && col >= bow_col && col < bow_col + len);
}

// Bounds + overlap. Checked here rather than relying on the cursor clamp,
// because phase 1 will validate a placement that arrived over the wire.
int board_can_place(const Board *b, int row, int col, int len, int vert) {
  if (row < 0 || col < 0)
    return 0;
  if (vert ? (row + len > SIZE) : (col + len > SIZE))
    return 0;
  for (int i = 0; i < len; i++)
    if (b->cell[vert ? row + i : row][vert ? col : col + i])
      return 0;
  return 1;
}

void board_place(Board *b, int idx, int row, int col, int vert) {
  int len = SHIP_LEN[idx];
  for (int i = 0; i < len; i++)
    b->cell[vert ? row + i : row][vert ? col : col + i] = idx + 1;
  b->fleet[idx] = (Ship){
      .len = len, .hits = 0, .row = row, .col = col, .vertical = vert};
}

void board_unplace(Board *b, int idx) {
  Ship s = b->fleet[idx];
  for (int i = 0; i < s.len; i++)
    b->cell[s.vertical ? s.row + i : s.row][s.vertical ? s.col : s.col + i] = 0;
}

Fire board_fire(Board *b, int row, int col, int *ship_idx) {
  *ship_idx = -1;
  if (row < 0 || row >= SIZE || col < 0 || col >= SIZE)
    return FIRE_REJECT;
  // Reject before mutating: a resent packet must not count the hit twice.
  if (b->shot[row][col])
    return FIRE_REJECT;

  b->shot[row][col] = 1;
  int id = b->cell[row][col];
  if (!id)
    return FIRE_MISS;

  *ship_idx = id - 1;
  Ship *s = &b->fleet[id - 1];
  s->hits++;
  return (s->hits == s->len) ? FIRE_SUNK : FIRE_HIT;
}

int board_all_sunk(const Board *b) {
  for (int i = 0; i < NSHIPS; i++) {
    // An unplaced ship has len 0, which would otherwise satisfy hits >= len and
    // report an empty board as a total defeat.
    if (b->fleet[i].len == 0 || b->fleet[i].hits < b->fleet[i].len)
      return 0;
  }
  return 1;
}

void track_mark(Track *t, int row, int col, Fire f, int ship_idx) {
  switch (f) {
  case FIRE_MISS: t->cell[row][col] = TRACK_MISS; break;
  case FIRE_HIT:  t->cell[row][col] = TRACK_HIT;  break;
  case FIRE_SUNK:
    t->cell[row][col] = TRACK_SUNK;
    if (ship_idx >= 0)
      t->sunk[ship_idx] = 1;
    break;
  case FIRE_REJECT: break; // nothing happened, nothing to record
  }
}
