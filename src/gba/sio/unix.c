/* Copyright (c) 2026 Bennet Hattesen
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <signal.h>

#include <mgba/internal/gba/sio/unix.h>

#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>

#define BITS_PER_SECOND 115200 // This is wrong, but we need to maintain compat for the time being
#define CYCLES_PER_BIT (GBA_ARM7TDMI_FREQUENCY / BITS_PER_SECOND)
#define CLOCK_GRAIN (CYCLES_PER_BIT * 8)

static bool GBASIOUnixInit(struct GBASIODriver* driver);
static void GBASIOUnixDeinit(struct GBASIODriver* driver);
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
	unix->siodriver.deinit = GBASIOUnixDeinit;
	unix->siodriver.reset = GBASIOUnixReset;
	unix->siodriver.setMode = GBASIOUnixSetMode;
	unix->siodriver.handlesMode = GBASIOUnixHandlesMode;
	unix->siodriver.connectedDevices = GBASIOUnixConnectedDevices;
	unix->siodriver.writeSIOCNT = GBASIOUnixWriteSIOCNT;
	unix->siodriver.writeSIODATA8 = GABSIOUnixWriteSIODATA8;
	unix->event.context = unix;
	unix->event.name = "GBASIOUnix Wakeup";
	unix->event.callback = GBASIOUnixProcessEvents;
	unix->event.priority = 0x80;
	unix->socket = 0;
	unix->peer_socket = 0;

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
	signal(SIGPIPE, SIG_IGN); // Stupid
	struct GBASIOUnix* unix = (struct GBASIOUnix*) driver;

	struct sockaddr_un my_addr;
	unix->socket = socket(AF_UNIX, SOCK_STREAM, 0);

	if (fcntl(unix->socket, F_SETFL, fcntl(unix->socket, F_GETFL, 0) | O_NONBLOCK) == -1){
		return false;
	}

	if (unix->socket == -1)
		return false;

	memset(&my_addr, 0, sizeof(my_addr));

	my_addr.sun_family = AF_UNIX;

	strncpy((char *) my_addr.sun_path, "gba_uart", sizeof(my_addr.sun_path) - 1);
	if (bind(unix->socket, (struct sockaddr *) &my_addr, sizeof(my_addr)) == -1)
		return false;
	if (listen(unix->socket, 1) == -1)
		return false;

	GBASIOUnixReset(driver);
	sleep(5);
	return true;
}

static void GBASIOUnixDeinit(struct GBASIODriver* driver) {
	struct GBASIOUnix* unix = (struct GBASIOUnix*) driver;
	puts("GBASIOUnixDeinit");
	if (unix->peer_socket)
		close(unix->peer_socket);
	if (unix->socket)
		close(unix->socket);
	unlink("gba_uart");
}

static void GBASIOUnixReset(struct GBASIODriver* driver) {
	puts("UnixReset");
	struct GBASIOUnix* unix = (struct GBASIOUnix*) driver;
	if (unix->peer_socket)
		close(unix->peer_socket);
	unix->peer_socket = 0;
	mTimingDeschedule(&unix->siodriver.p->p->timing, &unix->event);
	mTimingSchedule(&unix->siodriver.p->p->timing, &unix->event, 0);
}

static void GBASIOUnixSetMode(struct GBASIODriver* driver, enum GBASIOMode mode) {
	struct GBASIOUnix* unix = (struct GBASIOUnix*) driver;
	(void) unix;
	printf("GBASIOUnixSetMode! SetMode to: %s\n", _modeName(mode));
}

static bool GBASIOUnixHandlesMode(struct GBASIODriver* driver, enum GBASIOMode mode) {
	UNUSED(driver);
	if (mode == GBA_SIO_UART)
		puts("GBASIOUnixHandlesMode: yes I handle this mode");
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
	struct GBASIOUnix* unix = (struct GBASIOUnix*) driver;
	//printf("GABSIOUnixWriteSIODATA8: Write %u\n", value);
	if (unix->peer_socket) {
		int ret = write(unix->peer_socket, &value, 1);
		if (ret != 1) {
			puts("Write failed");
			GBASIOUnixReset(driver);
		}
	}
	return value;
}

static void GBASIOUnixProcessEvents(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	(void) timing;
	struct GBASIOUnix * unix = (struct GBASIOUnix *) context;
	//printf("GBASIOUnixProcessEvents: %u cyclesLate\n", cyclesLate);
	if (unix->peer_socket) {
		uint8_t buf[1];
		if (read(unix->peer_socket, buf, sizeof(buf)) == 1) {
			puts("GBASIOUnixProcessEvents: Read data from peer");
			unix->siodriver.p->p->memory.io[GBA_REG(SIODATA8)] = buf[0];
			GBARaiseIRQ(unix->siodriver.p->p, GBA_IRQ_SIO, cyclesLate);
		}
	} else {
		int fd = accept(unix->socket, NULL, NULL);
		if (fd > 0) {
			if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) != -1) {
				puts("GBASIOUnixProcessEvents: Accepted peer");
				unix->peer_socket = fd;
			}
		}
	}
	mTimingSchedule(timing, &unix->event, CLOCK_GRAIN);
}
