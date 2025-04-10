CC = gcc
CFLAGS = -Wall -O2
LDFLAGS = -lpthread -lncurses -lpanel -lmenu -lform

all: browser tab

browser: browser.c shared_memory.c common.h shared_memory.h
	$(CC) $(CFLAGS) browser.c shared_memory.c -o browser $(LDFLAGS)

tab: tab.c shared_memory.c common.h shared_memory.h
	$(CC) $(CFLAGS) tab.c shared_memory.c -o tab $(LDFLAGS)

clean:
	rm -f browser tab /tmp/browser_fifo /tmp/tab_response_*

.PHONY: all clean

