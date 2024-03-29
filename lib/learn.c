/*
 * Copyright (c) 2011, 2012 Nicira Networks.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>

#include "learn.h"

#include "byte-order.h"
#include "dynamic-string.h"
#include "meta-flow.h"
#include "nx-match.h"
#include "ofp-errors.h"
#include "ofp-util.h"
#include "ofpbuf.h"
#include "openflow/openflow.h"
#include "unaligned.h"

static ovs_be16
get_be16(const void **pp)
{
    const ovs_be16 *p = *pp;
    ovs_be16 value = *p;
    *pp = p + 1;
    return value;
}

static ovs_be32
get_be32(const void **pp)
{
    const ovs_be32 *p = *pp;
    ovs_be32 value = get_unaligned_be32(p);
    *pp = p + 1;
    return value;
}

static uint64_t
get_bits(int n_bits, const void **p)
{
    int n_segs = DIV_ROUND_UP(n_bits, 16);
    uint64_t value;

    value = 0;
    while (n_segs-- > 0) {
        value = (value << 16) | ntohs(get_be16(p));
    }
    return value;
}

static void
get_subfield(int n_bits, const void **p, struct mf_subfield *sf)
{
    sf->field = mf_from_nxm_header(ntohl(get_be32(p)));
    sf->ofs = ntohs(get_be16(p));
    sf->n_bits = n_bits;
}

static unsigned int
learn_min_len(uint16_t header)
{
    int n_bits = header & NX_LEARN_N_BITS_MASK;
    int src_type = header & NX_LEARN_SRC_MASK;
    int dst_type = header & NX_LEARN_DST_MASK;
    unsigned int min_len;

    min_len = 0;
    if (src_type == NX_LEARN_SRC_FIELD) {
        min_len += sizeof(ovs_be32); /* src_field */
        min_len += sizeof(ovs_be16); /* src_ofs */
    } else {
        min_len += DIV_ROUND_UP(n_bits, 16);
    }
    if (dst_type == NX_LEARN_DST_MATCH ||
        dst_type == NX_LEARN_DST_LOAD) {
        min_len += sizeof(ovs_be32); /* dst_field */
        min_len += sizeof(ovs_be16); /* dst_ofs */
    }
    return min_len;
}

static enum ofperr
learn_check_header(uint16_t header, size_t len)
{
    int src_type = header & NX_LEARN_SRC_MASK;
    int dst_type = header & NX_LEARN_DST_MASK;

    /* Check for valid src and dst type combination. */
    if (dst_type == NX_LEARN_DST_MATCH ||
        dst_type == NX_LEARN_DST_LOAD ||
        (dst_type == NX_LEARN_DST_OUTPUT &&
         src_type == NX_LEARN_SRC_FIELD)) {
        /* OK. */
    } else {
        return OFPERR_OFPBAC_BAD_ARGUMENT;
    }

    /* Check that the arguments don't overrun the end of the action. */
    if (len < learn_min_len(header)) {
        return OFPERR_OFPBAC_BAD_LEN;
    }

    return 0;
}

/* Checks that 'learn' (which must be at least 'sizeof *learn' bytes long) is a
 * valid action on 'flow'. */
enum ofperr
learn_check(const struct nx_action_learn *learn, const struct flow *flow)
{
    struct cls_rule rule;
    const void *p, *end;

    cls_rule_init_catchall(&rule, 0);

    if (learn->flags & ~htons(OFPFF_SEND_FLOW_REM)
        || !is_all_zeros(learn->pad, sizeof learn->pad)
        || learn->table_id == 0xff) {
        return OFPERR_OFPBAC_BAD_ARGUMENT;
    }

    end = (char *) learn + ntohs(learn->len);
    for (p = learn + 1; p != end; ) {
        uint16_t header = ntohs(get_be16(&p));
        int n_bits = header & NX_LEARN_N_BITS_MASK;
        int src_type = header & NX_LEARN_SRC_MASK;
        int dst_type = header & NX_LEARN_DST_MASK;

        enum ofperr error;
        uint64_t value;

        if (!header) {
            break;
        }

        error = learn_check_header(header, (char *) end - (char *) p);
        if (error) {
            return error;
        }

        /* Check the source. */
        if (src_type == NX_LEARN_SRC_FIELD) {
            struct mf_subfield src;

            get_subfield(n_bits, &p, &src);
            error = mf_check_src(&src, flow);
            if (error) {
                return error;
            }
            value = 0;
        } else {
            value = get_bits(n_bits, &p);
        }

        /* Check the destination. */
        if (dst_type == NX_LEARN_DST_MATCH || dst_type == NX_LEARN_DST_LOAD) {
            struct mf_subfield dst;

            get_subfield(n_bits, &p, &dst);
            error = (dst_type == NX_LEARN_DST_LOAD
                     ? mf_check_dst(&dst, &rule.flow)
                     : mf_check_src(&dst, &rule.flow));
            if (error) {
                return error;
            }

            if (dst_type == NX_LEARN_DST_MATCH
                && src_type == NX_LEARN_SRC_IMMEDIATE) {
                mf_set_subfield(&dst, value, &rule);
            }
        }
    }
    if (!is_all_zeros(p, (char *) end - (char *) p)) {
        return OFPERR_OFPBAC_BAD_ARGUMENT;
    }

    return 0;
}

void
learn_execute(const struct nx_action_learn *learn, const struct flow *flow,
              struct ofputil_flow_mod *fm)
{
    const void *p, *end;
    struct ofpbuf actions;

    cls_rule_init_catchall(&fm->cr, ntohs(learn->priority));
    fm->cookie = learn->cookie;
    fm->table_id = learn->table_id;
    fm->command = OFPFC_MODIFY_STRICT;
    fm->idle_timeout = ntohs(learn->idle_timeout);
    fm->hard_timeout = ntohs(learn->hard_timeout);
    fm->buffer_id = UINT32_MAX;
    fm->out_port = OFPP_NONE;
    fm->flags = ntohs(learn->flags) & OFPFF_SEND_FLOW_REM;
    fm->actions = NULL;
    fm->n_actions = 0;

    ofpbuf_init(&actions, 64);

    for (p = learn + 1, end = (char *) learn + ntohs(learn->len); p != end; ) {
        uint16_t header = ntohs(get_be16(&p));
        int n_bits = header & NX_LEARN_N_BITS_MASK;
        int src_type = header & NX_LEARN_SRC_MASK;
        int dst_type = header & NX_LEARN_DST_MASK;
        uint64_t value;

        struct nx_action_reg_load *load;
        struct mf_subfield dst;

        if (!header) {
            break;
        }

        if (src_type == NX_LEARN_SRC_FIELD) {
            struct mf_subfield src;

            get_subfield(n_bits, &p, &src);
            value = mf_get_subfield(&src, flow);
        } else {
            value = get_bits(n_bits, &p);
        }

        switch (dst_type) {
        case NX_LEARN_DST_MATCH:
            get_subfield(n_bits, &p, &dst);
            mf_set_subfield(&dst, value, &fm->cr);
            break;

        case NX_LEARN_DST_LOAD:
            get_subfield(n_bits, &p, &dst);
            load = ofputil_put_NXAST_REG_LOAD(&actions);
            load->ofs_nbits = nxm_encode_ofs_nbits(dst.ofs, dst.n_bits);
            load->dst = htonl(dst.field->nxm_header);
            load->value = htonll(value);
            break;

        case NX_LEARN_DST_OUTPUT:
            ofputil_put_OFPAT_OUTPUT(&actions)->port = htons(value);
            break;
        }
    }

    fm->actions = ofpbuf_steal_data(&actions);
    fm->n_actions = actions.size / sizeof(struct ofp_action_header);
}

static void
put_be16(struct ofpbuf *b, ovs_be16 x)
{
    ofpbuf_put(b, &x, sizeof x);
}

static void
put_be32(struct ofpbuf *b, ovs_be32 x)
{
    ofpbuf_put(b, &x, sizeof x);
}

static void
put_u16(struct ofpbuf *b, uint16_t x)
{
    put_be16(b, htons(x));
}

static void
put_u32(struct ofpbuf *b, uint32_t x)
{
    put_be32(b, htonl(x));
}

struct learn_spec {
    int n_bits;

    int src_type;
    struct mf_subfield src;
    uint8_t src_imm[sizeof(union mf_value)];

    int dst_type;
    struct mf_subfield dst;
};

static void
learn_parse_spec(const char *orig, char *name, char *value,
                 struct learn_spec *spec)
{
    memset(spec, 0, sizeof *spec);
    if (mf_from_name(name)) {
        const struct mf_field *dst = mf_from_name(name);
        union mf_value imm;
        char *error;

        error = mf_parse_value(dst, value, &imm);
        if (error) {
            ovs_fatal(0, "%s", error);
        }

        spec->n_bits = dst->n_bits;
        spec->src_type = NX_LEARN_SRC_IMMEDIATE;
        memcpy(spec->src_imm, &imm, dst->n_bytes);
        spec->dst_type = NX_LEARN_DST_MATCH;
        spec->dst.field = dst;
        spec->dst.ofs = 0;
        spec->dst.n_bits = dst->n_bits;
    } else if (strchr(name, '[')) {
        /* Parse destination and check prerequisites. */
        if (mf_parse_subfield(&spec->dst, name)[0] != '\0') {
            ovs_fatal(0, "%s: syntax error after NXM field name `%s'",
                      orig, name);
        }

        /* Parse source and check prerequisites. */
        if (value[0] != '\0') {
            if (mf_parse_subfield(&spec->src, value)[0] != '\0') {
                ovs_fatal(0, "%s: syntax error after NXM field name `%s'",
                          orig, value);
            }
            if (spec->src.n_bits != spec->dst.n_bits) {
                ovs_fatal(0, "%s: bit widths of %s (%u) and %s (%u) differ",
                          orig, name, spec->src.n_bits, value,
                          spec->dst.n_bits);
            }
        } else {
            spec->src = spec->dst;
        }

        spec->n_bits = spec->src.n_bits;
        spec->src_type = NX_LEARN_SRC_FIELD;
        spec->dst_type = NX_LEARN_DST_MATCH;
    } else if (!strcmp(name, "load")) {
        if (value[strcspn(value, "[-")] == '-') {
            struct nx_action_reg_load load;
            int nbits, imm_bytes;
            uint64_t imm;
            int i;

            nxm_parse_reg_load(&load, value);
            nbits = nxm_decode_n_bits(load.ofs_nbits);
            imm_bytes = DIV_ROUND_UP(nbits, 8);
            imm = ntohll(load.value);

            spec->n_bits = nbits;
            spec->src_type = NX_LEARN_SRC_IMMEDIATE;
            for (i = 0; i < imm_bytes; i++) {
                spec->src_imm[i] = imm >> ((imm_bytes - i - 1) * 8);
            }
            spec->dst_type = NX_LEARN_DST_LOAD;
            nxm_decode(&spec->dst, load.dst, load.ofs_nbits);
        } else {
            struct nx_action_reg_move move;

            nxm_parse_reg_move(&move, value);

            spec->n_bits = ntohs(move.n_bits);
            spec->src_type = NX_LEARN_SRC_FIELD;
            nxm_decode_discrete(&spec->src,
                                move.src, move.src_ofs, move.n_bits);
            spec->dst_type = NX_LEARN_DST_LOAD;
            nxm_decode_discrete(&spec->dst,
                                move.dst, move.dst_ofs, move.n_bits);
        }
    } else if (!strcmp(name, "output")) {
        if (mf_parse_subfield(&spec->src, value)[0] != '\0') {
            ovs_fatal(0, "%s: syntax error after NXM field name `%s'",
                      orig, name);
        }

        spec->n_bits = spec->src.n_bits;
        spec->src_type = NX_LEARN_SRC_FIELD;
        spec->dst_type = NX_LEARN_DST_OUTPUT;
    } else {
        ovs_fatal(0, "%s: unknown keyword %s", orig, name);
    }
}

void
learn_parse(struct ofpbuf *b, char *arg, const struct flow *flow)
{
    char *orig = xstrdup(arg);
    char *name, *value;
    enum ofperr error;
    size_t learn_ofs;
    size_t len;

    struct nx_action_learn *learn;
    struct cls_rule rule;

    learn_ofs = b->size;
    learn = ofputil_put_NXAST_LEARN(b);
    learn->idle_timeout = htons(OFP_FLOW_PERMANENT);
    learn->hard_timeout = htons(OFP_FLOW_PERMANENT);
    learn->priority = htons(OFP_DEFAULT_PRIORITY);
    learn->cookie = htonll(0);
    learn->flags = htons(0);
    learn->table_id = 1;

    cls_rule_init_catchall(&rule, 0);
    while (ofputil_parse_key_value(&arg, &name, &value)) {
        learn = ofpbuf_at_assert(b, learn_ofs, sizeof *learn);
        if (!strcmp(name, "table")) {
            learn->table_id = atoi(value);
            if (learn->table_id == 255) {
                ovs_fatal(0, "%s: table id 255 not valid for `learn' action",
                          orig);
            }
        } else if (!strcmp(name, "priority")) {
            learn->priority = htons(atoi(value));
        } else if (!strcmp(name, "idle_timeout")) {
            learn->idle_timeout = htons(atoi(value));
        } else if (!strcmp(name, "hard_timeout")) {
            learn->hard_timeout = htons(atoi(value));
        } else if (!strcmp(name, "cookie")) {
            learn->cookie = htonll(strtoull(value, NULL, 0));
        } else {
            struct learn_spec spec;

            learn_parse_spec(orig, name, value, &spec);

            /* Check prerequisites. */
            if (spec.src_type == NX_LEARN_SRC_FIELD
                && !mf_are_prereqs_ok(spec.src.field, flow)) {
                ovs_fatal(0, "%s: cannot specify source field %s because "
                          "prerequisites are not satisfied",
                          orig, spec.src.field->name);
            }
            if ((spec.dst_type == NX_LEARN_DST_MATCH
                 || spec.dst_type == NX_LEARN_DST_LOAD)
                && !mf_are_prereqs_ok(spec.dst.field, &rule.flow)) {
                ovs_fatal(0, "%s: cannot specify destination field %s because "
                          "prerequisites are not satisfied",
                          orig, spec.dst.field->name);
            }

            /* Update 'rule' to allow for satisfying destination
             * prerequisites. */
            if (spec.src_type == NX_LEARN_SRC_IMMEDIATE
                && spec.dst_type == NX_LEARN_DST_MATCH
                && spec.dst.ofs == 0
                && spec.n_bits == spec.dst.field->n_bytes * 8) {
                union mf_value imm;

                memcpy(&imm, spec.src_imm, spec.dst.field->n_bytes);
                mf_set_value(spec.dst.field, &imm, &rule);
            }

            /* Output the flow_mod_spec. */
            put_u16(b, spec.n_bits | spec.src_type | spec.dst_type);
            if (spec.src_type == NX_LEARN_SRC_IMMEDIATE) {
                int n_bytes = DIV_ROUND_UP(spec.n_bits, 8);
                if (n_bytes % 2) {
                    ofpbuf_put_zeros(b, 1);
                }
                ofpbuf_put(b, spec.src_imm, n_bytes);
            } else {
                put_u32(b, spec.src.field->nxm_header);
                put_u16(b, spec.src.ofs);
            }
            if (spec.dst_type == NX_LEARN_DST_MATCH ||
                spec.dst_type == NX_LEARN_DST_LOAD) {
                put_u32(b, spec.dst.field->nxm_header);
                put_u16(b, spec.dst.ofs);
            } else {
                assert(spec.dst_type == NX_LEARN_DST_OUTPUT);
            }
        }
    }

    put_u16(b, 0);

    len = b->size - learn_ofs;
    if (len % 8) {
        ofpbuf_put_zeros(b, 8 - len % 8);
    }

    learn = ofpbuf_at_assert(b, learn_ofs, sizeof *learn);
    learn->len = htons(b->size - learn_ofs);

    /* In theory the above should have caught any errors, but... */
    error = learn_check(learn, flow);
    if (error) {
        ovs_fatal(0, "%s: %s", orig, ofperr_to_string(error));
    }
    free(orig);
}

void
learn_format(const struct nx_action_learn *learn, struct ds *s)
{
    struct cls_rule rule;
    const void *p, *end;

    cls_rule_init_catchall(&rule, 0);

    ds_put_format(s, "learn(table=%"PRIu8, learn->table_id);
    if (learn->idle_timeout != htons(OFP_FLOW_PERMANENT)) {
        ds_put_format(s, ",idle_timeout=%"PRIu16, ntohs(learn->idle_timeout));
    }
    if (learn->hard_timeout != htons(OFP_FLOW_PERMANENT)) {
        ds_put_format(s, ",hard_timeout=%"PRIu16, ntohs(learn->hard_timeout));
    }
    if (learn->priority != htons(OFP_DEFAULT_PRIORITY)) {
        ds_put_format(s, ",priority=%"PRIu16, ntohs(learn->priority));
    }
    if (learn->flags & htons(OFPFF_SEND_FLOW_REM)) {
        ds_put_cstr(s, ",OFPFF_SEND_FLOW_REM");
    }
    if (learn->flags & htons(~OFPFF_SEND_FLOW_REM)) {
        ds_put_format(s, ",***flags=%"PRIu16"***",
                      ntohs(learn->flags) & ~OFPFF_SEND_FLOW_REM);
    }
    if (learn->cookie != htonll(0)) {
        ds_put_format(s, ",cookie=0x%"PRIx64, ntohll(learn->cookie));
    }
    if (!is_all_zeros(learn->pad, sizeof learn->pad)) {
        ds_put_cstr(s, ",***nonzero pad***");
    }

    end = (char *) learn + ntohs(learn->len);
    for (p = learn + 1; p != end; ) {
        uint16_t header = ntohs(get_be16(&p));
        int n_bits = header & NX_LEARN_N_BITS_MASK;

        int src_type = header & NX_LEARN_SRC_MASK;
        struct mf_subfield src;
        const uint8_t *src_value;
        int src_value_bytes;

        int dst_type = header & NX_LEARN_DST_MASK;
        struct mf_subfield dst;

        enum ofperr error;
        int i;

        if (!header) {
            break;
        }

        error = learn_check_header(header, (char *) end - (char *) p);
        if (error == OFPERR_OFPBAC_BAD_ARGUMENT) {
            ds_put_format(s, ",***bad flow_mod_spec header %"PRIx16"***)",
                          header);
            return;
        } else if (error == OFPERR_OFPBAC_BAD_LEN) {
            ds_put_format(s, ",***flow_mod_spec at offset %td is %u bytes "
                          "long but only %td bytes are left***)",
                          (char *) p - (char *) (learn + 1) - 2,
                          learn_min_len(header) + 2,
                          (char *) end - (char *) p + 2);
            return;
        }
        assert(!error);

        /* Get the source. */
        if (src_type == NX_LEARN_SRC_FIELD) {
            get_subfield(n_bits, &p, &src);
            src_value_bytes = 0;
            src_value = NULL;
        } else {
            src.field = NULL;
            src.ofs = 0;
            src.n_bits = 0;
            src_value_bytes = 2 * DIV_ROUND_UP(n_bits, 16);
            src_value = p;
            p = (const void *) ((const uint8_t *) p + src_value_bytes);
        }

        /* Get the destination. */
        if (dst_type == NX_LEARN_DST_MATCH || dst_type == NX_LEARN_DST_LOAD) {
            get_subfield(n_bits, &p, &dst);
        } else {
            dst.field = NULL;
            dst.ofs = 0;
            dst.n_bits = 0;
        }

        ds_put_char(s, ',');

        switch (src_type | dst_type) {
        case NX_LEARN_SRC_IMMEDIATE | NX_LEARN_DST_MATCH:
            if (dst.field && dst.ofs == 0 && n_bits == dst.field->n_bits) {
                union mf_value value;
                uint8_t *bytes = (uint8_t *) &value;

                if (src_value_bytes > dst.field->n_bytes) {
                    /* The destination field is an odd number of bytes, which
                     * got rounded up to a multiple of 2 to be put into the
                     * learning action.  Skip over the leading byte, which
                     * should be zero anyway.  Otherwise the memcpy() below
                     * will overrun the start of 'value'. */
                    int diff = src_value_bytes - dst.field->n_bytes;
                    src_value += diff;
                    src_value_bytes -= diff;
                }

                memset(&value, 0, sizeof value);
                memcpy(&bytes[dst.field->n_bytes - src_value_bytes],
                       src_value, src_value_bytes);
                ds_put_format(s, "%s=", dst.field->name);
                mf_format(dst.field, &value, NULL, s);
            } else {
                mf_format_subfield(&dst, s);
                ds_put_cstr(s, "=0x");
                for (i = 0; i < src_value_bytes; i++) {
                    ds_put_format(s, "%02"PRIx8, src_value[i]);
                }
            }
            break;

        case NX_LEARN_SRC_FIELD | NX_LEARN_DST_MATCH:
            mf_format_subfield(&dst, s);
            if (src.field != dst.field || src.ofs != dst.ofs) {
                ds_put_char(s, '=');
                mf_format_subfield(&src, s);
            }
            break;

        case NX_LEARN_SRC_IMMEDIATE | NX_LEARN_DST_LOAD:
            ds_put_cstr(s, "load:0x");
            for (i = 0; i < src_value_bytes; i++) {
                ds_put_format(s, "%02"PRIx8, src_value[i]);
            }
            ds_put_cstr(s, "->");
            mf_format_subfield(&dst, s);
            break;

        case NX_LEARN_SRC_FIELD | NX_LEARN_DST_LOAD:
            ds_put_cstr(s, "load:");
            mf_format_subfield(&src, s);
            ds_put_cstr(s, "->");
            mf_format_subfield(&dst, s);
            break;

        case NX_LEARN_SRC_FIELD | NX_LEARN_DST_OUTPUT:
            ds_put_cstr(s, "output:");
            mf_format_subfield(&src, s);
            break;
        }
    }
    if (!is_all_zeros(p, (char *) end - (char *) p)) {
        ds_put_cstr(s, ",***nonzero trailer***");
    }
    ds_put_char(s, ')');
}
