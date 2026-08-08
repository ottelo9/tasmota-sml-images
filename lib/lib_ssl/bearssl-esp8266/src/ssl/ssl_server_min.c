/*
 * Copyright (c) 2016 Thomas Pornin <pornin@bolet.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * br_ssl_server_zero() + br_ssl_server_reset() aus dem Upstream-BearSSL
 * (src/ssl/ssl_server.c). Die Tasmota-Lib wurde clientseitig zugeschnitten;
 * die Server-Handshake-Maschine (ssl_hs_server.c) und die EC-Server-Policy
 * (ssl_scert_single_ec.c) sind vorhanden, nur diese beiden Einstiegs-
 * funktionen fehlten. Struktur gespiegelt an br_ssl_client_reset()
 * (ssl_client.c) dieser Lib. Benoetigt vom SHIP-Server in
 * xdrv_126_eebus_guard.ino (USE_EEBUS_GUARD, eingehende SHIP-Verbindungen).
 */

#include "t_inner.h"

/* see bearssl_ssl.h */
void
br_ssl_server_zero(br_ssl_server_context *cc)
{
	/*
	 * For really standard C, we should explicitly set to NULL all
	 * pointers, and 0 all other fields. However, on all our target
	 * architectures, a direct memset() will set all bytes to 0,
	 * which means that all 'normal' types will be set to 0, and
	 * pointers will be NULL.
	 */
	memset(cc, 0, sizeof *cc);
}

/* see bearssl_ssl.h */
int
br_ssl_server_reset(br_ssl_server_context *cc)
{
	br_ssl_engine_set_buffer(&cc->eng, NULL, 0, 0);
	cc->eng.version_out = cc->eng.version_min;
	if (!br_ssl_engine_init_rand(&cc->eng)) {
		return 0;
	}
	cc->eng.reneg = 0;
	br_ssl_engine_hs_reset(&cc->eng,
		br_ssl_hs_server_init_main, br_ssl_hs_server_run);
	return br_ssl_engine_last_error(&cc->eng) == BR_ERR_OK;
}
