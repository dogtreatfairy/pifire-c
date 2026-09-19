PIFIRE_INCLUDE ?= ../../include
CFLAGS ?= -O2 -Wall -Wextra

bangbang.so: example.c
	$(CC) -shared -fPIC $(CFLAGS) -I$(PIFIRE_INCLUDE) -o $@ $<

install: bangbang.so
	install -d /usr/lib/pifire/controllers
	install -m 644 bangbang.so /usr/lib/pifire/controllers/

clean:
	rm -f bangbang.so

.PHONY: install clean
