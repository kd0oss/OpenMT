<?php
/***************************************************************************
 *   Copyright (C) 2026 by Rick KD0OSS                                     *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, see <http://www.gnu.org/licenses/>   *
 *                                                                         *
 ***************************************************************************/

/*
 * What the dashboard's Firmware tab can build, and how a request becomes a build.  This file has no database
 * code: data_query.php uses it to describe the options to the page and to check a request, and
 * firmwareBuildRun.php (command line only) uses it to do the build.
 *
 * Nothing from a web request ever reaches a shell or a file as text.  Every option is listed here with the
 * values it may have; a request is turned into
 *   - a block of "#undef" / "#define" lines added to the end of Config.h (OPENMT_HS), and
 *   - make variables and -D options (OPENMT_SAMPLE_STM32FXXX),
 * built only from the listed names and from values that passed the check of their option.
 */

if (PHP_SAPI !== 'cli' && basename($_SERVER['SCRIPT_NAME'] ?? '') === 'firmwareBuild.php')
{
    http_response_code(403);
    exit;
}

const FW_BUILD_LIMIT_S = 900;       /* longest time one build may take */

/*
 * fwPins()
 *
 * Returns the pin choice used by the options of the sample firmware: "" for the default of the chip, or a
 * pin name such as PB12.  The text of the page is made from the same list.
 */
function fwPinList($withNone)
{
    $pins = array('' => 'Default of the chip');
    if ($withNone)
        $pins['none'] = 'Not used';
    foreach (array('A', 'B', 'C', 'D', 'E') as $port)
    {
        for ($n = 0; $n < 16; $n++)
            $pins['P' . $port . $n] = 'P' . $port . $n;
    }
    return $pins;
}

/*
 * fwCatalog()
 *
 * The firmware that can be built.  Every option has an id, a label, a type and what it does to the build:
 *   group  one of several #defines of Config.h ("choices" are the define names; exactly one is wanted)
 *   bool   a #define of Config.h that is on or off
 *   num    a number with a limit, written as "#define NAME value" (Config.h) or -DNAME=value (make)
 *   sel    one of a list of values (for a -D option or a make variable); "" keeps the default of the firmware
 *   pin    a pin name (PA0 ... PE15), or "none" where "none" is listed
 * "make" names a make variable instead of a define; "check" is a note the page shows below the option.
 *
 * Returns the catalog as an array.
 */
function fwCatalog()
{
    $hsBoards = array(
        'ZUMSPOT_ADF7021'         => 'ZUMspot',
        'LIBRE_KIT_ADF7021'       => 'ZUMspot Libre Kit',
        'MMDVM_HS_HAT_REV12'      => 'MMDVM_HS_Hat (rev 1.2)',
        'MMDVM_HS_DUAL_HAT_REV10' => 'MMDVM_HS_Dual_Hat (rev 1.0)',
        'NANO_HOTSPOT'            => 'Nano hotSPOT',
        'NANO_DV_REV10'           => 'NanoDV (rev 1.0)',
        'D2RG_MMDVM_HS'           => 'D2RG MMDVM_HS',
        'SKYBRIDGE_HS'            => 'SkyBridge',
        'LONESTAR_USB'            => 'LoneStar USB stick'
    );

    $hs = array(
        'label'   => 'OPENMT_HS - ADF7021 hotspot board (modem type "openmths")',
        'dir'     => 'OPENMT_HS',
        'kind'    => 'hs',
        'about'   => 'Bit stream firmware for an STM32F103 board with one or two ADF7021. Only the three STM32F103 builds can be made from the files in the project.',
        'targets' => array(
            'hs'   => array('label' => 'No bootloader offset (flash with SWD or stm32flash)', 'files' => array('bin/openmths_f1.hex', 'bin/openmths_f1.bin')),
            'bl'   => array('label' => 'With the 8 KB bootloader offset and USB support',    'files' => array('bin/openmths_f1bl.hex', 'bin/openmths_f1bl.bin')),
            'nobl' => array('label' => 'USB build without the bootloader offset',           'files' => array('bin/openmths_f1nobl.hex', 'bin/openmths_f1nobl.bin'))
        ),
        'options' => array(
            array('id' => 'board',   'label' => 'Board',            'type' => 'group', 'choices' => $hsBoards, 'default' => 'MMDVM_HS_DUAL_HAT_REV10',
                  'help' => 'The pin definitions of the board.'),
            array('id' => 'duplex',  'label' => 'Duplex',           'type' => 'bool', 'define' => 'DUPLEX',
                  'help' => 'Two ADF7021: receive on the second chip while the first transmits.'),
            array('id' => 'tcxo',    'label' => 'TCXO frequency',   'type' => 'group', 'choices' => array('ADF7021_14_7456' => '14.7456 MHz', 'ADF7021_12_2880' => '12.2880 MHz'),
                  'default' => 'ADF7021_14_7456',
                  'help' => 'It must be the same in tools/ADF7021.h, because the services compute the radio registers from it.'),
            array('id' => 'gain',    'label' => 'ADF7021 gain',     'type' => 'group', 'choices' => array('AD7021_GAIN_AUTO' => 'Automatic', 'AD7021_GAIN_AUTO_LIN' => 'Automatic, linear', 'AD7021_GAIN_LOW' => 'Low', 'AD7021_GAIN_HIGH' => 'High'),
                  'default' => 'AD7021_GAIN_AUTO'),
            array('id' => 'link',    'label' => 'Link to the host', 'type' => 'group', 'choices' => array('STM32_USART1_HOST' => 'UART (USART1)', 'STM32_USB_HOST' => 'USB', 'STM32_I2C_HOST' => 'I2C'),
                  'default' => 'STM32_USART1_HOST'),
            array('id' => 'baud',    'label' => 'UART speed',       'type' => 'sel', 'define' => 'OPENMT_SERIAL_SPEED',
                  'choices' => array('' => 'Default (460800)', '115200' => '115200', '230400' => '230400', '460800' => '460800', '921600' => '921600'),
                  'help' => 'It must equal the "baud" setting of the modem in the Modem tab.'),
            array('id' => 'quiet_leds',   'label' => 'Quiet mode LEDs',                 'type' => 'bool', 'define' => 'QUIET_MODE_LEDS'),
            array('id' => 'srv_led',      'label' => 'Constant service LED',            'type' => 'bool', 'define' => 'CONSTANT_SRV_LED'),
            array('id' => 'srv_led_inv',  'label' => 'Constant service LED, inverted',  'type' => 'bool', 'define' => 'CONSTANT_SRV_LED_INVERTED'),
            array('id' => 'dis_led',      'label' => 'Discreet service LED',            'type' => 'bool', 'define' => 'DISCREET_SRV_LED'),
            array('id' => 'dis_led_inv',  'label' => 'Discreet service LED, inverted',  'type' => 'bool', 'define' => 'DISCREET_SRV_LED_INVERTED'),
            array('id' => 'alt_nxdn',     'label' => 'Alternate NXDN LEDs',             'type' => 'bool', 'define' => 'USE_ALTERNATE_NXDN_LEDS'),
            array('id' => 'alt_m17',      'label' => 'Alternate M17 LEDs',              'type' => 'bool', 'define' => 'USE_ALTERNATE_M17_LEDS'),
            array('id' => 'alt_pocsag',   'label' => 'Alternate POCSAG LEDs',           'type' => 'bool', 'define' => 'USE_ALTERNATE_POCSAG_LEDS'),
            array('id' => 'no_band_check', 'label' => 'Switch off the frequency band check', 'type' => 'bool', 'define' => 'DISABLE_FREQ_CHECK',
                  'help' => 'You are responsible for the frequencies you use.'),
            array('id' => 'no_band_ban',   'label' => 'Switch off the banned frequency ranges', 'type' => 'bool', 'define' => 'DISABLE_FREQ_BAN',
                  'help' => 'The ranges are kept free for satellite use. You are responsible for the frequencies you use.'),
            array('id' => 'tx_frames',    'label' => 'TX buffer (host frames)',         'type' => 'num', 'define' => 'OPENMT_TX_BUFFER_FRAMES', 'suffix' => 'U', 'min' => 2, 'max' => 16,
                  'help' => '192 bytes each, 2 x 192 bytes of RAM per frame. Default 8.'),
            array('id' => 'tx_start_bits', 'label' => 'Key up after (bits buffered)',   'type' => 'num', 'define' => 'OPENMT_TX_START_BITS', 'suffix' => 'U', 'min' => 192, 'max' => 12000,
                  'help' => 'Default 3072.'),
            array('id' => 'tx_start_ms',  'label' => 'Key up after (ms)',               'type' => 'num', 'define' => 'OPENMT_TX_START_TIMEOUT_MS', 'suffix' => 'U', 'min' => 20, 'max' => 5000,
                  'help' => 'Default 350.'),
            array('id' => 'rx_bytes',     'label' => 'Received bytes per frame',        'type' => 'num', 'define' => 'OPENMT_RX_FRAME_BYTES', 'suffix' => 'U', 'min' => 4, 'max' => 252,
                  'help' => 'Default 24.'),
            array('id' => 'lock_ms',      'label' => 'Stream after the last sync (ms)', 'type' => 'num', 'define' => 'OPENMT_LOCK_TIMEOUT_MS', 'suffix' => 'U', 'min' => 100, 'max' => 10000,
                  'help' => 'Default 1500.'),
            array('id' => 'rssi',         'label' => 'RSSI threshold (-dBm)',           'type' => 'num', 'define' => 'OPENMT_RSSI_THRESHOLD', 'suffix' => 'U', 'min' => 60, 'max' => 130,
                  'help' => 'A channel weaker than this is skipped while scanning. Default 115.')
        )
    );

    $uart = array('' => 'Default (USART1)', '1' => 'USART1', '2' => 'USART2', '3' => 'USART3', '4' => 'UART4 (F446, F722)', '5' => 'UART5 (F446, F722)', '6' => 'USART6 (F446, F722)');
    $lowhigh = array('' => 'Default', '1' => 'High', '0' => 'Low');

    $sample = array(
        'label'   => 'OPENMT_SAMPLE_STM32FXXX - sample modem (modem type "openmt")',
        'dir'     => 'OPENMT_SAMPLE_SMT32FXXX',
        'kind'    => 'sample',
        'about'   => 'Streams 24 kHz samples between the Pi and the modem\'s ADC and DAC. An option left at its default keeps what config.h says for the chip.',
        'targets' => array('all' => array('label' => 'Application', 'files' => array())),
        'options' => array(
            array('id' => 'mcu',     'label' => 'Chip',         'type' => 'sel', 'make' => 'MCU',
                  'choices' => array('f105' => 'STM32F105 (128 KB / 32 KB)', 'f446' => 'STM32F446 (512 KB / 128 KB)', 'f722' => 'STM32F722 (512 KB / 192 KB)'), 'default' => 'f105', 'required' => true),
            array('id' => 'flash',   'label' => 'Flash (KB)',   'type' => 'num', 'make' => 'FLASH_KB', 'min' => 16, 'max' => 2048,
                  'help' => 'Only if the part differs from the default of the chip (F446RC: 256, F105R8: 64).'),
            array('id' => 'ram',     'label' => 'RAM (KB)',     'type' => 'num', 'make' => 'RAM_KB', 'min' => 8, 'max' => 512,
                  'help' => 'Only if the part differs from the default of the chip (F105R8: 20).'),
            array('id' => 'loader',  'label' => 'Serial loader (F105 only)', 'type' => 'bool', 'make' => 'LOADER', 'onlymcu' => 'f105',
                  'help' => 'Builds the loader and the application behind it as one image.'),
            array('id' => 'loader_led_port', 'label' => 'Loader LED port', 'type' => 'sel', 'make' => 'LOADER_LED_PORT', 'onlymcu' => 'f105', 'needs' => 'loader',
                  'choices' => array('' => 'None', '0' => 'A', '1' => 'B', '2' => 'C', '3' => 'D', '4' => 'E')),
            array('id' => 'loader_led_num',  'label' => 'Loader LED pin', 'type' => 'num', 'make' => 'LOADER_LED_NUM', 'min' => 0, 'max' => 15, 'onlymcu' => 'f105', 'needs' => 'loader'),
            array('id' => 'clock',   'label' => 'Clock source', 'type' => 'sel', 'define' => 'CLOCK_SOURCE',
                  'choices' => array('' => 'Default (crystal)', 'CLOCK_SOURCE_HSE' => 'Crystal or external clock (HSE)', 'CLOCK_SOURCE_HSI' => 'Internal RC (HSI)')),
            array('id' => 'hse_hz',  'label' => 'Crystal frequency (Hz)', 'type' => 'num', 'define' => 'HSE_HZ', 'suffix' => 'UL', 'min' => 3000000, 'max' => 26000000,
                  'help' => 'F105: 3-25 MHz, F446 and F722: 4-26 MHz. Default 8000000.'),
            array('id' => 'hse_byp', 'label' => 'External clock on OSC_IN (TCXO)', 'type' => 'sel', 'define' => 'HSE_BYPASS', 'choices' => array('' => 'Default (no)', '1' => 'Yes', '0' => 'No')),
            array('id' => 'ext_hz',  'label' => 'External sample clock', 'type' => 'sel', 'define' => 'EXT_CLK_HZ', 'suffix' => 'UL',
                  'choices' => array('' => 'Default (19.2 MHz)', '0' => 'None (the MCU clock makes the 24 kHz)', '6144000' => '6.144 MHz', '9600000' => '9.6 MHz',
                                     '12000000' => '12 MHz', '12288000' => '12.288 MHz', '19200000' => '19.2 MHz', '24000000' => '24 MHz'),
                  'help' => 'The clock on the ETR pin of the timer; the timer divides it down to 24 kHz and the ADC and DAC follow it.'),
            array('id' => 'ext_custom', 'label' => 'Other external clock (Hz)', 'type' => 'num', 'define' => 'EXT_CLK_HZ', 'suffix' => 'UL', 'min' => 24000, 'max' => 100000000,
                  'help' => 'Used instead of the choice above when filled in. It need not be a multiple of 24 kHz (the divider is rounded).'),
            array('id' => 'ext_tim', 'label' => 'External clock timer (ETR pin)', 'type' => 'sel', 'define' => 'EXT_CLK_TIM',
                  'choices' => array('' => 'Default (TIM2)', '1' => 'TIM1 (PA12)', '2' => 'TIM2 (PA0, or PA15 with the remap)', '3' => 'TIM3 (PD2)', '4' => 'TIM4 (PE0)')),
            array('id' => 'ext_remap', 'label' => 'TIM2 ETR on PA15', 'type' => 'sel', 'define' => 'EXT_CLK_TIM2_REMAP', 'choices' => array('' => 'Default (no, PA0)', '1' => 'Yes (PA15)', '0' => 'No (PA0)')),
            array('id' => 'ext_inv', 'label' => 'External clock edge', 'type' => 'sel', 'define' => 'EXT_CLK_INVERT', 'choices' => array('' => 'Default (rising)', '0' => 'Rising', '1' => 'Falling')),
            array('id' => 'ext_filter', 'label' => 'External clock input filter', 'type' => 'num', 'define' => 'EXT_CLK_FILTER', 'min' => 0, 'max' => 15,
                  'help' => '0 is off; only for noisy clocks below about 1 MHz.'),
            array('id' => 'ext_fallback', 'label' => 'If the external clock is missing', 'type' => 'sel', 'define' => 'EXT_CLK_FALLBACK',
                  'choices' => array('' => 'Default (use the MCU clock)', '1' => 'Use the MCU clock after 250 ms', '0' => 'Wait for it')),
            array('id' => 'uart',    'label' => 'UART to the Pi', 'type' => 'sel', 'define' => 'UART_NUM', 'choices' => $uart),
            array('id' => 'remap',   'label' => 'UART pin remap', 'type' => 'sel', 'define' => 'UART_REMAP', 'choices' => array('' => 'Default', '0' => 'No', '1' => 'Yes')),
            array('id' => 'uart_baud', 'label' => 'UART speed',   'type' => 'sel', 'define' => 'UART_BAUD', 'suffix' => 'UL',
                  'choices' => array('' => 'Default (921600)', '115200' => '115200', '230400' => '230400', '460800' => '460800', '921600' => '921600', '1000000' => '1000000', '2000000' => '2000000'),
                  'help' => 'It must equal the "baud" setting of the modem in the Modem tab.'),
            array('id' => 'stop',    'label' => 'UART stop bits', 'type' => 'sel', 'define' => 'UART_STOP_BITS', 'choices' => array('' => 'Default (1)', '1' => '1', '2' => '2'),
                  'help' => 'Must match the host (Modem_Host sets 2 stop bits for 2).'),
            array('id' => 'ptt_pin', 'label' => 'PTT pin',       'type' => 'pin', 'define' => 'PTT_PIN'),
            array('id' => 'ptt_high', 'label' => 'PTT active level', 'type' => 'sel', 'define' => 'PTT_ACTIVE_HIGH', 'choices' => $lowhigh),
            array('id' => 'inh_pin', 'label' => 'TX inhibit pin (F446, F722)', 'type' => 'pin', 'define' => 'INHIBIT_PIN', 'none' => true),
            array('id' => 'inh_high', 'label' => 'TX inhibit active level',    'type' => 'sel', 'define' => 'INHIBIT_ACTIVE_HIGH', 'choices' => $lowhigh),
            array('id' => 'adc_pin', 'label' => 'RX audio input pin (ADC)',    'type' => 'pin', 'define' => 'ADC_PIN',
                  'help' => 'Any of PA0-PA7, PB0, PB1, PC0-PC5.'),
            array('id' => 'adc_time', 'label' => 'ADC sample time', 'type' => 'sel', 'define' => 'ADC_SAMPLE_TIME',
                  'choices' => array('' => 'Default of the chip', '0' => '0 (shortest)', '1' => '1', '2' => '2', '3' => '3', '4' => '4', '5' => '5', '6' => '6', '7' => '7 (longest)')),
            array('id' => 'txbuf',   'label' => 'TX buffer (samples)', 'type' => 'num', 'define' => 'TX_BUF_SAMPLES', 'min' => 1024, 'max' => 65536,
                  'help' => 'Default 8192 (F105), 16384 (F446, F722).'),
            array('id' => 'rx_dc',     'label' => 'RX DC blocker',        'type' => 'sel', 'define' => 'RX_DC_BLOCK',
                  'choices' => array('' => 'Default', '1' => 'On', '0' => 'Off'),
                  'help' => 'Removes the offset of the audio input (the dc= value of the debug report).'),
            array('id' => 'rx_dc_shift', 'label' => 'RX DC blocker time constant', 'type' => 'num', 'define' => 'RX_DC_SHIFT', 'min' => 8, 'max' => 20,
                  'help' => '2^n samples; 14 is about 0.7 s.'),
            array('id' => 'rx_unity',  'label' => 'RX level for gain x1', 'type' => 'num', 'define' => 'RX_LEVEL_UNITY', 'min' => 1, 'max' => 255,
                  'help' => 'The modem gain is rxLevel of the Modem tab divided by this number.'),
            array('id' => 'rx_adc_shift', 'label' => 'RX ADC scaling shift', 'type' => 'sel', 'define' => 'RX_ADC_SHIFT',
                  'choices' => array('' => 'Default', '4' => '4 (12-bit ADC to full q15)', '0' => '0 (raw ADC counts)')),
            array('id' => 'tx_dac_shift', 'label' => 'TX DAC scaling shift', 'type' => 'sel', 'define' => 'TX_DAC_SHIFT',
                  'choices' => array('' => 'Default', '4' => '4 (full q15 to the 12-bit DAC)', '0' => '0 (raw DAC counts)')),
            array('id' => 'dac_buf',   'label' => 'DAC output buffer',    'type' => 'sel', 'define' => 'DAC_OUTPUT_BUFFER',
                  'choices' => array('' => 'Default', '1' => 'On', '0' => 'Off')),
            array('id' => 'trim_ppm',  'label' => 'Sample clock trim (ppm)', 'type' => 'num', 'define' => 'SAMPLE_TRIM_PPM', 'min' => -20000, 'max' => 20000, 'signed' => true,
                  'help' => 'Slows the 24 kHz sample clock by this many ppm (negative = faster).'),
            array('id' => 'debug',     'label' => 'Debug report every 5 s', 'type' => 'sel', 'define' => 'DEBUG_FRAMES',
                  'choices' => array('' => 'Default', '1' => 'On', '0' => 'Off'),
                  'help' => 'The FW rx= line in the Modem_Host log, with the audio peak, clip and dc values.')
        )
    );
    for ($i = 0; $i < 8; $i++)
        $sample['options'][] = array('id' => 'led' . $i, 'label' => 'LED ' . $i . ' pin', 'type' => 'pin', 'define' => 'LED' . $i . '_PIN');
    $sample['options'][] = array('id' => 'led_high', 'label' => 'LED active level', 'type' => 'sel', 'define' => 'LED_ACTIVE_HIGH', 'choices' => $lowhigh);

    return array('hs' => $hs, 'sample' => $sample);
}

/*
 * fwIni()
 *
 * Reads the value of a key in a section of /etc/openmt.ini.
 *
 * $section - Section name without brackets.
 * $key     - Key name.
 *
 * Returns the value, or "" if it is not there.
 */
function fwIni($section, $key)
{
    $ini = @parse_ini_file('/etc/openmt.ini', true, INI_SCANNER_RAW);
    if (!is_array($ini) || !isset($ini[$section][$key]))
        return '';
    return trim((string)$ini[$section][$key], " \t\"'");
}

/*
 * fwFirmwareDir()
 *
 * Finds the folder of one firmware inside the project's modem folder.  The names have changed between
 * versions of the project, so the sample firmware is any OPENMT_SAMPLE_* folder (in any letter case) that has a
 * Makefile, and OPENMT_HS is matched in any letter case.
 *
 * $root - The project folder (the one that holds modem/).
 * $kind - "hs" or "sample".
 *
 * Returns the name of the folder inside modem/, or "" if there is none.
 */
function fwFirmwareDir($root, $kind)
{
    $names = @scandir($root . '/modem');
    if ($names === false)
        return '';
    $pattern = ($kind === 'hs') ? '/^OPENMT_HS$/i' : '/^OPENMT_SAMPLE_/i';
    $best = '';
    foreach ($names as $name)
    {
        if (!preg_match($pattern, $name) || !@is_dir($root . '/modem/' . $name))
            continue;
        if (!@is_file($root . '/modem/' . $name . '/Makefile'))
            continue;
        if ($best === '' || strcasecmp($name, 'OPENMT_SAMPLE_SMT32FXXX') === 0 || strcasecmp($name, 'OPENMT_SAMPLE_STM32FXXX') === 0)
            $best = $name;
    }
    return $best;
}

/*
 * fwSourceCandidates()
 *
 * The places searched for the project: [firmware] source= in /etc/openmt.ini (the project folder or its modem
 * folder), /home/pi/OpenMT (the firmware folders are in /home/pi/OpenMT/modem), the folder this script is in if
 * it is inside a clone, and other usual places for a clone.
 *
 * Returns an array of folders.
 */
function fwSourceCandidates()
{
    $candidates = array();
    $set = fwIni('firmware', 'source');
    if ($set !== '')
        $candidates[] = (basename(rtrim($set, '/')) === 'modem') ? dirname(rtrim($set, '/')) : rtrim($set, '/');
    $candidates[] = '/home/pi/OpenMT';
    $candidates[] = dirname(__DIR__, 2);
    foreach (glob('/home/*/OpenMT', GLOB_ONLYDIR) ?: array() as $d)
        $candidates[] = $d;
    foreach (glob('/home/*/OpenMT-main', GLOB_ONLYDIR) ?: array() as $d)
        $candidates[] = $d;
    $candidates[] = '/opt/OpenMT';
    $candidates[] = '/usr/local/src/OpenMT';
    return array_values(array_unique($candidates));
}

/*
 * fwFindSource()
 *
 * Finds the project's source tree: the first of the places of fwSourceCandidates() that has at least one of
 * the firmware folders.
 *
 * Returns the folder, or "" if there is none.
 */
function fwFindSource()
{
    foreach (fwSourceCandidates() as $root)
    {
        if (fwFirmwareDir($root, 'hs') !== '' || fwFirmwareDir($root, 'sample') !== '')
            return $root;
    }
    return '';
}

/*
 * fwSourceReport()
 *
 * Says what the search for the source files found, for the page when nothing was found: each place with the
 * reason it did not work, in words a person can act on (the web server's user cannot read the folder, there is
 * no modem folder, the folder holds other names).
 *
 * Returns an array of text lines.
 */
function fwSourceReport()
{
    $lines = array();
    $user  = function_exists('posix_getpwuid') ? (posix_getpwuid(posix_geteuid())['name'] ?? '') : '';
    $lines[] = 'The web server runs PHP as ' . ($user !== '' ? $user : 'an unknown user') . '.';
    $base = (string)ini_get('open_basedir');
    if ($base !== '')
        $lines[] = 'PHP open_basedir is set to ' . $base . ' and keeps PHP out of other folders; add the project folder to it.';
    foreach (fwSourceCandidates() as $root)
    {
        if (!@is_dir($root))
        {
            $parent = dirname($root);
            $why = @is_dir($parent) ? 'not there, or the web server cannot enter ' . $parent : 'not there, or the web server cannot enter ' . $parent;
            $lines[] = $root . ': ' . $why . ' (it needs the x permission on every folder in the path)';
        }
        else if (!@is_dir($root . '/modem'))
            $lines[] = $root . ': there is no modem folder in it, or the web server cannot read it';
        else
        {
            $names = @scandir($root . '/modem');
            if ($names === false)
                $lines[] = $root . '/modem: the web server cannot read it (it needs r and x)';
            else
            {
                $dirs = array();
                foreach ($names as $n)
                    if ($n !== '.' && $n !== '..' && @is_dir($root . '/modem/' . $n))
                        $dirs[] = $n;
                $lines[] = $root . '/modem holds: ' . implode(', ', $dirs) . ' - no OPENMT_HS or OPENMT_SAMPLE_* folder with a Makefile';
            }
        }
    }
    return $lines;
}

/*
 * fwFindTool()
 *
 * Finds a program in the usual folders and in the folder [firmware] toolchain= names.
 *
 * $name - Program name, for example "arm-none-eabi-gcc".
 *
 * Returns the full path, or "" if it was not found.
 */
function fwFindTool($name)
{
    $dirs = array();
    $set = fwIni('firmware', 'toolchain');
    if ($set !== '')
        $dirs[] = rtrim($set, '/');
    foreach (explode(':', (string)getenv('PATH')) as $d)
        if ($d !== '')
            $dirs[] = $d;
    foreach (array('/usr/bin', '/usr/local/bin', '/bin') as $d)
        $dirs[] = $d;
    foreach (glob('/opt/*/bin', GLOB_ONLYDIR) ?: array() as $d)
        $dirs[] = $d;
    foreach ($dirs as $d)
    {
        if (@is_file($d . '/' . $name) && @is_executable($d . '/' . $name))
            return $d . '/' . $name;
    }
    return '';
}

/*
 * fwHsDefaults()
 *
 * Reads Config.h of OPENMT_HS to find what it switches on now, so the page starts from what the tree builds
 * without any change.  A define counts when its line is not commented out.
 *
 * $root - Source tree.
 *
 * Returns an array: define name => true for each define that is on, and name => value for the numbers.
 */
function fwHsDefaults($root)
{
    $on = array();
    $dir  = fwFirmwareDir($root, 'hs');
    $text = $dir === '' ? false : @file_get_contents($root . '/modem/' . $dir . '/Config.h');
    if ($text === false)
        return $on;
    foreach (preg_split('/\r?\n/', $text) as $line)
    {
        if (preg_match('/^\s*#\s*define\s+([A-Z0-9_]+)(?:\s+(\S+))?/', $line, $m))
            $on[$m[1]] = isset($m[2]) ? $m[2] : true;
    }
    return $on;
}

/*
 * fwSampleDefaults()
 *
 * Reads what the sample firmware builds with when an option is left at its default: the #define lines of config.h,
 * and the FLASH_KB / RAM_KB defaults of each chip in the Makefile.  The page shows them, because a build made by
 * hand (make FLASH_KB=64 RAM_KB=20 ...) is not described by config.h alone.
 *
 * $root - Source tree, as fwFindSource() found it.
 *
 * Returns array("cfg" => define name => text, "mem" => chip => array("flash" => KB, "ram" => KB)).
 */
function fwSampleDefaults($root)
{
    $cfg = array();
    $mem = array();
    $dir = fwFirmwareDir($root, 'sample');
    if ($dir === '')
        return array('cfg' => $cfg, 'mem' => $mem);
    $text = @file_get_contents($root . '/modem/' . $dir . '/config.h');
    if ($text !== false)
    {
        foreach (preg_split('/\r?\n/', $text) as $line)
        {
            if (preg_match('/^\s*#\s*define\s+([A-Z0-9_]+)\s+([^\s\/]+)/', $line, $m) && !isset($cfg[$m[1]]))
                $cfg[$m[1]] = $m[2];
        }
    }
    $make = @file_get_contents($root . '/modem/' . $dir . '/Makefile');
    if ($make !== false && preg_match_all('/ifeq \(\$\(MCU\),(\w+)\)(.*?)(?=\n(?:else|endif))/s', $make, $blocks, PREG_SET_ORDER))
    {
        foreach ($blocks as $b)
        {
            if (preg_match('/FLASH_KB\s*\?=\s*(\d+)/', $b[2], $f) && preg_match('/RAM_KB\s*\?=\s*(\d+)/', $b[2], $r))
                $mem[$b[1]] = array('flash' => $f[1], 'ram' => $r[1]);
        }
    }
    return array('cfg' => $cfg, 'mem' => $mem);
}

/*
 * fwDescribe()
 *
 * The catalog for the page, with the defaults of Config.h filled in when the source tree is known.
 *
 * Returns an array: the catalog, with a "value" for each option of OPENMT_HS.
 */
function fwDescribe()
{
    $root = fwFindSource();
    $cat  = fwCatalog();
    $on   = $root !== '' ? fwHsDefaults($root) : array();

    foreach ($cat['hs']['options'] as &$opt)
    {
        $opt['value'] = '';
        if ($opt['type'] === 'group')
        {
            $opt['value'] = $opt['default'];
            foreach ($opt['choices'] as $name => $label)
            {
                if (isset($on[$name]))
                    $opt['value'] = $name;
            }
        }
        else if ($opt['type'] === 'bool')
            $opt['value'] = isset($on[$opt['define']]) ? '1' : '0';
        else if (isset($on[$opt['define']]) && $on[$opt['define']] !== true)
            $opt['value'] = rtrim((string)$on[$opt['define']], 'UL');
    }
    unset($opt);
    $sd = $root !== '' ? fwSampleDefaults($root) : array('cfg' => array(), 'mem' => array());
    foreach ($cat['sample']['options'] as &$opt)
    {
        $opt['value'] = isset($opt['default']) ? $opt['default'] : '';
        if (isset($opt['define']) && isset($sd['cfg'][$opt['define']]))
            $opt['cfg'] = $sd['cfg'][$opt['define']];
        if (isset($opt['make']) && ($opt['make'] === 'FLASH_KB' || $opt['make'] === 'RAM_KB'))
        {
            $key = $opt['make'] === 'FLASH_KB' ? 'flash' : 'ram';
            $opt['cfgmcu'] = array();
            foreach ($sd['mem'] as $chip => $m)
                $opt['cfgmcu'][$chip] = $m[$key];
        }
    }
    unset($opt);
    foreach ($cat as &$fw)
    {
        foreach ($fw['options'] as &$opt)
        {
            if ($opt['type'] === 'pin')
                $opt['choices'] = fwPinList(!empty($opt['none']));
        }
        unset($opt);
    }
    unset($fw);
    foreach ($cat as &$fw)
        $fw['present'] = $root !== '' && fwFirmwareDir($root, $fw['kind']) !== '';
    unset($fw);
    return array('catalog' => $cat, 'source' => $root, 'report' => $root === '' ? fwSourceReport() : array(),
                 'toolchain' => fwFindTool('arm-none-eabi-gcc') !== '', 'make' => fwFindTool('make') !== '');
}

/*
 * fwPlan()
 *
 * Checks a request and turns it into a build plan.  Nothing that is not listed in the catalog is accepted, and
 * every value is checked against its option.
 *
 * $fwId   - "hs" or "sample".
 * $target - Make target of OPENMT_HS ("hs", "bl", "nobl"); ignored for the sample firmware.
 * $values - The option values of the request: id => text.
 *
 * Returns an array with "ok" => false and "error", or "ok" => true and
 *   dir, make (target and variables for make), extra (-D options text for CFLAGS_EXTRA), append (text to
 *   add to Config.h, or ""), files (output files to collect, relative to the build folder, or null for all
 *   of build/<mcu>), lines (what was asked, for the log).
 */
function fwPlan($fwId, $target, $values)
{
    $cat = fwCatalog();
    if (!isset($cat[$fwId]))
        return array('ok' => false, 'error' => 'Unknown firmware');
    $fw = $cat[$fwId];
    if (!is_array($values))
        $values = array();
    foreach ($values as $id => $v)
    {
        $known = false;
        foreach ($fw['options'] as $opt)
            if ($opt['id'] === $id)
                $known = true;
        if (!$known)
            return array('ok' => false, 'error' => 'Unknown option ' . substr(preg_replace('/[^A-Za-z0-9_]/', '', (string)$id), 0, 30));
    }

    $plan = array('ok' => true, 'dir' => $fw['dir'], 'kind' => $fw['kind'], 'make' => array(), 'extra' => '', 'append' => '', 'files' => null, 'lines' => array());

    if ($fwId === 'hs')
    {
        if (!isset($fw['targets'][$target]))
            return array('ok' => false, 'error' => 'Unknown build');
        $plan['make']  = array($target);
        $plan['files'] = $fw['targets'][$target]['files'];
        $plan['lines'][] = 'Build: ' . $fw['targets'][$target]['label'];
    }
    else
        $plan['make'] = array('all');

    $defs   = array();      /* -D options of the sample firmware */
    $append = array();      /* lines for Config.h */
    $mcu    = isset($values['mcu']) && $values['mcu'] !== '' ? (string)$values['mcu'] : 'f105';
    $loader = isset($values['loader']) && (string)$values['loader'] === '1';

    foreach ($fw['options'] as $opt)
    {
        $id = $opt['id'];
        $v  = isset($values[$id]) ? trim((string)$values[$id]) : '';
        $label = $opt['label'];

        if (!empty($opt['onlymcu']) && $mcu !== $opt['onlymcu'])
            continue;
        if (!empty($opt['needs']) && !(isset($values[$opt['needs']]) && (string)$values[$opt['needs']] === '1'))
            continue;
        if ($id === 'ext_hz' && isset($values['ext_custom']) && trim((string)$values['ext_custom']) !== '')
            continue;           /* the number typed in wins over the choice */

        if ($opt['type'] === 'group')
        {
            if ($v === '')
                $v = $opt['default'];
            if (!isset($opt['choices'][$v]))
                return array('ok' => false, 'error' => $label . ': not a choice');
            foreach ($opt['choices'] as $name => $text)
                $append[] = '#undef ' . $name;
            $append[] = '#define ' . $v;
            $plan['lines'][] = $label . ': ' . $opt['choices'][$v];
            continue;
        }
        if ($opt['type'] === 'bool')
        {
            if ($v !== '' && $v !== '0' && $v !== '1')
                return array('ok' => false, 'error' => $label . ': not a valid value');
            if (isset($opt['define']))
            {
                if ($v === '')
                    continue;
                $append[] = '#undef ' . $opt['define'];
                if ($v === '1')
                    $append[] = '#define ' . $opt['define'];
                $plan['lines'][] = $label . ': ' . ($v === '1' ? 'on' : 'off');
            }
            else if ($v === '1')
            {
                $plan['make'][] = $opt['make'] . '=1';
                $plan['lines'][] = $label . ': on';
            }
            continue;
        }
        if ($v === '')
        {
            if (!empty($opt['required']))
                $v = $opt['default'];
            else
                continue;
        }
        if ($opt['type'] === 'num')
        {
            if (!preg_match(!empty($opt['signed']) ? '/^-?[0-9]{1,9}$/' : '/^[0-9]{1,9}$/', $v) || (int)$v < $opt['min'] || (int)$v > $opt['max'])
                return array('ok' => false, 'error' => $label . ': a number from ' . $opt['min'] . ' to ' . $opt['max'] . ' is needed');
            $v = (string)(int)$v;
            $text = $v;
        }
        else if ($opt['type'] === 'sel')
        {
            if (!isset($opt['choices'][$v]))
                return array('ok' => false, 'error' => $label . ': not a choice');
            $text = $opt['choices'][$v];
        }
        else if ($opt['type'] === 'pin')
        {
            if ($v === 'none' && !empty($opt['none']))
            {
                $text = 'not used';
            }
            else if (preg_match('/^P([A-E])([0-9]|1[0-5])$/', $v, $m))
                $text = $v;
            else
                return array('ok' => false, 'error' => $label . ': not a pin');
        }
        else
            return array('ok' => false, 'error' => 'Bad catalog entry');

        $plan['lines'][] = $label . ': ' . $text;
        if (isset($opt['make']))
        {
            $plan['make'][] = $opt['make'] . '=' . $v;
            continue;
        }
        /* a define: pins are PB(12) for the sample firmware, numbers get their suffix */
        if ($opt['type'] === 'pin')
            $value = ($v === 'none') ? 'PIN_NONE' : 'P' . $m[1] . '(' . $m[2] . ')';
        else
            $value = $v . (isset($opt['suffix']) ? $opt['suffix'] : '');
        if ($fwId === 'hs')
        {
            $append[] = '#undef ' . $opt['define'];
            $append[] = '#define ' . $opt['define'] . ' ' . $value;
        }
        else
            $defs[] = '-D"' . $opt['define'] . '=' . $value . '"';
    }

    if ($fwId === 'sample')
    {
        $root = fwFindSource();
        $sd = $root !== '' ? fwSampleDefaults($root) : array('mem' => array());
        foreach (array('flash' => 'Flash (KB)', 'ram' => 'RAM (KB)') as $k => $label)
        {
            if ((!isset($values[$k]) || trim((string)$values[$k]) === '') && isset($sd['mem'][$mcu]))
                $plan['lines'][] = $label . ': default of the Makefile for ' . $mcu . ': ' . $sd['mem'][$mcu][$k];
        }
    }
    if ($fwId === 'sample' && $loader && $mcu !== 'f105')
        return array('ok' => false, 'error' => 'The serial loader is for the F105 only');

    $plan['extra'] = implode(' ', $defs);
    if (count($append) > 0)
        $plan['append'] = "\n/* ---- added by the dashboard Firmware tab ---- */\n" . implode("\n", $append) . "\n";
    if ($fwId === 'sample')
        $plan['mcu'] = $mcu;
    return $plan;
}

/*
 * fwCopyTree()
 *
 * Copies a folder, leaving out the results of earlier builds and version control.
 *
 * $from - Source folder.
 * $to   - Destination folder (created).
 *
 * Returns true on success.
 */
function fwCopyTree($from, $to)
{
    if (!@mkdir($to, 0700, true) && !is_dir($to))
        return false;
    $items = @scandir($from);
    if ($items === false)
        return false;
    foreach ($items as $name)
    {
        if ($name === '.' || $name === '..' || $name === '.git' || $name === 'bin' || $name === 'build' || strncmp($name, 'obj_', 4) === 0)
            continue;
        $src = $from . '/' . $name;
        if (is_link($src))
            continue;
        if (is_dir($src))
        {
            if (!fwCopyTree($src, $to . '/' . $name))
                return false;
        }
        else if (!@copy($src, $to . '/' . $name))
            return false;
    }
    return true;
}

/*
 * fwRemoveTree()
 *
 * Deletes a folder made by fwCopyTree().
 *
 * $dir - Folder.
 *
 * Returns nothing.
 */
function fwRemoveTree($dir)
{
    if (!is_dir($dir) || is_link($dir))
        return;
    foreach (scandir($dir) ?: array() as $name)
    {
        if ($name === '.' || $name === '..')
            continue;
        $p = $dir . '/' . $name;
        if (is_dir($p) && !is_link($p))
            fwRemoveTree($p);
        else
            @unlink($p);
    }
    @rmdir($dir);
}

/*
 * fwBuild()
 *
 * Does a build: copies the firmware folder to a work folder, adds the options, runs make and collects the
 * files.  The output of make is passed to a function as it comes.
 *
 * $root   - Source tree.
 * $plan   - The plan from fwPlan().
 * $work   - Work folder (it must not exist; it is removed at the end).
 * $onText - Function called with each piece of output, and every few seconds with "" so the caller can write
 *           a heartbeat.
 * $files  - Receives the result files: array of name => content.
 *
 * Returns true if the build worked and made the files.
 */
function fwBuild($root, $plan, $work, $onText, &$files)
{
    $files = array();
    $make  = fwFindTool('make');
    $gcc   = fwFindTool('arm-none-eabi-gcc');

    if ($make === '')
    {
        $onText("make is not installed (sudo apt install build-essential).\n");
        return false;
    }
    if ($gcc === '')
    {
        $onText("The Arm toolchain is not installed (sudo apt install gcc-arm-none-eabi).\n");
        return false;
    }

    $found = fwFirmwareDir($root, $plan['kind']);
    if ($found === '')
    {
        $onText("The folder of this firmware was not found in " . $root . "/modem.\n");
        return false;
    }
    $src = $root . '/modem/' . $found;
    $dir = $work . '/' . $plan['dir'];
    if (!fwCopyTree($src, $dir))
    {
        $onText("Could not copy the source files from " . $src . ".\n");
        fwRemoveTree($work);
        return false;
    }

    if ($plan['append'] !== '')
    {
        $cfg  = $dir . '/Config.h';
        $text = @file_get_contents($cfg);
        $end  = $text === false ? false : strrpos($text, '#endif');
        if ($end === false)
        {
            $onText("Config.h was not found or has no closing #endif.\n");
            fwRemoveTree($work);
            return false;
        }
        file_put_contents($cfg, substr($text, 0, $end) . $plan['append'] . "\n" . substr($text, $end));
        $files['build-options.txt'] = $plan['append'];
    }

    $args = array($make, '-j2');
    foreach ($plan['make'] as $a)
        $args[] = $a;
    if ($plan['extra'] !== '')
        $args[] = 'CFLAGS_EXTRA=' . $plan['extra'];

    $env = array('PATH' => dirname($gcc) . ':/usr/bin:/bin', 'LC_ALL' => 'C', 'HOME' => $work);
    $onText("\n$ make " . implode(' ', array_slice($args, 1)) . "\n");

    $pipes = array();
    $proc  = proc_open($args, array(0 => array('file', '/dev/null', 'r'), 1 => array('pipe', 'w'), 2 => array('pipe', 'w')), $pipes, $dir, $env);
    if (!is_resource($proc))
    {
        $onText("Could not start make.\n");
        fwRemoveTree($work);
        return false;
    }
    stream_set_blocking($pipes[1], false);
    stream_set_blocking($pipes[2], false);

    $start  = time();
    $open   = true;
    $failed = false;
    while ($open)
    {
        $read = array($pipes[1], $pipes[2]);
        $w = null;
        $e = null;
        if (stream_select($read, $w, $e, 1) > 0)
        {
            foreach ($read as $p)
            {
                $chunk = fread($p, 8192);
                if ($chunk !== false && $chunk !== '')
                    $onText($chunk);
            }
        }
        $onText('');
        $status = proc_get_status($proc);
        if (!$status['running'])
            $open = false;
        if (time() - $start > FW_BUILD_LIMIT_S)
        {
            proc_terminate($proc, 9);
            $onText("\nStopped: the build took more than " . (FW_BUILD_LIMIT_S / 60) . " minutes.\n");
            $failed = true;
            $open = false;
        }
    }
    foreach (array($pipes[1], $pipes[2]) as $p)
    {
        $rest = stream_get_contents($p);
        if ($rest !== false && $rest !== '')
            $onText($rest);
        fclose($p);
    }
    $exit = proc_close($proc);
    if ($exit !== 0 && !$failed)
    {
        $onText("\nmake stopped with an error (status " . $exit . ").\n");
        $failed = true;
    }

    if (!$failed)
    {
        $found = array();
        if ($plan['files'] !== null)
        {
            foreach ($plan['files'] as $rel)
                $found[basename($rel)] = $dir . '/' . $rel;
        }
        else
        {
            /* the sample firmware: everything it made for the chip, except the objects */
            $mcu = isset($plan['mcu']) ? $plan['mcu'] : 'f105';
            foreach (glob($dir . '/build/' . $mcu . '/*') ?: array() as $f)
            {
                if (preg_match('/\.(bin|hex)$/', $f))
                    $found[basename($f)] = $f;
            }
        }
        foreach ($found as $name => $path)
        {
            $data = @file_get_contents($path);
            if ($data === false || strlen($data) == 0)
            {
                $onText("Missing result file: " . $name . "\n");
                $failed = true;
            }
            else
                $files[$name] = $data;
        }
        if (count($found) == 0)
        {
            $onText("The build made no result files.\n");
            $failed = true;
        }
    }

    fwRemoveTree($work);
    return !$failed;
}

/*
 * fwFlashPlan()
 *
 * Checks a request to flash a file of a build and turns it into the arguments of openmt_flash.sh.  Both the page
 * (data_query.php) and the flasher (tools/firmware_flasher/flasher.php, which runs as root) use it, so the
 * request is checked again where it counts.  Only a .bin file that a build made can be flashed, and only the ones
 * that belong at the start of the flash: the OPENMT_HS "hs" and "nobl" builds and the application file of the
 * sample firmware.  The "bl" build of OPENMT_HS is linked behind the 8 KB bootloader of MMDVM_HS boards; written
 * to the start of the flash it would not run, so it is refused.
 *
 * $fwId    - "hs" or "sample" (the firmware of the build).
 * $target  - The make target of the build ("hs", "bl", "nobl" or "all").
 * $options - The option values of the build, as the JSON text stored with it.
 * $name    - Name of the result file.
 * $mode    - "soft", "gpio" or "seq" (see openmt_flash.sh).
 * $port    - Serial port of the modem, such as /dev/ttyAMA0.
 * $boot0   - GPIO number that drives BOOT0 (gpio and seq modes).
 * $nrst    - GPIO number that drives NRST (gpio and seq modes).
 *
 * Returns an array with "ok" => false and "error", or "ok" => true and "args" (the arguments for the script,
 * without the file name), "line" (a text for the log) and "mcu".
 */
function fwFlashPlan($fwId, $target, $options, $name, $mode, $port, $boot0, $nrst)
{
    $values = json_decode((string)$options, true);
    if (!is_array($values))
        $values = array();
    $mcu = 'f105';
    $loader = false;

    if ($fwId === 'hs')
    {
        if ($target === 'bl')
            return array('ok' => false, 'error' => 'The "with bootloader offset" build is linked for 0x08002000, behind the MMDVM_HS bootloader. Build the one without the offset to flash it over the serial port.');
        $want = array('hs' => 'openmths_f1.bin', 'nobl' => 'openmths_f1nobl.bin');
        if (!isset($want[$target]) || $name !== $want[$target])
            return array('ok' => false, 'error' => 'This file cannot be flashed');
        if ($mode === 'soft')
            return array('ok' => false, 'error' => 'OPENMT_HS cannot reboot into the boot loader by itself: use gpio or seq');
    }
    else if ($fwId === 'sample')
    {
        $mcu = isset($values['mcu']) && $values['mcu'] !== '' ? (string)$values['mcu'] : 'f105';
        if (!in_array($mcu, array('f105', 'f446', 'f722'), true) || $name !== 'openmt_sample_' . $mcu . '.bin')
            return array('ok' => false, 'error' => 'This file cannot be flashed (only openmt_sample_<chip>.bin)');
        $loader = ($mcu === 'f105' && isset($values['loader']) && (string)$values['loader'] === '1');
        if ($mode === 'soft' && $mcu !== 'f105')
            return array('ok' => false, 'error' => 'The soft boot is for the F105 only: use gpio or seq');
        /* the F105's own ROM boot loader cannot be started by software; the soft method works only through the serial
           loader, whose application starts at 0x08001800.  A build linked for 0x08000000 would be written to the
           wrong place, so it is refused. */
        if ($mode === 'soft' && !$loader)
            return array('ok' => false, 'error' => 'The soft method needs a build made with "Serial loader" switched on (the application is then linked for 0x08001800, behind the loader). This build is linked for 0x08000000: build it again with the option, or use gpio or seq.');
    }
    else
        return array('ok' => false, 'error' => 'Unknown firmware');

    if (!in_array($mode, array('soft', 'gpio', 'seq'), true))
        return array('ok' => false, 'error' => 'The method must be soft, gpio or seq');
    if (!preg_match('#^/dev/(ttyAMA[0-9]{1,2}|ttyS[0-9]{1,2}|ttyUSB[0-9]{1,2}|ttyACM[0-9]{1,2}|serial[0-9]{1,2})$#', (string)$port))
        return array('ok' => false, 'error' => 'The serial port must be like /dev/ttyAMA0');

    $args = array('-m', $mode, '-p', (string)$port);
    if ($mode !== 'soft')
    {
        if (!preg_match('/^[0-9]{1,2}$/', (string)$boot0) || !preg_match('/^[0-9]{1,2}$/', (string)$nrst) ||
            (int)$boot0 > 27 || (int)$nrst > 27 || (int)$boot0 == (int)$nrst)
            return array('ok' => false, 'error' => 'BOOT0 and NRST must be two different GPIO numbers from 0 to 27');
        $args[] = '-B';
        $args[] = (string)(int)$boot0;
        $args[] = '-R';
        $args[] = (string)(int)$nrst;
    }
    if ($loader)
        $args[] = '-L';

    return array('ok' => true, 'args' => $args, 'mcu' => $mcu, 'loader' => $loader,
                 'line' => $name . ' by ' . $mode . ' on ' . $port . ($mode !== 'soft' ? ' (BOOT0 GPIO ' . (int)$boot0 . ', NRST GPIO ' . (int)$nrst . ')' : '') . ($loader ? ', behind the serial loader' : ''));
}

/*
 * fwFindFlashScript()
 *
 * Finds openmt_flash.sh in the source tree (it is in the sample firmware's tools folder and flashes the
 * OPENMT_HS firmware as well).
 *
 * $root - Source tree, as fwFindSource() found it.
 *
 * Returns the path, or "" if it is not there.
 */
function fwFindFlashScript($root)
{
    $dir = fwFirmwareDir($root, 'sample');
    if ($dir !== '' && @is_file($root . '/modem/' . $dir . '/tools/openmt_flash.sh'))
        return $root . '/modem/' . $dir . '/tools/openmt_flash.sh';
    return '';
}
