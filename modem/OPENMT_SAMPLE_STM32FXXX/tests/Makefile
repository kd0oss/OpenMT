CC ?= gcc
CFLAGS = -O1 -g -Wall -Wextra -std=gnu99 -I.. -DUNIT_TEST

test_engine: test_engine.c fakehw.c ../engine.c ../clockcalc.c
	$(CC) $(CFLAGS) -DTX_PRIME_SAMPLES=480 $^ -o $@ -lm

run: test_engine
	./test_engine

clean:
	rm -f test_legacy
	rm -f test_engine

LEGACY = -DRX_ADC_SHIFT=0 -DTX_DAC_SHIFT=0 -DRX_LEVEL_UNITY=256 -DTX_LEVEL_UNITY=256 -DRX_DC_BLOCK=0

legacy: test_legacy.c fakehw.c ../engine.c
	$(CC) $(CFLAGS) $(LEGACY) $^ -o test_legacy && ./test_legacy

loader_sim: loader_sim.c ../loader/loader_core.c ../loader/loader.h
	$(CC) -O1 -g -Wall -Wextra -std=gnu99 -I../loader $< ../loader/loader_core.c -o $@

loader: loader_sim
	./loader_sim selftest
