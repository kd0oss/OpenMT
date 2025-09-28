#IMBE_IDIR=../../openrtx/src/imbe_vocoder
M17_IDIR=..
IDIR=.

CC=g++
CFLAGS=-I$(IDIR) -I$(M17_IDIR)

#IMBE_ODIR=imbe_vocoder
M17_ODIR=../M17
TOOLS_ODIR=../../../tools
RTX_ODIR=.

#LDIR=../../lib

LIBS=-lm -lpthread -lmariadb

#IMBE=aux_sub.cc basicop2.cc ch_decode.cc ch_encode.cc dc_rmv.cc \
#     decode.cc dsp_sub.cc encode.cc imbe_vocoder.cc imbe_vocoder_impl.cc \
#     math_sub.cc pe_lpf.cc pitch_est.cc pitch_ref.cc \
#     qnt_sub.cc rand_gen.cc sa_decode.cc sa_encode.cc \
#     sa_enh.cc tbls.cc uv_synt.cc v_synt.cc v_uv_det.cc

M17=M17Callsign.cpp M17DSP.cpp M17FrameDecoder.cpp M17FrameEncoder.cpp M17LinkSetupFrame.cpp M17Golay.cpp M17Callsign.cpp

_OBJ=m17gateway.o $(M17)
tOBJ=tools.o
OBJ=$(patsubst %,$(TOOLS_ODIR)/%,$(tOBJ)) $(patsubst %,$(M17_ODIR)/%,$(_OBJ))


$(M17_ODIR)/%.o: %.cpp #$(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

$(TOOLS_ODIR)/%.o: %.cpp #$(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

M17_gateway: $(OBJ)
	$(CC) -o $@ $^ $(CFLAGS) $(LIBS)

.PHONY: clean

clean:
	rm -f $(M17_ODIR)/*.o $(TOOLS_ODIR)/*.o $(INCDIR)/*~ 

