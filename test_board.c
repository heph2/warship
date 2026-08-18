// Rules-only tests. board.c has no I/O, so nothing here needs a terminal.
#include "board.h"

#include <assert.h>
#include <stdio.h>

static void test_placement(void) {
  Board b = {0};

  assert(board_can_place(&b, 0, 0, 5, 0));
  board_place(&b, 0, 0, 0, 0);
  assert(b.cell[0][0] == 1 && b.cell[0][4] == 1 && b.cell[0][5] == 0);

  assert(!board_can_place(&b, 0, 4, 4, 0)); // overlaps the carrier
  assert(!board_can_place(&b, 0, 7, 4, 0)); // runs off the right edge
  assert(!board_can_place(&b, 7, 0, 4, 1)); // runs off the bottom edge
  assert(board_can_place(&b, 1, 0, 4, 0));  // row below is free

  board_unplace(&b, 0);
  assert(b.cell[0][0] == 0 && board_can_place(&b, 0, 4, 4, 0));
}

static void test_fire(void) {
  Board b = {0};
  board_place(&b, 4, 3, 3, 0); // Destroyer, len 2, at D4-E4
  int idx;

  assert(board_fire(&b, 0, 0, &idx) == FIRE_MISS && idx == -1);
  assert(board_fire(&b, 0, 0, &idx) == FIRE_REJECT); // repeat, even on water
  assert(board_fire(&b, -1, 0, &idx) == FIRE_REJECT);

  assert(board_fire(&b, 3, 3, &idx) == FIRE_HIT && idx == 4);
  assert(board_fire(&b, 3, 3, &idx) == FIRE_REJECT); // resend must not re-hit
  assert(b.fleet[4].hits == 1);
  assert(board_fire(&b, 3, 4, &idx) == FIRE_SUNK && idx == 4);
}

static void test_all_sunk(void) {
  Board b = {0};
  assert(!board_all_sunk(&b)); // empty board is not a loss
  board_place(&b, 0, 0, 0, 0);
  assert(!board_all_sunk(&b)); // nor is a partly placed one
  board_unplace(&b, 0);
  b.fleet[0] = (Ship){0};

  for (int i = 0; i < NSHIPS; i++)
    board_place(&b, i, i, 0, 0);
  assert(!board_all_sunk(&b));

  int idx;
  for (int i = 0; i < NSHIPS; i++)
    for (int c = 0; c < SHIP_LEN[i]; c++)
      board_fire(&b, i, c, &idx);
  assert(board_all_sunk(&b));
}

static void test_track(void) {
  Track t = {0};

  track_mark(&t, 1, 1, FIRE_MISS, -1);
  track_mark(&t, 2, 2, FIRE_HIT, 0);
  track_mark(&t, 2, 3, FIRE_SUNK, 0);
  track_mark(&t, 5, 5, FIRE_REJECT, -1);

  assert(t.cell[1][1] == TRACK_MISS);
  assert(t.cell[2][2] == TRACK_HIT);
  assert(t.cell[2][3] == TRACK_SUNK && t.sunk[0] == 1);
  assert(t.cell[5][5] == TRACK_UNKNOWN); // a rejected shot leaves no trace
  assert(t.sunk[1] == 0);
}

int main(void) {
  test_placement();
  test_fire();
  test_all_sunk();
  test_track();
  puts("all tests ok");
  return 0;
}
