/* Copyright (c) 2026 Bennet Hattesen
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <sys/socket.h>
#include <sys/un.h>

#include <mgba/internal/gba/sio/unix.h>

#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>

static bool GBASIOUnixInit(struct GBASIODriver* driver);
static void GBASIOUnixReset(struct GBASIODriver* driver);
static void GBASIOUnixSetMode(struct GBASIODriver* driver, enum GBASIOMode mode);
static bool GBASIOUnixHandlesMode(struct GBASIODriver* driver, enum GBASIOMode mode);
static int GBASIOUnixConnectedDevices(struct GBASIODriver* driver);
static uint16_t GBASIOUnixWriteSIOCNT(struct GBASIODriver* driver, uint16_t value);
static uint8_t GABSIOUnixWriteSIODATA8(struct GBASIODriver* driver, uint8_t value);
static void GBASIOUnixProcessEvents(struct mTiming* timing, void* context, uint32_t cyclesLate);

void GBASIOUnixCreate(struct GBASIOUnix* unix) {
	memset(&unix->siodriver, 0, sizeof(unix->siodriver));
	unix->siodriver.init = GBASIOUnixInit;
	unix->siodriver.reset = GBASIOUnixReset;
	unix->siodriver.setMode = GBASIOUnixSetMode;
	unix->siodriver.handlesMode = GBASIOUnixHandlesMode;
	//unix->siodriver.connectedDevices = GBASIOUnixConnectedDevices;
	unix->siodriver.writeSIOCNT = GBASIOUnixWriteSIOCNT;
	unix->siodriver.writeSIODATA8 = GABSIOUnixWriteSIODATA8;
	unix->event.context = unix;
	unix->event.name = "GB SIO Lockstep";
	unix->event.callback = GBASIOUnixProcessEvents;
	unix->event.priority = 0x80;

	puts("UnixCreate");
}

static const char* _modeName(enum GBASIOMode mode) {
	switch (mode) {
	case GBA_SIO_NORMAL_8:
		return "NORMAL8";
	case GBA_SIO_NORMAL_32:
		return "NORMAL32";
	case GBA_SIO_MULTI:
		return "MULTI";
	case GBA_SIO_JOYBUS:
		return "JOYBUS";
	case GBA_SIO_GPIO:
		return "GPIO";
	case GBA_SIO_UART:
		return "UART";
	default:
		return "(unknown)";
	}
}

static bool GBASIOUnixInit(struct GBASIODriver* driver) {
	puts("UnixInit");
	struct GBASIOUnix* unix = (struct GBASIOUnix*) driver;
	GBASIOUnixReset(driver);

	int fd, cfd;
    socklen_t peer_addr_size;
    struct sockaddr_un my_addr;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1) 
    	return false;

    memset(&my_addr, 0, sizeof(my_addr));

    my_addr.sun_family = AF_UNIX;

    strncpy((char *) my_addr.sun_path, "gba_uart", sizeof(my_addr.sun_path) - 1);
    if (bind(fd, (struct sockaddr *) &my_addr, sizeof(my_addr)) == -1)
        return false;
    // if (listen(fd, LISTEN_BACKLOG) == -1)
    //     err(EXIT_FAILURE, "listen");
    // /* Now we can accept incoming connections one
    //    at a time using accept(2).  */
    // peer_addr_size = sizeof(peer_addr);
    // cfd = accept(sfd, (struct sockaddr *) &peer_addr,
    //              &peer_addr_size);
    // if (cfd == -1)
    //     err(EXIT_FAILURE, "accept");
    // /* Code to deal with incoming connection(s)...  */
    // if (close(sfd) == -1)
    //     err(EXIT_FAILURE, "close");
    // if (unlink(MY_SOCK_PATH) == -1)
    //     err(EXIT_FAILURE, "unlink");
	return true;
}

static void GBASIOUnixReset(struct GBASIODriver* driver) {
	puts("UnixReset");
	struct GBASIOUnix* unix = (struct GBASIOUnix*) driver;
	mTimingDeschedule(&unix->siodriver.p->p->timing, &unix->event);
	mTimingSchedule(&unix->siodriver.p->p->timing, &unix->event, 0);
}

static void GBASIOUnixSetMode(struct GBASIODriver* driver, enum GBASIOMode mode) {
	struct GBASIOUnix* unix = (struct GBASIOUnix*) driver;
	(void) unix;
	printf("I am in SIOUnix driver! SetMode to: %s\n", _modeName(mode));
}

static bool GBASIOUnixHandlesMode(struct GBASIODriver* driver, enum GBASIOMode mode) {
	UNUSED(driver);
	return mode == GBA_SIO_UART;
}

static int GBASIOUnixConnectedDevices(struct GBASIODriver* driver) {
	(void) driver;
	return 0;
}

static uint16_t GBASIOUnixWriteSIOCNT(struct GBASIODriver* driver, uint16_t value) {
	siocnt_uart_t *uart = (siocnt_uart_t *) &value;
	printf("Baud rate: %u\n", sio_uart_baud[uart->baud]);
	puts(uart->cts ? "Send only when SC=LOW" : "Send always");
	puts(uart->parity ? "Parity: Odd" : "Parity: Even");
	puts(uart->send_data ? "Send: Full" : "Send: Not Full");
	puts(uart->receive_data ? "Receive: Empty" : "Receive: Not Empty");
	puts(uart->error ? "Error" : "No Error");
	puts(uart->data_bits ? "8 Bits" : "7 Bits");
	puts(uart->fifo_enable ? "Fifo enabled" : "Fifo disabled");
	puts(uart->parity_enable ? "Parity enabled" : "Parity disabled");
	puts(uart->send_enable ? "Send enabled" : "Send disabled");
	puts(uart->receive_enable ? "Receive enabled" : "Receive disabled");
	puts(uart->irq_enable ? "IRQ enabled" : "IRQ disabled");

	return value;
}

static uint8_t GABSIOUnixWriteSIODATA8(struct GBASIODriver* driver, uint8_t value) {
	printf("GABSIOUnixWriteSIODATA8: Write %u\n", value);
	return value;
}

static void GBASIOUnixProcessEvents(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	(void) timing;
	struct GBASIOUnix * unix = (struct GBASIOUnix *) context;
	printf("GBASIOUnixProcessEvents: %u cyclesLate\n", cyclesLate);
	mTimingSchedule(timing, &unix->event, 16000000 * 10);
}
