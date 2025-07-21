CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS = -lX11 -lXrandr

SRCS = main.c display_manager.c
OBJS = $(SRCS:.c=.o)
TARGET = tabcaster

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean