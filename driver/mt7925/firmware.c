/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Adapted from mt76_connac_mcu.c and mt7925/mcu.c (MediaTek, 2020/2023).
 * Bounds validation and callback-based sequencing are Windows port additions.
 */
#include "firmware.h"
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[3] | (uint32_t)p[2] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[0] << 24;
}
static void put32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
static int span(uint32_t offset, uint32_t length, uint32_t total) {
    return offset <= total && length <= total - offset;
}
int mt_fw_validate(const uint8_t *data, uint32_t size, uint32_t kind, mt_fw_plan *out) {
    uint32_t count, table, minimum, i, j, offset = 0, entry = 0;
    const uint8_t *p;
    mt_fw_region r;
    if (!out) return MT_INVALID;
    out->count = 0; out->kind = 0; out->entry = 0;
    if (!data || size > MT_FW_MAX_SIZE) return MT_INVALID;
    if (kind == MT_FW_PATCH) {
        if (size < 96) return MT_INVALID;
        count = be32(data + 44); table = 96;
        if (!count || count > MT_FW_MAX_REGIONS || count > (size - 96) / 64) return MT_INVALID;
        minimum = 96 + count * 64;
    } else if (kind == MT_FW_RAM) {
        if (size < 36) return MT_INVALID;
        count = data[size - 34];
        if (!count || count > MT_FW_MAX_REGIONS || count > (size - 36) / 40) return MT_INVALID;
        table = size - 36 - count * 40; minimum = 0;
    } else return MT_INVALID;
    for (i=0; i<count; ++i) {
        r.skip = 0; r.override_address = 0; r.mode = 0x80000000u;
        if (kind == MT_FW_PATCH) {
            uint32_t encryption, section_size, key;
            p = data + table + i * 64;
            if ((be32(p) & 0xffffu) != 2) return MT_INVALID;
            r.offset = be32(p+4); section_size = be32(p+8);
            r.address = be32(p+12); r.length = be32(p+16); key = be32(p+20);
            if (r.offset < minimum || !span(r.offset, section_size, size) || r.length > section_size)
                return MT_INVALID;
            encryption = key == 0xffffffffu ? 0 : key >> 24;
            if (encryption == 1) r.mode |= 9u | ((key & 3u) << 1);
            else if (encryption == 2) r.mode |= 0x49u;
            else if (encryption) return MT_INVALID;
        } else {
            uint8_t feature;
            p = data + table + i * 40;
            r.offset = offset; r.address = le32(p+16); r.length = le32(p+20); feature = p[24];
            if (!span(offset, r.length, table)) return MT_INVALID;
            r.skip = !!(feature & 0x40); r.override_address = !!(feature & 0x20);
            r.mode |= feature & 6u;
            if (feature & 1u) r.mode |= 9u;
            if (feature & 0x10u) r.mode |= 0x40u;
            offset += r.length;
        }
        if (!r.length || !span(r.offset, r.length, size) ||
            (!r.skip && !mt_dma_span32(r.address, r.length))) return MT_INVALID;
        for (j=0; j<i; ++j) {
            const mt_fw_region *old = &out->regions[j];
            if ((uint64_t)r.offset < (uint64_t)old->offset + old->length &&
                (uint64_t)old->offset < (uint64_t)r.offset + r.length) return MT_INVALID;
        }
        out->regions[i] = r;
        if (!r.skip && r.override_address) entry = r.address;
    }
    out->kind = kind; out->count = count; out->entry = entry;
    return MT_OK;
}

int mt_fw_preflight_bundle(const uint8_t *data, uint32_t size, mt_fw_preflight *out) {
    mt_fw_plan patch, ram;
    uint32_t ps, rs, i, regions = 0, bytes = 0;
    if (!data || !out || size < 16 || le32(data) != 0x3546574du || le32(data+4) != 1)
        return MT_INVALID;
    ps = le32(data+8); rs = le32(data+12);
    if (ps > MT_FW_MAX_SIZE || rs > MT_FW_MAX_SIZE || ps > size-16 || rs != size-16-ps)
        return MT_INVALID;
    if (mt_fw_validate(data+16,ps,MT_FW_PATCH,&patch) ||
        mt_fw_validate(data+16+ps,rs,MT_FW_RAM,&ram)) return MT_INVALID;
    for (i=0; i<patch.count; ++i) { ++regions; bytes += patch.regions[i].length; }
    for (i=0; i<ram.count; ++i) {
        if (ram.regions[i].skip) continue;
        ++regions; bytes += ram.regions[i].length;
    }
    out->size = sizeof(*out); out->version = 1;
    out->patch_regions = patch.count; out->ram_regions = ram.count;
    out->download_regions = regions; out->download_bytes = bytes;
    out->entry = ram.entry; out->reserved = 0;
    return MT_OK;
}

static int command(const mt_fw_transport *t, uint32_t cmd, const uint8_t *data,
                    uint32_t length, int wait, uint32_t *reply) {
    *reply = 0xffffffffu;
    return t->send(t->context, cmd, data, length, wait, reply);
}
static int download(const uint8_t *data, const mt_fw_plan *plan,
                     const mt_fw_transport *t, mt_fw_result *result) {
    uint32_t i, position, chunk, reply, cmd;
    uint8_t request[12];
    int error;
    for (i=0; i<plan->count; ++i) {
        const mt_fw_region *r = &plan->regions[i];
        if (r->skip) continue;
        put32(request, r->address); put32(request+4, r->length); put32(request+8, r->mode);
        cmd = (r->address == 0x200000 || r->address == 0x900000 || r->address == 0xe0002800u) ? 5u : 1u;
        error = command(t, cmd, request, 12, 1, &reply);
        if (error) return error;
        if (reply) return MT_FW_PROTOCOL;
        for (position=0; position<r->length; position+=chunk) {
            chunk = r->length-position;
            if (chunk > MT_FW_CHUNK) chunk = MT_FW_CHUNK;
            error = command(t, MT_FW_SCATTER, data+r->offset+position, chunk, 0, &reply);
            if (error) return error;
            result->bytes_sent += chunk;
        }
        error = t->drain(t->context);
        if (error) return error;
        ++result->regions_sent;
    }
    return MT_OK;
}

int mt_fw_boot(const uint8_t *patch, uint32_t patch_size, const uint8_t *ram,
               uint32_t ram_size, const mt_fw_transport *t, mt_fw_result *result) {
    mt_fw_plan pp, rp;
    uint32_t reply;
    uint8_t request[8];
    int error, release;
    if (!result) return MT_INVALID;
    result->patch_present=0; result->regions_sent=0; result->bytes_sent=0;
    result->start_acked=0; result->ready=0; result->primary_error=0; result->release_error=0;
    if (!t || !t->send || !t->drain || !t->wait_state) { error=MT_INVALID; goto done; }
    /* Preflight BOTH images before any hardware callback or firmware restart. */
    error = mt_fw_validate(patch, patch_size, MT_FW_PATCH, &pp);
    if (error) goto done;
    error = mt_fw_validate(ram, ram_size, MT_FW_RAM, &rp);
    if (error) goto done;
    put32(request, 1); put32(request+4, 0);
    error = command(t, 4, request, 4, 0, &reply);
    if (error) goto done;
    error = t->wait_state(t->context, 0);
    if (error) goto done;
    error = command(t, 0x10, request, 4, 1, &reply);
    if (error) goto done;
    if (reply == 1) result->patch_present = 1;
    else if (reply == 2) {
        error = download(patch, &pp, t, result);
        if (!error) {
            request[0] = 0;
            error = command(t, 7, request, 4, 1, &reply);
            if (!error && reply) error = MT_FW_PROTOCOL;
        }
        /* A successfully acquired semaphore is released on every exit path. */
        request[0] = 0;
        release = command(t, 0x10, request, 4, 1, &reply);
        if (!release && reply != 3) release = MT_FW_PROTOCOL;
        result->release_error = release;
        if (error) goto done;
        if (release) { error=release; goto done; }
    } else { error=MT_FW_PROTOCOL; goto done; }
    error = download(ram, &rp, t, result);
    if (error) goto done;
    put32(request, rp.entry ? 1u : 0u); put32(request+4, rp.entry);
    error = command(t, 2, request, 8, 1, &reply);
    if (error) goto done;
    if (reply) { error=MT_FW_PROTOCOL; goto done; }
    result->start_acked = 1;
    error = t->wait_state(t->context, 1);
    if (!error) result->ready = 1;
done:
    result->primary_error = error;
    return error;
}

int mt_mcu_response(uint32_t cmd, uint8_t sequence, const uint8_t *p,
                    uint32_t size, uint32_t *status) {
    uint32_t declared;
    if (!p || !status || !sequence || sequence > 15 || size < 40) return MT_INVALID;
    declared = (uint32_t)p[32] | (uint32_t)p[33] << 8;
    if (declared < 8 || declared > size - 32) return MT_INVALID;
    if (p[37] != sequence || (p[38] & 4u)) return MT_PENDING;
    if (cmd == 0x10 || cmd == 7) {
        if (declared < 9) return MT_INVALID;
        *status = p[40]; return MT_OK;
    }
    if (cmd & MT_MCU_UNI) {
        if (declared < 20) return MT_INVALID;
        if (p[36] != 1 || p[44] != (cmd & 0xffu)) return MT_PENDING;
        *status = le32(p+48); return MT_OK;
    }
    if (declared < 12) return MT_INVALID;
    if (p[36] != 1) return MT_PENDING;
    /* Linux's MT7925 parser treats matching legacy download replies as ACKs;
     * unlike patch/UNI replies it does not define a status payload here. */
    *status = 0;
    return MT_OK;
}
