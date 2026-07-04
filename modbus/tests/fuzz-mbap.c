/* libFuzzer harness: MBAP frame parser fed one byte at a time (maximal
 * split stress), decoding each completed frame as both a response and a
 * request. */

#include "../csmb-private.h"
#include "fuzz-main.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    csmb_mbap_parser p;
    const uint8_t *in = data;
    size_t left = size;

    csmb_mbap_parser_init(&p);
    while (left > 0) {
        const uint8_t *chunk = in;
        size_t n = 1;
        csmb_parse_ret r = csmb_mbap_parser_feed(&p, &chunk, &n);

        in++;
        left--;
        if (r == CSMB_PR_FRAME) {
            csmb_response resp;
            csmb_request req;
            uint8_t fc = p.plen ? (uint8_t)(p.pdu[0] & 0x7f) : 0;

            csmb_decode_response(p.pdu, p.plen, fc, &resp);
            csmb_decode_request(p.pdu, p.plen, &req);
            csmb_mbap_parser_reset(&p);
        } else if (r == CSMB_PR_BAD) {
            csmb_mbap_parser_init(&p);
        }
    }
    return 0;
}
