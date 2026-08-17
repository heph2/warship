CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -g
TARGET = warship

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $(TARGET) main.c

clean:
	rm -f $(TARGET)

.PHONY: clean
