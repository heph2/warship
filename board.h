#ifndef BOARD_H
#define BOARD_H

// Pure game rules. No printing, no sockets, no globals -- everything hangs off
// a Board* so tests can build one from scratch and so phase 1 can hold two.

#define SIZE 10
#define NSHIPS 5

extern const int SHIP_LEN[NSHIPS];
extern const char *const SHIP_NAME[NSHIPS];

typedef struct {
  int len;
  int hits;
  int row, col; // bow
  int vertical;
} Ship;

// My own board: where my ships are, and what the enemy has fired at.
typedef struct {
  int cell[SIZE][SIZE]; // 0 = water, else ship index + 1
  int shot[SIZE][SIZE]; // 1 = already fired at
  Ship fleet[NSHIPS];
  int placed; // ships committed so far; NSHIPS == ready
} Board;

// Outcome of one shot. These are also the values that go on the wire.
typedef enum {
  FIRE_REJECT = -1, // out of range or already fired: costs no turn
  FIRE_MISS = 0,
  FIRE_HIT = 1,
  FIRE_SUNK = 2
} Fire;

// The enemy board as *we* see it: no ship data, only what they reported back.
typedef enum {
  TRACK_UNKNOWN = 0,
  TRACK_MISS,
  TRACK_HIT,
  TRACK_SUNK
} TrackCell;

typedef struct {
  TrackCell cell[SIZE][SIZE];
  int sunk[NSHIPS]; // 1 once the enemy told us this ship went down
} Track;

// Placement
int board_can_place(const Board *b, int row, int col, int len, int vert);
void board_place(Board *b, int idx, int row, int col, int vert);
void board_unplace(Board *b, int idx);
int ship_covers(int row, int col, int bow_row, int bow_col, int len, int vert);

// Combat
Fire board_fire(Board *b, int row, int col, int *ship_idx);
int board_all_sunk(const Board *b);

// Our view of them
void track_mark(Track *t, int row, int col, Fire f, int ship_idx);

#endif
