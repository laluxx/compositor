CC = gcc
CFLAGS = -Wall -g
LIBS = -lX11 -lXcomposite -lXdamage -lXrender -lXext -lXfixes -lm


SRC = main.c
OBJ = $(SRC:.c=.o)
TARGET = compositor

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f $(OBJ) $(TARGET)
