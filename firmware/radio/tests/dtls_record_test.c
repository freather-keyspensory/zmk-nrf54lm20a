/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <string.h>
#include "../src/secure_record.h"
int main(void)
{
 uint8_t packet[256] = {23, 0xfe, 0xfd, 0, 1}; packet[12] = 16;
 assert(rf_dtls_framing(packet, 29, true));
 for (size_t n = 0; n < 29; n++) assert(!rf_dtls_framing(packet, n, false));
 assert(!rf_dtls_framing(packet, 30, false));
 assert(!rf_dtls_framing(packet, 256, false));
 memcpy(packet+29, packet, 29); packet[29] = 22; packet[29+4] = 0; packet[29+2] = 0xff;
 assert(rf_dtls_framing(packet, 58, false));
 assert(!rf_dtls_framing(packet, 58, true)); /* appended clear handshake */
 for (size_t n = 30; n < 58; n++) assert(!rf_dtls_framing(packet, n, false));
 packet[29+4] = 1;
 assert(!rf_dtls_framing(packet, 58, false)); /* encrypted old-version record */
 packet[29+2] = 0xfd;
 assert(rf_dtls_framing(packet, 58, true));
 packet[11] = 0xff; assert(!rf_dtls_framing(packet, 58, false)); packet[11] = 0;
 packet[12] = 0; assert(!rf_dtls_framing(packet, 58, false)); packet[12] = 16;
 packet[0] = 82; assert(!rf_dtls_framing(packet, 29, false));
 return 0;
}
