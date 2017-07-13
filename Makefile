CFLAGS=-Wall -Wextra -march=native -O2
EXEC_NAME=abeep
INSTALL_DIR=/usr/local/bin

default : abeep

clean :
	rm $(EXEC_NAME)

abeep : abeep.c sintable.c sintable.h
	$(CC) $(CFLAGS) -o $(EXEC_NAME) abeep.c sintable.c -lasound

install : abeep
	install -m0755 $(EXEC_NAME) $(INSTALL_DIR)
