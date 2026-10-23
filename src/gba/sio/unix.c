/* Copyright (c) 2026 Bennet Hattesen
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <errno.h>
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

static inline char _convert(uint32_t in, uint8_t offset) {
	char value = (char) ((in >> (8 * offset)) & 0xff);
	if (value == 0x00) {
		return '_';
	}
	if (value <= 0x20) {
		return '?';
	}
	if (value >= 0x7F) {
		return '?';
	}
	return value;
}

static inline void _print_fifo(uint32_t fifo, unsigned offset, uint8_t value, char * name) {
	printf("%s: [%c|%c|%c|%c](%u), value: %c\n",
		name,
		_convert(fifo, 3), _convert(fifo, 2), _convert(fifo, 1), _convert(fifo, 0),
		offset, value);
}

static inline uint8_t _fifo_pop(uint32_t *fifo, unsigned *offset) {
	mASSERT_DEBUG(*offset > 0);
	/* Get the oldest byte */
	uint8_t first_out = *fifo & 0xff;
	/* Advance fifo 8 bits, discarding the oldes byte */
	*fifo >>= 8;
	/* One byte less in the fifo */
	(*offset)--;

	return first_out;
}

static inline void _fifo_push(uint32_t *fifo, unsigned *offset, uint8_t value) {
	mASSERT_DEBUG(*offset < 4);
	/* Offset is in bytes, shift in bits */
	unsigned shift = (*offset) * 8;
	/* zero the bits where our byte will be placed */
	uint32_t mask = ~((uint32_t) 0xff << shift);
	*fifo &= mask;
	/* add our byte */
	*fifo |= (uint32_t) value << shift;
	/* One byte more in the fifo */
	(*offset)++;
}

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

	/*
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
	*/

	unix->uart_bytes = value;
	/* Transmit / Receive time (in cpu cycles) for a single byte at the given baud */
	unix->cycles_to_wait = 8 * (GBA_ARM7TDMI_FREQUENCY / sio_uart_baud[unix->uart.baud]);

	return value;
}

static uint8_t GABSIOUnixWriteSIODATA8(struct GBASIODriver* driver, uint8_t value) {
	struct GBASIOUnix* unix = (struct GBASIOUnix*) driver;

	_print_fifo(unix->send_fifo, unix->send_fifo_offset, value, "wrte: send_fifo");

	if (unix->uart.fifo_enable) {
		/* Todo: How does hardware behave when the fifo is full?
		 * Overwriting the last byte?
		 * Dropping the new write attempt?
		 * Cycling back to the front of the "queue"? */

		/* Is there still space left in the send buffer? */
		if (unix->send_fifo_offset < 4) {
			_fifo_push(&unix->send_fifo, &unix->send_fifo_offset, value);
		} else {
			puts("GABSIOUnixWriteSIODATA8: Send Fifo full, dropped  byte!");
			/* Todo: Does the real hardware overwrite the latest byte? Drop the first byte? */
		}

		/* Is the send buffer full now? */
		if (unix->send_fifo_offset >= 4) {
			unix->uart.send_fifo_full = 1; /* 0=Not full, 1=Full */
			unix->siodriver.p->p->sio.siocnt = unix->uart_bytes;
		}
	} else {
		/* If the fifo is disabled, we pretend our fifo has a space of one (instead of four). */
		unix->send_fifo = value;
		/* Mark that we got data, so GBASIOUnixProcessEvents knows it could send something */
		unix->send_fifo_offset = 1;
		unix->uart.send_fifo_full = 1; /* 0=Not full, 1=Full */
		unix->siodriver.p->p->sio.siocnt = unix->uart_bytes;
	}

	return value;
}

static uint8_t GABSIOUnixReadSIODATA8(struct GBASIODriver* driver, uint8_t value) {
	UNUSED(value);
	struct GBASIOUnix* unix = (struct GBASIOUnix*) driver;

	/* Receiving data is not enabled */
	if (!unix->uart.receive_enable) {
		/* Todo: Does real hardware return zero or the last value that was in the reg? */
		return 0;
	}

	/* Is the receive buffer empty? */
	if (unix->receive_fifo_offset == 0) {
		puts("GABSIOUnixReadSIODATA8: Got no data");
		/* Todo: Does real hardware return zero or the last value that was in the reg? */
		return 0;
	}

	_print_fifo(
		unix->receive_fifo, unix->receive_fifo_offset, unix->receive_fifo & 0xff, "read: receive_fifo"
	);

	/* Buffer is not empty: unix->receive_fifo_offset > 0
	 * We always read from the fifo, even if the fifo was not enabled by the application.
	 * In that case we just pretend our fifo has a capacity of one (instead of four). */
	uint8_t oldest_byte_from_fifo = _fifo_pop(&unix->receive_fifo, &unix->receive_fifo_offset);

	/* Is the fifo empty now? */
	if (unix->receive_fifo_offset == 0) {
		unix->uart.receive_fifo_empty = 1; /* 0=Not empty, 1=Empty */
		unix->siodriver.p->p->sio.siocnt = unix->uart_bytes;
	}

	return oldest_byte_from_fifo;
}

static ssize_t _receive_byte(struct GBASIOUnix* unix, uint32_t cyclesLate) {
	if (!unix->peer_socket) {
		return 1;
	}

	if (unix->uart.fifo_enable && unix->receive_fifo_offset == 4) {
		// keep data in socket buffer if fifo is full
		return 1;
	}

	if (!unix->uart.fifo_enable && unix->receive_fifo_offset > 0) {
		// keep data in socket buffer if fifo is full
		return 1;
	}

	uint8_t data = 0;
	ssize_t ret = read(unix->peer_socket, &data, sizeof(data));
	/* Expecting exactly one byte */
	if (ret != 1) {
		return ret;
	}

	if (!unix->uart.receive_enable) {
		puts("GBASIOUnixProcessEvents: Received data but receiving is disabled");
		return 1;
	}

	if (unix->uart.fifo_enable) {
		if (unix->receive_fifo_offset < 4) {
			_fifo_push(&unix->receive_fifo, &unix->receive_fifo_offset, data);
		} else {
			/* ToDo: What does hardware do when the last byte wasn't read by the application yet?
		 	 * Overwrite the last byte? Drop the new byte? */
			puts("GBASIOUnixProcessEvents: Receive Fifo full, dropped  byte!");
		}
	} else {
		/* ToDo: What does hardware do when the last byte wasn't read by the application yet?
		 * Overwrite the last byte? Drop the new byte? */
		unix->receive_fifo = data;
		/* Mark that we got data */
		unix->receive_fifo_offset = 1;
	}

	_print_fifo(
		unix->receive_fifo, unix->receive_fifo_offset, data, "recv: receive_fifo"
	);

	/* We added a byte, mark the receive buffer as non-empty */
	unix->uart.receive_fifo_empty = 0; /* 0=Not empty, 1=Empty */
	unix->siodriver.p->p->sio.siocnt = unix->uart_bytes;

	/* Only raise IRQ when the flag has been cleared */
	if (!(unix->siodriver.p->p->memory.io[GBA_REG(IF)] & (1 << GBA_IRQ_SIO))) {
		GBARaiseIRQ(unix->siodriver.p->p, GBA_IRQ_SIO, cyclesLate);
	}

	return 1;
}

static ssize_t _send_byte(struct GBASIOUnix* unix) {
	/* Can only send data if there is data */
	if (unix->send_fifo_offset == 0) {
		return 1;
	}

	/* Don't send data if sending is disabled */
	if (!unix->uart.send_enable) {
		puts("GBASIOUnixProcessEvents: Got data but sending is disabled");
		return 1;
	}

	_print_fifo(
		unix->send_fifo, unix->send_fifo_offset, unix->send_fifo & 0xff, "send: send_fifo"
	);

	/* We always read from the fifo, even if the fifo was not enabled by the application.
	 * In that case we just pretend our fifo has a capacity of one (instead of four). */
	uint8_t oldest_byte_from_fifo = _fifo_pop(&unix->send_fifo, &unix->send_fifo_offset);

	/* We just popped a byte, can not be full anymore */
	unix->uart.send_fifo_full = 0; /* 0=Not full, 1=Full */
	unix->siodriver.p->p->sio.siocnt = unix->uart_bytes;

	if (unix->peer_socket) {
		return write(unix->peer_socket, &oldest_byte_from_fifo, 1);
	} else {
		return 1;
	}
}

static void GBASIOUnixProcessEvents(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	struct GBASIOUnix * unix = (struct GBASIOUnix*) context;

	if (!unix->peer_socket) {
		int fd = accept(unix->socket, NULL, NULL);
		if (fd > 0) {
			if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) != -1) {
				unix->peer_socket = fd;
			}
		}
	}

	if (_send_byte(unix) != 1) {
		GBASIOUnixReset((struct GBASIODriver *) context);
		return;
	}

	ssize_t ret = _receive_byte(unix, cyclesLate);
	if ((ret == 0) || ((ret == -1) && (errno != EAGAIN && errno != EWOULDBLOCK))) {
		GBASIOUnixReset((struct GBASIODriver *) context);
		return;
	}

	mTimingSchedule(timing, &unix->event, unix->cycles_to_wait);
}
