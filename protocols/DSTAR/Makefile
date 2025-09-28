IDIR=.

CC=g++
CFLAGS=-I$(IDIR)

TOOLS_ODIR=../../tools

LIBS=-lm -lpthread -lmariadb

_OBJ=dstar_host.o
tOBJ=tools.o CCITTChecksumReverse.o
OBJ=$(patsubst %,$(TOOLS_ODIR)/%,$(tOBJ)) $(patsubst %,./%,$(_OBJ))

$(TOOLS_ODIR)/%.o: %.cpp
	$(CC) -c -o $@ $< $(CFLAGS)

DSTAR_Service: $(OBJ)
	$(CC) -o $@ $^ $(CFLAGS) $(LIBS)

.PHONY: clean

clean:
	rm -f $(TOOLS_ODIR)/*.o $(INCDIR)/*~
