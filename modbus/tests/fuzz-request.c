/* libFuzzer harness: csmb_decode_request over arbitrary PDUs, expanding
 * clean write requests through csmb_request_values, and csmb_decode_response
 * over the same bytes. */

#include "../csmb-private.h"
#include "fuzz-main.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    csmb_request req;
    csmb_response resp;
    uint16_t vals[CSMB_MAX_WRITE_COILS];

    if (size > CSMB_MAX_PDU)
        size = CSMB_MAX_PDU;

    if (csmb_decode_request(data, size, &req) == 0 && req.exc == CSMB_EXC_NONE) {
        switch (req.fc) {
        case CSMB_FC_WRITE_SINGLE_COIL:
        case CSMB_FC_WRITE_SINGLE_REG:
        case CSMB_FC_WRITE_MULTI_COILS:
        case CSMB_FC_WRITE_MULTI_REGS:
            csmb_request_values(&req, vals);
            break;
        default:
            break;
        }
    }
    csmb_decode_response(data, size, size ? (uint8_t)(data[0] & 0x7f) : 0, &resp);
    return 0;
}
