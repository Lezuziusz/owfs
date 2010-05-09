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
#include "ow_codes.h"

/** BUS_send_data
    Send a data and expect response match
    puts into data mode if needed.
    return 0=good
    bad return sendback_data
    -EIO if memcpy
 */
GOOD_OR_BAD BUS_send_data(const BYTE * data, const size_t len, const struct parsedname *pn)
{
	GOOD_OR_BAD ret = gbGOOD;
	BYTE *resp;

	if (len == 0) {
		return gbGOOD;
	}

	resp = owmalloc(len);
	if (resp == NULL) {
		return gbBAD;
	}

	if ( BAD( BUS_sendback_data(data, resp, len, pn) ) ) {
		ret = gbBAD ;
		if ((ret = memcmp(data, resp, (size_t) len))) {
			ret = gbBAD;			/* EPROTO not available for MacOSX */
			STAT_ADD1_BUS(e_bus_errors, pn->selected_connection);
		}
	}
	owfree(resp);
	return ret;
}

/** readin_data
  Send 0xFFs and return response block
  Returns 0=good
  Bad sendback_data
 */
GOOD_OR_BAD BUS_readin_data(BYTE * data, const size_t len, const struct parsedname *pn)
{
	GOOD_OR_BAD ret = BUS_sendback_data(memset(data, 0xFF, (size_t) len), data, len, pn);
	if (ret) {
		STAT_ADD1(BUS_readin_data_errors);
	}
	return ret;
}

// ----------------------------------------------------------------
// Low level default routines, often superceded by more capable adapters

/* Symmetric */
/* send bytes, and read back -- calls lower level bit routine */
GOOD_OR_BAD BUS_select_and_sendback(const BYTE * data, BYTE * resp, const size_t len, const struct parsedname *pn)
{
	if ( FunctionExists(pn->selected_connection->iroutines.select_and_sendback) ) {
		return (pn->selected_connection->iroutines.select_and_sendback) (data, resp, len, pn);
	} else {
		RETURN_ERROR_IF_BAD( BUS_select(pn) );
		return BUS_sendback_data(data, resp, len, pn);
	}
}

/* Symmetric */
/* send bytes, and read back -- calls lower level bit routine */
GOOD_OR_BAD BUS_sendback_data(const BYTE * data, BYTE * resp, const size_t len, const struct parsedname *pn)
{
	UINT i, bits = len << 3;
	int ret;
	int combuffer_length_adjusted = MAX_FIFO_SIZE >> 3;
	int remain = len - combuffer_length_adjusted;

	if (len == 0) {
		return gbGOOD;
	}
	if ( FunctionExists(pn->selected_connection->iroutines.sendback_data) ) {
		return (pn->selected_connection->iroutines.sendback_data) (data, resp, len, pn);
	}

	/* Split into smaller packets? */
	if (remain > 0) {
		RETURN_BAD_IF_BAD( BUS_sendback_data(data, resp, combuffer_length_adjusted, pn) );
		RETURN_BAD_IF_BAD( BUS_sendback_data(&data[combuffer_length_adjusted], resp ? (&resp[combuffer_length_adjusted]) : NULL, remain, pn) );
	}

	/* Encode bits */
	for (i = 0; i < bits; ++i) {
		pn->selected_connection->combuffer[i] = UT_getbit(data, i) ? 0xFF : 0x00;
	}

	/* Communication with DS9097 routine */
	if ( BAD( BUS_sendback_bits(pn->selected_connection->combuffer, pn->selected_connection->combuffer, bits, pn)) ) {
		STAT_ADD1_BUS(e_bus_errors, pn->selected_connection);
		return gbBAD;
	}

	/* Decode Bits */
	if (resp) {
		for (i = 0; i < bits; ++i) {
			UT_setbit(resp, i, pn->selected_connection->combuffer[i] & 0x01);
		}
	}

	return gbGOOD;
}
