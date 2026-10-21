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

#define BITS_PER_SECOND 38400//115200
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
static uint8_t GABSIOUnixReadSIODATA8(struct GBASIODriver* driver, uint8_t value);
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
	unix->siodriver.readSIODATA8 = GABSIOUnixReadSIODATA8;
	unix->event.context = unix;
	unix->event.name = "GBASIOUnix Wakeup";
	unix->event.callback = GBASIOUnixProcessEvents;
	unix->event.priority = 0x80;
	unix->socket = 0;
	unix->peer_socket = 0;
	unix->cycles_to_wait = 5000;
	unix->receive_fifo = 0;
	unix->send_fifo = 0;
	unix->receive_fifo_offset = 0;
	unix->send_fifo_offset = 0;

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
	sleep(2);
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
	UNUSED(driver);
	UNUSED(mode);
}

static bool GBASIOUnixHandlesMode(struct GBASIODriver* driver, enum GBASIOMode mode) {
	UNUSED(driver);
	return mode == GBA_SIO_UART;
}

static int GBASIOUnixConnectedDevices(struct GBASIODriver* driver) {
	UNUSED(driver);
	return 0;
}

static uint16_t GBASIOUnixWriteSIOCNT(struct GBASIODriver* driver, uint16_t value) {
	struct GBASIOUnix * unix = (struct GBASIOUnix *) driver;
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

	unix->uart_bytes = value;
	/* Transmit / Receive time (in cpu cycles) for a single byte at the given baud */
	unix->cycles_to_wait = 8 * (GBA_ARM7TDMI_FREQUENCY / sio_uart_baud[unix->uart.baud]);

	return value;
}

static char _convert(uint32_t in, uint8_t offset) {
	char value = (char) ((in >> (8 * offset)) & 0xff);
	if (value <= 0x20) {
		value = '_';
	}
	if (value >= 0x7F) {
		value = '?';
	}
	return value;
}

static uint8_t GABSIOUnixWriteSIODATA8(struct GBASIODriver* driver, uint8_t value) {
	struct GBASIOUnix* unix = (struct GBASIOUnix*) driver;

	if (unix->uart.fifo_enable) {
		/* Todo: How does hardware behave when the fifo is full?
		 * Overwriting the last byte?
		 * Dropping the new write attempt?
		 * Cycling back to the front of the "queue"? */
		printf("write: send_fifo: [%c|%c|%c|%c][%u], value: %c\n",
			_convert(unix->send_fifo, 3),
			_convert(unix->send_fifo, 2),
			_convert(unix->send_fifo, 1),
			_convert(unix->send_fifo, 0),
			unix->send_fifo_offset, value);
		if (unix->send_fifo_offset < 4) {
			unix->send_fifo |= (uint32_t) value << (unix->send_fifo_offset * 8);
			unix->send_fifo_offset++;
		}
		if (unix->send_fifo_offset >= 4) {
			puts("Fifo full!");
			unix->uart.send_data = 1;
			unix->siodriver.p->p->sio.siocnt = unix->uart_bytes;
		}
	} else {
		unix->send_fifo = value;
		/* Mark that we got data, so GBASIOUnixProcessEvents knows it should send something */
		unix->send_fifo_offset = 1;
		unix->uart.send_data = 1;
		unix->siodriver.p->p->sio.siocnt = unix->uart_bytes;
	}

	return value;
}

static uint8_t GABSIOUnixReadSIODATA8(struct GBASIODriver* driver, uint8_t value) {
	struct GBASIOUnix* unix = (struct GBASIOUnix*) driver;
	printf("GABSIOUnixReadSIODATA8: Read data\n");
	unix->uart.receive_data = 1;
	unix->siodriver.p->p->sio.siocnt = unix->uart_bytes;
	return value;
}

static void GBASIOUnixProcessEvents(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	struct GBASIOUnix * unix = (struct GBASIOUnix *) context;
	//printf("GBASIOUnixProcessEvents: %u cyclesLate\n", cyclesLate);
	if (unix->peer_socket) {
		uint8_t buf[1];
		if (read(unix->peer_socket, buf, sizeof(buf)) == 1) {
			printf("GBASIOUnixProcessEvents: Read data from peer at %lu - %lu\n", timing->globalCycles, cyclesLate);
			if (unix->uart.receive_enable) {
				if (unix->uart.fifo_enable) {

				} else {

				}
				if (unix->siodriver.p->p->memory.io[GBA_REG(IF)] & (1 << GBA_IRQ_SIO)) {
					printf("GBASIOUnixProcessEvents: Last interrupt has not been cleared yet!\n");
					//printf("PC: %06X\n", unix->siodriver.p->p->cpu->gprs[ARM_PC]);
				}
				unix->siodriver.p->p->memory.io[GBA_REG(SIODATA8)] = buf[0];
				unix->uart.receive_data = 0;
				unix->siodriver.p->p->sio.siocnt = unix->uart_bytes;

				GBARaiseIRQ(unix->siodriver.p->p, GBA_IRQ_SIO, cyclesLate);
			} else {
				printf("GBASIOUnixProcessEvents: Received data but receiving is disabled\n");
			}
		}
	}

	if (unix->uart.send_enable) {
		if (unix->send_fifo_offset > 0) {
			uint8_t value = unix->send_fifo & 0xff;
			printf("send : send_fifo: [%c|%c|%c|%c][%u], value: %c\n",
			_convert(unix->send_fifo, 3),
			_convert(unix->send_fifo, 2),
			_convert(unix->send_fifo, 1),
			_convert(unix->send_fifo, 0),
			unix->send_fifo_offset, value);
			unix->send_fifo >>= 8;
			unix->send_fifo_offset--;

			unix->uart.send_data = 0;
			unix->siodriver.p->p->sio.siocnt = unix->uart_bytes;

			if (unix->peer_socket && write(unix->peer_socket, &value, 1) != 1) {
				puts("GBASIOUnixProcessEvents: Write failed");
				GBASIOUnixReset((struct GBASIODriver *) context);
			}
		}
	} else {
		if (unix->send_fifo_offset > 0) {
			puts("GBASIOUnixProcessEvents: Got data but sending is disabled");
		}
	}

	if (!unix->peer_socket) {
		int fd = accept(unix->socket, NULL, NULL);
		if (fd > 0) {
			if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) != -1) {
				puts("GBASIOUnixProcessEvents: Accepted peer");
				unix->peer_socket = fd;
			}
		}
	}
	mTimingSchedule(timing, &unix->event, unix->cycles_to_wait);
}
