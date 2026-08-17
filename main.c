#include <stdio.h>
#include <stdlib.h>

#define SIZE 10

void print_grid(int (*grid)[10], int x, int y) {
  for (x = 0; x < SIZE; x++) {
    for (y = 0; y < SIZE; y++)
      printf(" %d ", grid[x][y]);
    putchar('\n');
  }
}

int main(void) {
  int x, y;
  int grid[SIZE][SIZE];

  for (x = 0; x < SIZE; x++) {
    for (y = 0; y < SIZE; y++) {
      grid[x][y] = 0;
    }
  }

  print_grid(grid, x, y);

  return 0;
}
