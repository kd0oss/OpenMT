#!/bin/sh
# Created by Rick KD0OSS
#
# openmt_flash.sh - update the modem firmware over the Raspberry Pi serial
# port with the chip's built-in ROM boot loader (needs stm32flash:
#     sudo apt install stm32flash ).
#
#   openmt_flash.sh [options] firmware.bin
#
#   -m soft|gpio   soft : ask the running firmware to reboot into the boot
#                         loader (F105 boards without BOOT0 / NRST wires)
#                  gpio : drive BOOT0 and NRST from Pi GPIO pins with pinctrl
#                  seq  : let stm32flash drive them (-i), the way the Pi-Star /
#                         WPSD modem upgrade tool does (sysfs pin numbers with
#                         the gpiochip offset worked out here); stm32flash's
#                         own default baud rate is used unless -b is given
#                  default: soft
#   -p PORT        serial port, default /dev/ttyAMA0 (Pi header pins 8 / 10)
#   -b BAUD        boot loader baud rate, default 115200 (8E1)
#   -B GPIO        BOOT0 GPIO number, default 20 (header pin 38)
#   -R GPIO        NRST  GPIO number, default 21 (header pin 40)
#   -a ADDR        flash address of the image, default 0x08000000
#   -L             the modem has the serial loader (F105 built with LOADER=1):
#                  image address 0x08001800.  Use the application .bin
#                  (openmt_sample_f105.bin), not the _full one.
#   -r FILE        READ the flash into FILE (a backup) instead of writing;
#                  needs -l.  Reading works only while the chip's read
#                  protection (RDP) is off.  Example, F446 with 256 KB:
#                    openmt_flash.sh -m gpio -r backup.bin -l 262144
#   -l BYTES       number of bytes to read with -r (the flash size)
#   -x PATH        stm32flash program to use (default: stm32flash)
#   -n             dry run: show what would be done, change nothing
#
# Stop the OpenMT / MMDVM host first, it owns the serial port.  The GPIO
# mode also needs BOOT1 (PB2 on the F446) held low on the board.

PORT=/dev/ttyAMA0
MODE=soft
BAUD=115200
BAUDSET=0
STM32FLASH=stm32flash
BOOT0=20
NRST=21
ADDR=0x08000000
DRY=0
LOADER=0
READFILE=
READLEN=

usage()
{
    sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
}

while getopts "m:p:b:B:R:a:r:l:x:nLh" opt; do
    case $opt in
        m) MODE=$OPTARG ;;
        p) PORT=$OPTARG ;;
        b) BAUD=$OPTARG; BAUDSET=1 ;;
        x) STM32FLASH=$OPTARG ;;
        B) BOOT0=$OPTARG ;;
        R) NRST=$OPTARG ;;
        a) ADDR=$OPTARG ;;
        n) DRY=1 ;;
        L) LOADER=1 ;;
        r) READFILE=$OPTARG ;;
        l) READLEN=$OPTARG ;;
        *) usage ;;
    esac
done
shift $((OPTIND - 1))
FW=$1

if [ -n "$READFILE" ]; then
    [ -n "$READLEN" ] || { echo "-r needs -l BYTES (the flash size)" >&2; exit 2; }
    [ "$LOADER" -eq 0 ] || { echo "-r and -L do not mix" >&2; exit 2; }
    FW=$READFILE
fi
[ -n "$FW" ] || usage
SPEC=$ADDR
if [ "$LOADER" -eq 1 ]; then
    ADDR=0x08001800
    SPEC=$ADDR
    # erase only what the image needs (the loader checks the flash size, but
    # stm32flash otherwise assumes the biggest chip of the family)
    if [ -f "$FW" ]; then
        # the reset vector (bytes 4..7) must point above 0x08001800, else the
        # image was linked for 0x08000000 (built without LOADER=1)
        RV=$(od -An -t u4 -j 4 -N 4 "$FW" | tr -d ' ')
        if [ -n "$RV" ] && [ "$RV" -lt $((0x08001800)) ]; then
            echo "$FW is linked for 0x08000000, not for the loader" >&2
            echo "(rebuild with LOADER=1 and use build/f105/openmt_sample_f105.bin)" >&2
            exit 1
        fi
        LEN=$(wc -c < "$FW")
        SPEC=$ADDR:$(( (LEN + 3) / 4 * 4 ))
    fi
fi
if [ "$BAUD" -gt 115200 ] 2>/dev/null; then
    echo "warning: the ROM boot loaders are only reliable up to 115200 baud;" >&2
    echo "         $BAUD usually ends in \"Failed to init device, timeout\"" >&2
fi
if [ -n "$READFILE" ] && [ "$MODE" = soft ]; then
    echo "-r needs -m gpio or -m seq (the running firmware cannot be read through the soft boot)" >&2
    exit 2
fi
case $MODE in soft|gpio|seq) ;; *) echo "mode must be soft, gpio or seq" >&2; exit 2 ;; esac

run()
{
    echo "+ $*"
    if [ "$DRY" -eq 0 ]; then
        "$@"
    fi
}

gpio_out()      # gpio_out NUMBER 0|1
{
    if [ "$DRY" -eq 1 ] || command -v pinctrl >/dev/null 2>&1; then
        if [ "$2" -eq 1 ]; then run pinctrl set "$1" op dh; else run pinctrl set "$1" op dl; fi
    elif command -v raspi-gpio >/dev/null 2>&1; then
        if [ "$2" -eq 1 ]; then run raspi-gpio set "$1" op dh; else run raspi-gpio set "$1" op dl; fi
    else
        echo "need pinctrl or raspi-gpio to drive GPIO $1" >&2
        exit 1
    fi
}

if [ "$DRY" -eq 0 ]; then
    command -v "$STM32FLASH" >/dev/null 2>&1 || [ -x "$STM32FLASH" ] || { echo "$STM32FLASH not found" >&2; exit 1; }
    if [ -z "$READFILE" ]; then [ -f "$FW" ] || { echo "$FW not found" >&2; exit 1; }; fi
    # Writing to a name that is not a device would silently create a plain
    # file in /dev, so insist on a real character device first.
    if [ -e "$PORT" ] && [ ! -c "$PORT" ]; then
        echo "$PORT exists but is not a serial device (a stray file?)." >&2
        echo "Remove it with:  sudo rm $PORT   and check the port name." >&2
        exit 1
    fi
    if [ ! -c "$PORT" ]; then
        echo "$PORT does not exist. Serial ports here:" >&2
        ls /dev/serial* /dev/ttyAMA* /dev/ttyS* 2>/dev/null >&2
        echo "Enable the header UART first (see README), or pass -p PORT." >&2
        exit 1
    fi
    if command -v fuser >/dev/null 2>&1 && fuser "$PORT" >/dev/null 2>&1; then
        echo "$PORT is in use - stop the OpenMT / MMDVM host first" >&2
        exit 1
    fi
fi

# seq mode: the sysfs GPIO number of a BCM pin is the number of its gpiochip
# plus the pin (the same working out as the Pi-Star / WPSD upgrade script;
# the Pi 5 chip, gpiochip504, counts from 0)
SEQ=
if [ "$MODE" = seq ]; then
    BASE=$(ls -la /sys/class/gpio/gpiochip* 2>/dev/null | grep "0000.gpio" | sed -n "s/.*gpiochip\([0-9]*\).*/\1/p" | tail -n 1)
    [ -n "$BASE" ] || BASE=0
    [ "$BASE" = 504 ] && BASE=0
    P0=$((BASE + BOOT0))
    PR=$((BASE + NRST))
    SEQ="$P0,-$PR,$PR:-$P0,$PR"
    echo "seq mode: BOOT0 = GPIO $BOOT0 (sysfs $P0), NRST = GPIO $NRST (sysfs $PR)"
fi

if [ "$MODE" = soft ]; then
    # 61 07 D1 'B' 'O' 'O' 'T' at the firmware's normal 921600 baud
    run stty -F "$PORT" 921600 raw -echo cs8 -parenb -cstopb || exit 1
    echo "+ send 61 07 D1 'BOOT' to $PORT"
    if [ "$DRY" -eq 0 ]; then
        printf '\141\007\321BOOT' > "$PORT"
    fi
    run sleep 0.5
elif [ "$MODE" = gpio ]; then
    gpio_out "$BOOT0" 1
    gpio_out "$NRST" 0
    run sleep 0.1
    gpio_out "$NRST" 1
    run sleep 0.3
fi

# the boot loader talks 8 data bits, even parity, 1 stop bit
if [ "$MODE" = soft ]; then
    run "$STM32FLASH" -b "$BAUD" -S "$SPEC" -w "$FW" -v -g "$ADDR" "$PORT"
    STATUS=$?
elif [ "$MODE" = seq ]; then
    BOPT=
    [ "$BAUDSET" -eq 1 ] && BOPT="-b $BAUD"
    # like the Pi-Star / WPSD tool does for the MMDVM repeater boards: one
    # call to run the pin sequence and wake the boot loader (the chip stays in
    # it, BOOT0 is released without a reset), then the real work
    run "$STM32FLASH" $BOPT -i "$SEQ" "$PORT"
    if [ -n "$READFILE" ]; then
        run "$STM32FLASH" $BOPT -S "$ADDR:$READLEN" -r "$READFILE" -R "$PORT"
    else
        run "$STM32FLASH" $BOPT -S "$ADDR" -w "$FW" -v -g "$ADDR" -R "$PORT"
    fi
    STATUS=$?
else
    if [ -n "$READFILE" ]; then
        run "$STM32FLASH" -b "$BAUD" -S "$ADDR:$READLEN" -r "$READFILE" "$PORT"
    else
        run "$STM32FLASH" -b "$BAUD" -S "$ADDR" -w "$FW" -v "$PORT"
    fi
    STATUS=$?
    # leave the boot loader: BOOT0 low, pulse reset
    gpio_out "$BOOT0" 0
    gpio_out "$NRST" 0
    run sleep 0.1
    gpio_out "$NRST" 1
fi

if [ "$STATUS" -ne 0 ]; then
    echo "flashing failed (status $STATUS)" >&2
    exit "$STATUS"
fi
echo "done"
