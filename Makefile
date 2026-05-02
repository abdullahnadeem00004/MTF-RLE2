# Makefile – BZip2 Phase 2
CC      = gcc
CFLAGS  = -Wall -Wextra -Wpedantic -std=c11 -O2 -g
TARGET  = bzip2_phase2
SRCS    = main.c config.c block.c rle1.c bwt.c mtf.c rle2.c
OBJS    = $(SRCS:.c=.o)

.PHONY: all clean test sample verify

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c bzip2.h
	$(CC) $(CFLAGS) -c -o $@ $<

verify: verify.c config.o block.o rle1.o bwt.o mtf.o rle2.o
	$(CC) $(CFLAGS) -o verify verify.c config.o block.o rle1.o bwt.o mtf.o rle2.o

# Quick smoke-test with generated file
test: $(TARGET)
	python3 -c "import random,string; d='AAAA'*20+'BBBBBB'*10+'CD'*50+''.join(random.choices(string.ascii_letters,k=200)); open('test_input.txt','w').write(d); print('Generated',len(d),'bytes')"
	./$(TARGET) test_input.txt

# Run on the provided sample_input.txt
sample: $(TARGET) verify
	./$(TARGET) sample_input.txt
	./verify sample_input.txt

clean:
	rm -f $(OBJS) verify.o $(TARGET) verify test_input.txt test_input.txt_phase2.bwt sample_input.txt_phase2.bwt test_input.txt_phase4.mtf sample_input.txt_phase4.mtf test_input.txt_phase5.rle2 sample_input.txt_phase5.rle2
