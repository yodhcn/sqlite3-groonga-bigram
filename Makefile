CC=gcc
CFLAGS=-g -O0 -Wall -I. -fPIC

GROONGA_CFLAGS=$(shell pkg-config --cflags groonga 2>/dev/null)
GROONGA_LIBS=$(shell pkg-config --libs groonga 2>/dev/null)
SQLITE_CFLAGS=$(shell pkg-config --cflags sqlite3 2>/dev/null)

CFLAGS += $(GROONGA_CFLAGS) $(SQLITE_CFLAGS)
LDFLAGS = $(GROONGA_LIBS)

EXT_SRC = main.c
EXT_TARGET = libbigram.so

compile_commands.json:
	@echo '[' > $@
	@echo '  {' >> $@
	@echo '    "directory": "$(PWD)",' >> $@
	@echo '    "command": "$(CC) $(CFLAGS) -shared $(EXT_SRC) -o $(EXT_TARGET) $(LDFLAGS)",' >> $@
	@echo '    "file": "$(EXT_SRC)"' >> $@
	@echo '  }' >> $@
	@echo ']' >> $@

extension: clean compile_commands.json
	@echo "Compiling SQLite extension with: $(CC) $(CFLAGS)"
	$(CC) $(CFLAGS) -shared $(EXT_SRC) -o $(EXT_TARGET) $(LDFLAGS)
	@echo "SQLite extension built: ./$(EXT_TARGET)"

clean:
	rm -f $(EXT_TARGET) compile_commands.json

.PHONY: clean extension
