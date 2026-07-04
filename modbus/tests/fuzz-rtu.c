/* libFuzzer harness: RTU frame parser in both roles (the first input
 * byte selects master/slave mode), fed one byte at a time, decoding each
 * completed frame per role. */

#include "../csmb-private.h"
#include "fuzz-main.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    csmb_rtu_parser p;
    const uint8_t *in;
    size_t left;
    int slave_mode;

    if (size == 0)
        return 0;
    slave_mode = data[0] & 1;   /* first byte selects the role */
    in = data + 1;
    left = size - 1;

    csmb_rtu_parser_init(&p, slave_mode);
    while (left > 0) {
        const uint8_t *chunk = in;
        size_t n = 1;
        csmb_parse_ret r = csmb_rtu_parser_feed(&p, &chunk, &n);

        in++;
        left--;
        if (r == CSMB_PR_FRAME) {
            if (slave_mode) {
                csmb_request req;

                csmb_decode_request(p.pdu, p.plen, &req);
            } else {
                csmb_response resp;
                uint8_t fc = p.plen ? (uint8_t)(p.pdu[0] & 0x7f) : 0;

                csmb_decode_response(p.pdu, p.plen, fc, &resp);
            }
            csmb_rtu_parser_reset(&p);
        } else if (r == CSMB_PR_BAD) {
            csmb_rtu_parser_reset(&p);
        }
    }
    return 0;
}
