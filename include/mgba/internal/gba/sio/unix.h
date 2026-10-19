/* Copyright (c) 2026 Bennet Hattesen
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef SIO_UNIX_H
#define SIO_UNIX_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba/core/timing.h>
#include <mgba/internal/gba/sio.h>

/*
 * https://problemkaputt.de/gbatek.htm#sioguartmode
 *
 * Bit   Expl.
 * 0-1   Baud Rate  (0-3: 9600,38400,57600,115200 bps)
 * 2     CTS Flag   (0=Send always/blindly, 1=Send only when SC=LOW)
 * 3     Parity Control (0=Even, 1=Odd)
 * 4     Send Data Flag      (0=Not Full,  1=Full)    (Read Only)
 * 5     Receive Data Flag   (0=Not Empty, 1=Empty)   (Read Only)
 * 6     Error Flag          (0=No Error,  1=Error)   (Read Only)
 * 7     Data Length         (0=7bits,   1=8bits)
 * 8     FIFO Enable Flag    (0=Disable, 1=Enable)
 * 9     Parity Enable Flag  (0=Disable, 1=Enable)
 * 10    Send Enable Flag    (0=Disable, 1=Enable)
 * 11    Receive Enable Flag (0=Disable, 1=Enable)
 * 12    Must be "1" for UART mode
 * 13    Must be "1" for UART mode
 * 14    IRQ Enable          (0=Disable, 1=IRQ when any Bit 4/5/6 become set)
 * 15    Not used            (Read only, always 0)
 */
typedef struct {
    uint16_t baud : 2;
    uint16_t cts : 1;
    uint16_t parity : 1;
    uint16_t send_data : 1;
    uint16_t receive_data : 1;
    uint16_t error : 1;
    uint16_t data_bits : 1;
    uint16_t fifo_enable : 1;
    uint16_t parity_enable : 1;
    uint16_t send_enable : 1;
    uint16_t receive_enable : 1;
    uint16_t must_be_1_for_uart : 2;
    uint16_t irq_enable : 1;
    uint16_t unused_15 : 1;
} siocnt_uart_t;

unsigned int sio_uart_baud[4] = { 9600, 38400, 57600, 115200 };

struct GBASIOUnix {
	struct GBASIODriver siodriver;
	struct mTimingEvent event;
};

struct GBASIOUnix m_unixsocket;

void GBASIOUnixCreate(struct GBASIOUnix* unix);

CXX_GUARD_END

#endif
