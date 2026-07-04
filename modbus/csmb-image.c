/* Slave register image: per (unit, reg_type) sorted block lists.
 *
 * Pure logic, no libwebsockets.  Blocks store one uint16 per register
 * (coils/discretes: one per bit position).  Safe blocks accept writes
 * but discard them and always read 0.  See csmb-private.h. */

#include "csmb-private.h"

void csmb_image_init(csmb_image *img)
{
    img->units = NULL;
}

void csmb_image_free(csmb_image *img)
{
    csmb_unit_image *u = img->units;

    while (u) {
        csmb_unit_image *un = u->next;
        int rt;

        for (rt = 0; rt < CSMB_NUM_REG_TYPES; rt++) {
            csmb_block *b = u->blocks[rt];

            while (b) {
                csmb_block *bn = b->next;
                free(b);
                b = bn;
            }
        }
        free(u);
        u = un;
    }
    img->units = NULL;
}

static csmb_unit_image *find_unit(csmb_image *img, uint8_t unit)
{
    csmb_unit_image *u;

    for (u = img->units; u; u = u->next)
        if (u->unit == unit)
            return u;
    return NULL;
}

static csmb_unit_image *ensure_unit(csmb_image *img, uint8_t unit)
{
    csmb_unit_image *u = find_unit(img, unit);

    if (u)
        return u;
    u = calloc(1, sizeof(*u));
    if (!u)
        return NULL;
    u->unit = unit;
    u->next = img->units;
    img->units = u;
    return u;
}

/* The block of REG_TYPE containing ADDR, or NULL.  Lists are sorted by
 * start, so a block whose start already exceeds ADDR ends the search. */
static csmb_block *find_block(csmb_unit_image *u, int reg_type, uint16_t addr)
{
    csmb_block *b;

    for (b = u->blocks[reg_type]; b; b = b->next) {
        if (addr < b->start)
            return NULL;
        if ((uint32_t)addr < (uint32_t)b->start + b->count)
            return b;
    }
    return NULL;
}

int csmb_image_register(csmb_image *img, uint8_t unit, int reg_type,
                        uint16_t start, uint16_t count, int writable, int safe)
{
    csmb_unit_image *u;
    csmb_block **pp, *b;

    if (reg_type < 0 || reg_type >= CSMB_NUM_REG_TYPES)
        return CSMB_EBADTYPE;
    if (count == 0 || (uint32_t)start + count > 0x10000u)
        return CSMB_ERANGE;

    u = ensure_unit(img, unit);
    if (!u)
        return CSMB_ENOMEM;

    /* Walk to the sorted insert point, rejecting any overlap. */
    pp = &u->blocks[reg_type];
    while (*pp && (*pp)->start < start) {
        if ((uint32_t)(*pp)->start + (*pp)->count > start)
            return CSMB_EOVERLAP;
        pp = &(*pp)->next;
    }
    if (*pp && (uint32_t)start + count > (*pp)->start)
        return CSMB_EOVERLAP;

    b = calloc(1, sizeof(*b) + (size_t)count * sizeof(uint16_t));
    if (!b)
        return CSMB_ENOMEM;
    b->start = start;
    b->count = count;
    b->writable = writable ? 1 : 0;
    b->safe = safe ? 1 : 0;
    b->next = *pp;
    *pp = b;
    return CSMB_OK;
}

int csmb_image_set_values(csmb_image *img, uint8_t unit, int reg_type,
                          uint16_t start, uint16_t count, const uint16_t *values)
{
    csmb_unit_image *u;
    csmb_block *b;
    uint16_t i;

    if (reg_type < 0 || reg_type >= CSMB_NUM_REG_TYPES)
        return CSMB_EBADTYPE;
    if (count == 0)
        return CSMB_ERANGE;
    u = find_unit(img, unit);
    if (!u)
        return CSMB_ERANGE;
    b = find_block(u, reg_type, start);
    if (!b || (uint32_t)start + count > (uint32_t)b->start + b->count)
        return CSMB_ERANGE;
    if (!b->safe)
        for (i = 0; i < count; i++)
            b->regs[start - b->start + i] = values[i];
    return CSMB_OK;
}

int csmb_image_touch_unit(csmb_image *img, uint8_t unit)
{
    return ensure_unit(img, unit) ? CSMB_OK : CSMB_ENOMEM;
}

int csmb_image_unit_has_ranges(csmb_image *img, uint8_t unit)
{
    csmb_unit_image *u = find_unit(img, unit);
    int rt;

    if (!u)
        return 0;
    for (rt = 0; rt < CSMB_NUM_REG_TYPES; rt++)
        if (u->blocks[rt])
            return 1;
    return 0;
}

uint8_t csmb_image_read_range(csmb_image *img, uint8_t unit, int reg_type,
                              uint16_t start, uint16_t count, uint16_t *out)
{
    csmb_unit_image *u;
    uint16_t i;

    if (reg_type < 0 || reg_type >= CSMB_NUM_REG_TYPES ||
        count == 0 || (uint32_t)start + count > 0x10000u)
        return CSMB_EXC_ILLEGAL_ADDRESS;
    u = find_unit(img, unit);
    if (!u)
        return CSMB_EXC_ILLEGAL_ADDRESS;
    for (i = 0; i < count; i++) {
        uint16_t addr = (uint16_t)(start + i);
        csmb_block *b = find_block(u, reg_type, addr);

        if (!b)
            return CSMB_EXC_ILLEGAL_ADDRESS;
        out[i] = b->safe ? 0 : b->regs[addr - b->start];
    }
    return CSMB_EXC_NONE;
}

uint8_t csmb_image_write_range(csmb_image *img, uint8_t unit, int reg_type,
                               uint16_t start, uint16_t count,
                               const uint16_t *values,
                               csmb_apply_cb apply, void *ctx)
{
    csmb_unit_image *u;
    uint16_t i, run_start = 0, run_len = 0;

    if (reg_type < 0 || reg_type >= CSMB_NUM_REG_TYPES ||
        count == 0 || (uint32_t)start + count > 0x10000u)
        return CSMB_EXC_ILLEGAL_ADDRESS;
    u = find_unit(img, unit);
    if (!u)
        return CSMB_EXC_ILLEGAL_ADDRESS;

    /* validate first: every register must be writable or safe */
    for (i = 0; i < count; i++) {
        uint16_t addr = (uint16_t)(start + i);
        csmb_block *b = find_block(u, reg_type, addr);

        if (!b || (!b->writable && !b->safe))
            return CSMB_EXC_ILLEGAL_ADDRESS;
    }

    /* apply, emitting one run per maximal writable (non-safe) stretch */
    for (i = 0; i < count; i++) {
        uint16_t addr = (uint16_t)(start + i);
        csmb_block *b = find_block(u, reg_type, addr);

        if (b->writable && !b->safe) {
            b->regs[addr - b->start] = values[i];
            if (run_len == 0)
                run_start = addr;
            run_len++;
        } else {
            if (run_len && apply)
                apply(ctx, run_start, run_len, values + (run_start - start));
            run_len = 0;
        }
    }
    if (run_len && apply)
        apply(ctx, run_start, run_len, values + (run_start - start));
    return CSMB_EXC_NONE;
}
