/*
$Id$
    OWFS -- One-Wire filesystem
    OWHTTPD -- One-Wire Web Server
    Written 2003 Paul H Alfille
    email: palfille@earthlink.net
    Released under the GPL
    See the header file: ow.h for full attribution
    1wire/iButton system from Dallas Semiconductor
*/

#include <config.h>
#include "owfs_config.h"
#include "ow.h"
#include "ow_counters.h"
#include "ow_connection.h"

/* All the rest of the program sees is the DS9907_detect and the entry in iroutines */

static RESET_TYPE DS9097_reset(const struct parsedname *pn);
static GOOD_OR_BAD DS9097_sendback_bits(const BYTE * outbits, BYTE * inbits, const size_t length, const struct parsedname *pn);
static void DS9097_setroutines(struct connection_in *in);
static GOOD_OR_BAD DS9097_send_and_get(const BYTE * bussend, BYTE * busget, const size_t length, const struct parsedname *pn);

#define	OneBit	0xFF
//#define ZeroBit 0xC0
// Should be all zero's when we send 8 bits. digitemp write 0xFF or 0x00
#define ZeroBit 0x00

/* Device-specific functions */
static void DS9097_setroutines(struct connection_in *in)
{
	in->iroutines.detect = DS9097_detect;
	in->iroutines.reset = DS9097_reset;
	in->iroutines.next_both = NULL;
	in->iroutines.PowerByte = NULL;
//    in->iroutines.ProgramPulse = ;
	in->iroutines.sendback_data = NULL;
	in->iroutines.sendback_bits = DS9097_sendback_bits;
	in->iroutines.select = NULL;
	in->iroutines.reconnect = NULL;
	in->iroutines.close = COM_close;
	in->iroutines.flags = ADAP_FLAG_overdrive;
	in->bundling_length = UART_FIFO_SIZE / 10;
}

/* _detect is a bit of a misnomer, no detection is actually done */
// no bus locking here (higher up)
GOOD_OR_BAD DS9097_detect(struct connection_in *in)
{
	struct parsedname pn;

	/* open the COM port in 9600 Baud  */
	if (COM_open(in)) {
		return gbBAD;
	}

	/* Set up low-level routines */
	DS9097_setroutines(in);

	in->Adapter = adapter_DS9097;
	// in->adapter_name already set, to support HA3 and HA4B
	in->busmode = bus_passive;	// in case initially tried DS9097U

	FS_ParsedName_Placeholder(&pn);	// minimal parsename -- no destroy needed
	pn.selected_connection = in;

	switch( DS9097_reset(&pn) ) {
		case BUS_RESET_OK:
		case BUS_RESET_SHORT:
			return gbGOOD ;
		default:
			return gbBAD ;
	}
}

/* DS9097 Reset -- A little different from DS2480B */
/* Puts in 9600 baud, sends 11110000 then reads response */
static RESET_TYPE DS9097_reset(const struct parsedname *pn)
{
	BYTE resetbyte = 0xF0;
	BYTE c;
	struct termios term;
	FILE_DESCRIPTOR_OR_ERROR file_descriptor = pn->selected_connection->file_descriptor;
	RESET_TYPE ret;

	if ( FILE_DESCRIPTOR_NOT_VALID(file_descriptor) ) {
		return -EINVAL;
	}

	/* 8 data bits */
	//valgrind warn about uninitialized memory in tcsetattr(), so clear all.
	memset(&term, 0, sizeof(struct termios));
	tcgetattr(file_descriptor, &term);
	term.c_cflag = CS8 | CREAD | HUPCL | CLOCAL;
	if (cfsetospeed(&term, B9600) < 0 || cfsetispeed(&term, B9600) < 0) {
		ERROR_CONNECT("Cannot set speed (9600): %s", SAFESTRING(pn->selected_connection->name));
	}
	if (tcsetattr(file_descriptor, TCSANOW, &term) < 0) {
		ERROR_CONNECT("Cannot set attributes: %s", SAFESTRING(pn->selected_connection->name));
		return -EIO;
	}
	if ( BAD( DS9097_send_and_get(&resetbyte, &c, 1, pn)) ) {
		return BUS_RESET_ERROR ;
	}

	switch (c) {
	case 0:
		ret = BUS_RESET_SHORT;
		break;
	case 0xF0:
		ret = BUS_RESET_OK;
		pn->selected_connection->AnyDevices = anydevices_no ;
		break;
	default:
		ret = BUS_RESET_OK;
		pn->selected_connection->AnyDevices = anydevices_yes ;
		pn->selected_connection->ProgramAvailable = 0;	/* from digitemp docs */
		if (pn->selected_connection->ds2404_compliance) {
			// extra delay for alarming DS1994/DS2404 compliance
			UT_delay(5);
		}
	}

	/* Reset all settings */
	term.c_lflag = 0;
	term.c_iflag = 0;
	term.c_oflag = 0;

	/* 1 byte at a time, no timer */
	term.c_cc[VMIN] = 1;
	term.c_cc[VTIME] = 0;

#if 0
	/* digitemp seems to contain a really nasty bug.. in
	   SMALLINT owTouchReset(int portnum)
	   They use 8bit all the time actually..
	   8 data bits */
	term[portnum].c_cflag |= CS8;	//(0x60)
	cfsetispeed(&term[portnum], B9600);
	cfsetospeed(&term[portnum], B9600);
	tcsetattr(file_descriptor[portnum], TCSANOW, &term[portnum]);
	send_reset_byte(0xF0);
	cfsetispeed(&term[portnum], B115200);
	cfsetospeed(&term[portnum], B115200);
	/* set to 6 data bits */
	term[portnum].c_cflag |= CS6;	// (0x20)
	tcsetattr(file_descriptor[portnum], TCSANOW, &term[portnum]);
	/* Not really a change of data-bits here...
	   They always use 8bit mode... doohhh? */
#endif

	if (Globals.eightbit_serial) {
		/* coninue with 8 data bits */
		term.c_cflag = CS8 | CREAD | HUPCL | CLOCAL;
	} else {
		/* 6 data bits, Receiver enabled, Hangup, Dont change "owner" */
		term.c_cflag = CS6 | CREAD | HUPCL | CLOCAL;
	}
#ifndef B115200
	/* MacOSX support max 38400 in termios.h ? */
	if (cfsetospeed(&term, B38400) < 0 || cfsetispeed(&term, B38400) < 0) {
		ERROR_CONNECT("Cannot set speed (38400): %s", SAFESTRING(pn->selected_connection->name));
	}
#else
	if (cfsetospeed(&term, B115200) < 0 || cfsetispeed(&term, B115200) < 0) {
		ERROR_CONNECT("Cannot set speed (115200): %s", SAFESTRING(pn->selected_connection->name));
	}
#endif

	if (tcsetattr(file_descriptor, TCSANOW, &term) < 0) {
		ERROR_CONNECT("Cannot set attributes: %s", SAFESTRING(pn->selected_connection->name));
		return -EFAULT;
	}
	/* Flush the input and output buffers */
	COM_flush(pn->selected_connection);
	return ret;
}

#if 0
/* Adapted from http://www.linuxquestions.org/questions/showthread.php?t=221632 */
static int setRTS(int on, const struct connection_in *in)
{
	int status;

	if (ioctl(in->file_descriptor, TIOCMGET, &status) == -1) {
		ERROR_CONNECT("setRTS(): TIOCMGET");
		return 0;
	}
	if (on) {
		status |= TIOCM_RTS;
	} else {
		status &= ~TIOCM_RTS;
	}
	if (ioctl(in->file_descriptor, TIOCMSET, &status) == -1) {
		ERROR_CONNECT("setRTS(): TIOCMSET");
		return 0;
	}
	return 1;
}
#endif							/* 0 */

/* Symmetric */
/* send bits -- read bits */
/* Actually uses bit zero of each byte */
/* Dispatches DS9097_MAX_BITS "bits" at a time */
#define DS9097_MAX_BITS 24
static GOOD_OR_BAD DS9097_sendback_bits(const BYTE * outbits, BYTE * inbits, const size_t length, const struct parsedname *pn)
{
	BYTE d[DS9097_MAX_BITS];
	size_t l = 0;
	size_t i = 0;
	size_t start = 0;

	if (length == 0) {
		return gbGOOD;
	}

	/* Split into smaller packets? */
	do {
		d[l++] = outbits[i++] ? OneBit : ZeroBit;
		if (l == DS9097_MAX_BITS || i == length) {
			/* Communication with DS9097 routine */
			if ( BAD( DS9097_send_and_get(d, &inbits[start], l, pn)) ) {
				STAT_ADD1_BUS(e_bus_errors, pn->selected_connection);
				return gbBAD;
			}
			l = 0;
			start = i;
		}
	} while (i < length);

	/* Decode Bits */
	for (i = 0; i < length; ++i) {
		inbits[i] &= 0x01;
	}

	return gbGOOD;
}

/* Routine to send a string of bits and get another string back */
/* This seems rather COM-port specific */
/* Indeed, will move to DS9097 */
static GOOD_OR_BAD DS9097_send_and_get(const BYTE * bussend, BYTE * busget, const size_t length, const struct parsedname *pn)
{
	if ( COM_write( bussend, length, pn->selected_connection ) < 0 ) {
		return gbBAD ;
	}	

	/* get back string -- with timeout and partial read loop */
	return COM_read( busget, length, pn->selected_connection ) ? gbBAD : gbGOOD ;
}
