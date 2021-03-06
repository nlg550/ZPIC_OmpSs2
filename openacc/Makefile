# GCC options
CC = pgcc
CFLAGS = -acc -ta=tesla:managed -Minfo=acc -O3 -fast -mp -DENABLE_ADVISE -DTEST

INCLUDES = 
LDFLAGS = -lm -lcuda
SOURCE = current.c emf.c particles.c random.c timer.c main.c simulation.c zdf.c utilities.c region.c 
TARGET = zpic

all : $(TARGET)

prefetch : CFLAGS += -DENABLE_PREFETCH 
prefetch : $(TARGET)

valgrind: $(SOURCE) $(KERNELS)
	gcc $^ $(INCLUDES) -o $(TARGET) -O3 -std=c99 -g $(LDFLAGS)
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes --verbose --log-file=log.txt ./$(TARGET) 2

$(TARGET) : $(SOURCE:.c=.o)
	$(CC) $^ -o $@ $(CFLAGS) $(INCLUDES) $(LDFLAGS)

%.o : %.c
	$(CC) -c $^ -o $@ $(CFLAGS) $(INCLUDES) $(LDFLAGS)

clean:
	@touch $(TARGET) 
	rm -f $(TARGET) *.o *.i
