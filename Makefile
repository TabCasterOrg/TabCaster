CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS = -lX11 -lXrandr

SRCDIR = .
BUILDDIR = build
SRCS = main.c display_manager.c
OBJS = $(SRCS:%.c=$(BUILDDIR)/%.o)
TARGET = $(BUILDDIR)/tabcaster

all: $(TARGET)

# Create build directory if it doesn't exist
$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(LDFLAGS)

$(BUILDDIR)/%.o: %.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILDDIR)

install: $(TARGET)
	cp $(TARGET) /usr/local/bin/

.PHONY: all clean install