/*
 * Copyright (c) 2010, 2011, 2012 Nicira Networks.
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

#ifndef NX_MATCH_H
#define NX_MATCH_H 1

#include <stdint.h>
#include <sys/types.h>
#include <netinet/in.h>
#include "openvswitch/types.h"
#include "ofp-errors.h"

struct cls_rule;
struct ds;
struct flow;
struct mf_subfield;
struct ofpbuf;
struct nx_action_reg_load;
struct nx_action_reg_move;

/* Nicira Extended Match (NXM) flexible flow match helper functions.
 *
 * See include/openflow/nicira-ext.h for NXM specification.
 */

enum ofperr nx_pull_match(struct ofpbuf *, unsigned int match_len,
                          uint16_t priority, struct cls_rule *,
                          ovs_be64 *cookie, ovs_be64 *cookie_mask);
enum ofperr nx_pull_match_loose(struct ofpbuf *, unsigned int match_len,
                                uint16_t priority, struct cls_rule *,
                                ovs_be64 *cookie, ovs_be64 *cookie_mask);
int nx_put_match(struct ofpbuf *, const struct cls_rule *,
                 ovs_be64 cookie, ovs_be64 cookie_mask);

char *nx_match_to_string(const uint8_t *, unsigned int match_len);
int nx_match_from_string(const char *, struct ofpbuf *);

void nxm_parse_reg_move(struct nx_action_reg_move *, const char *);
void nxm_parse_reg_load(struct nx_action_reg_load *, const char *);

void nxm_format_reg_move(const struct nx_action_reg_move *, struct ds *);
void nxm_format_reg_load(const struct nx_action_reg_load *, struct ds *);

enum ofperr nxm_check_reg_move(const struct nx_action_reg_move *,
                               const struct flow *);
enum ofperr nxm_check_reg_load(const struct nx_action_reg_load *,
                               const struct flow *);

void nxm_execute_reg_move(const struct nx_action_reg_move *, struct flow *);
void nxm_execute_reg_load(const struct nx_action_reg_load *, struct flow *);

int nxm_field_bytes(uint32_t header);
int nxm_field_bits(uint32_t header);

/* Dealing with the 'ofs_nbits' members in several Nicira extensions. */

static inline ovs_be16
nxm_encode_ofs_nbits(int ofs, int n_bits)
{
    return htons((ofs << 6) | (n_bits - 1));
}

static inline int
nxm_decode_ofs(ovs_be16 ofs_nbits)
{
    return ntohs(ofs_nbits) >> 6;
}

static inline int
nxm_decode_n_bits(ovs_be16 ofs_nbits)
{
    return (ntohs(ofs_nbits) & 0x3f) + 1;
}

void nxm_decode(struct mf_subfield *, ovs_be32 header, ovs_be16 ofs_nbits);
void nxm_decode_discrete(struct mf_subfield *, ovs_be32 header,
                         ovs_be16 ofs, ovs_be16 n_bits);

/* Upper bound on the length of an nx_match.  The longest nx_match (an
 * IPV6 neighbor discovery message using 5 registers) would be:
 *
 *                   header  value  mask  total
 *                   ------  -----  ----  -----
 *  NXM_OF_IN_PORT      4       2    --      6
 *  NXM_OF_ETH_DST_W    4       6     6     16
 *  NXM_OF_ETH_SRC      4       6    --     10
 *  NXM_OF_ETH_TYPE     4       2    --      6
 *  NXM_OF_VLAN_TCI     4       2     2      8
 *  NXM_OF_IP_TOS       4       1    --      5
 *  NXM_NX_IP_ECN       4       1    --      5
 *  NXM_OF_IP_TTL       4       1    --      5
 *  NXM_NX_IP_FRAG      4       1     1      8
 *  NXM_OF_IP_PROTO     4       2    --      6
 *  NXM_OF_IPV6_SRC_W   4      16    16     36
 *  NXM_OF_IPV6_DST_W   4      16    16     36
 *  NXM_OF_IPV6_LABEL   4       4    --      8
 *  NXM_OF_ICMP_TYPE    4       1    --      5
 *  NXM_OF_ICMP_CODE    4       1    --      5
 *  NXM_NX_ND_TARGET    4      16    --     20
 *  NXM_NX_ND_SLL       4       6    --     10
 *  NXM_NX_REG_W(0)     4       4     4     12
 *  NXM_NX_REG_W(1)     4       4     4     12
 *  NXM_NX_REG_W(2)     4       4     4     12
 *  NXM_NX_REG_W(3)     4       4     4     12
 *  NXM_NX_REG_W(4)     4       4     4     12
 *  NXM_NX_TUN_ID_W     4       8     8     20
 *  -------------------------------------------
 *  total                                  275
 *
 * So this value is conservative.
 */
#define NXM_MAX_LEN 384

/* This is my guess at the length of a "typical" nx_match, for use in
 * predicting space requirements. */
#define NXM_TYPICAL_LEN 64

#endif /* nx-match.h */
