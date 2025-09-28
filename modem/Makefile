IDIR = .

CC=g++ -g
CFLAGS=-I$(IDIR) -I../tools

LDIR =../lib

TOOLS_ODIR=../tools

LIBS=-lm -lpthread -lmariadb
tOBJ=tools.o
OBJ = Modem_Host.o mmdvm.o $(patsubst %,$(TOOLS_ODIR)/%,$(tOBJ))

Modem_Host: $(OBJ)
	$(CC) -o $@ $^ $(CFLAGS) $(LIBS)

.PHONY: clean

clean:
	rm -f *.o  $(TOOLS_ODIR)/*.o 

