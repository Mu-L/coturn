/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * https://opensource.org/license/bsd-3-clause
 *
 * Copyright (C) 2011, 2012, 2013 Citrix Systems
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "ns_turn_allocation.h"
#include "ns_turn_ioalib.h"
#include "ns_turn_msg_defs.h" // for STUN_VALID_CHANNEL
#include "ns_turn_utils.h"    // for TURN_LOG_FUNC, TURN_LOG_LEVEL_ERROR

#include <stdbool.h> // for bool, false, true
#include <stdlib.h>  // for NULL, size_t, free, realloc, calloc
#include <string.h>  // for memset, memcpy

/////////////// Permission forward declarations /////////////////

static void init_turn_permission_hashtable(turn_permission_hashtable *map);
static void free_turn_permission_hashtable(turn_permission_hashtable *map);
static turn_permission_info *get_from_turn_permission_hashtable(turn_permission_hashtable *map, const ioa_addr *addr);

/////////////// ALLOCATION //////////////////////////////////////

void init_allocation(void *owner, allocation *a, ur_map *tcp_connections) {
  if (a) {
    memset(a, 0, sizeof(allocation));
    a->owner = owner;
    a->tcp_connections = tcp_connections;
    init_turn_permission_hashtable(&(a->addr_to_perm));
  }
}

void clear_allocation(allocation *a, SOCKET_TYPE socket_type) {
  if (!a) {
    return;
  }

  if (a->is_valid) {
    turn_report_allocation_delete(a, socket_type);
  }

  if (a->tcs.elems) {
    for (size_t i = 0; i < a->tcs.sz; ++i) {
      tcp_connection *tc = a->tcs.elems[i];
      if (tc) {
        delete_tcp_connection(tc);
        a->tcs.elems[i] = NULL;
      }
    }
    free(a->tcs.elems);
    a->tcs.elems = NULL;
  }
  a->tcs.sz = 0;

  for (size_t i = 0; i < ALLOC_PROTOCOLS_NUMBER; ++i) {
    clear_ioa_socket_session_if(a->relay_sessions[i].s, a->owner);
    clear_relay_endpoint_session_data(&(a->relay_sessions[i]));
    IOA_EVENT_DEL(a->relay_sessions[i].lifetime_ev);
  }

  /* The order is important here: */
  free_turn_permission_hashtable(&(a->addr_to_perm));
  ch_map_clean(&(a->chns));

  a->is_valid = 0;
}

relay_endpoint_session *get_relay_session(allocation *a, int family) {
  if (a) {
    return &(a->relay_sessions[ALLOC_INDEX(family)]);
  }
  return NULL;
}

int get_relay_session_failure(allocation *a, int family) {
  if (a) {
    return a->relay_sessions_failure[ALLOC_INDEX(family)];
  }
  return 0;
}

void set_relay_session_failure(allocation *a, int family) {
  if (a) {
    a->relay_sessions_failure[ALLOC_INDEX(family)] = 1;
  }
}

ioa_socket_handle get_relay_socket(allocation *a, int family) {
  if (a) {
    return a->relay_sessions[ALLOC_INDEX(family)].s;
  }
  return NULL;
}

void set_allocation_family_invalid(allocation *a, int family) {
  if (!a) {
    return;
  }

  const size_t index = ALLOC_INDEX(family);
  if (a->relay_sessions[index].s) {
    if (a->tcs.elems) {
      for (size_t i = 0; i < a->tcs.sz; ++i) {
        tcp_connection *tc = a->tcs.elems[i];
        if (tc) {
          if (tc->peer_s && (get_ioa_socket_address_family(tc->peer_s) == family)) {
            delete_tcp_connection(tc);
            a->tcs.elems[i] = NULL;
          }
        }
      }
    }

    clear_ioa_socket_session_if(a->relay_sessions[index].s, a->owner);
    clear_relay_endpoint_session_data(&(a->relay_sessions[index]));
    IOA_EVENT_DEL(a->relay_sessions[index].lifetime_ev);
  }
}

void set_allocation_lifetime_ev(allocation *a, turn_time_t exp_time, ioa_timer_handle ev, int family) {
  if (a) {
    IOA_EVENT_DEL(a->relay_sessions[ALLOC_INDEX(family)].lifetime_ev);
    a->relay_sessions[ALLOC_INDEX(family)].expiration_time = exp_time;
    a->relay_sessions[ALLOC_INDEX(family)].lifetime_ev = ev;
  }
}

bool is_allocation_valid(const allocation *a) {
  if (a) {
    return a->is_valid;
  }
  return false;
}

void set_allocation_valid(allocation *a, bool value) {
  if (a) {
    a->is_valid = value;
  }
}

turn_permission_info *allocation_get_permission(allocation *a, const ioa_addr *addr) {
  if (a) {
    return get_from_turn_permission_hashtable(&(a->addr_to_perm), addr);
  }
  return NULL;
}

///////////////////////////// TURN_PERMISSION /////////////////////////////////

static bool delete_channel_info_from_allocation_map(ur_map_key_type key, ur_map_value_type value);

void turn_permission_clean(turn_permission_info *tinfo) {
  if (!tinfo || !tinfo->allocated) {
    return;
  }

  if (tinfo->verbose) {
    char s[257] = "\0";
    addr_to_string(&(tinfo->addr), (uint8_t *)s);
    TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "session %018llu: peer %s deleted\n", tinfo->session_id, s);
  }

  if (!(tinfo->lifetime_ev)) {
    TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "!!! %s: strange (1) permission to be cleaned\n", __FUNCTION__);
  }

  IOA_EVENT_DEL(tinfo->lifetime_ev);
  lm_map_foreach(&(tinfo->chns), (foreachcb_type)delete_channel_info_from_allocation_map);
  lm_map_clean(&(tinfo->chns));
  memset(tinfo, 0, sizeof(turn_permission_info));
}

static void init_turn_permission_hashtable(turn_permission_hashtable *map) {
  if (map) {
    memset(map, 0, sizeof(turn_permission_hashtable));
  }
}

static void free_turn_permission_hashtable(turn_permission_hashtable *map) {
  if (!map) {
    return;
  }

  for (size_t i = 0; i < TURN_PERMISSION_HASHTABLE_SIZE; ++i) {

    turn_permission_array *parray = &(map->table[i]);

    for (size_t j = 0; j < TURN_PERMISSION_ARRAY_SIZE; ++j) {
      turn_permission_slot *slot = &(parray->main_slots[j]);
      if (slot->info.allocated) {
        turn_permission_clean(&(slot->info));
      }
    }

    if (parray->extra_slots) {
      for (size_t j = 0; j < parray->extra_sz; ++j) {
        turn_permission_slot *slot = parray->extra_slots[j];
        if (slot) {
          if (slot->info.allocated) {
            turn_permission_clean(&(slot->info));
          }
          free(slot);
          parray->extra_slots[j] = NULL;
        }
      }
      free(parray->extra_slots);
      parray->extra_slots = NULL;
    }
    parray->extra_sz = 0;
  }
}

static turn_permission_info *get_from_turn_permission_hashtable(turn_permission_hashtable *map, const ioa_addr *addr) {
  if (!addr || !map) {
    return NULL;
  }

  uint32_t index = addr_hash_no_port(addr) & (TURN_PERMISSION_HASHTABLE_SIZE - 1);
  turn_permission_array *parray = &(map->table[index]);

  for (size_t i = 0; i < TURN_PERMISSION_ARRAY_SIZE; ++i) {
    turn_permission_slot *slot = &(parray->main_slots[i]);
    if (slot->info.allocated && addr_eq_no_port(&(slot->info.addr), addr)) {
      return &(slot->info);
    }
  }

  if (parray->extra_slots) {
    for (size_t i = 0; i < parray->extra_sz; ++i) {
      turn_permission_slot *slot = parray->extra_slots[i];
      if (slot->info.allocated && addr_eq_no_port(&(slot->info.addr), addr)) {
        return &(slot->info);
      }
    }
  }

  return NULL;
}

static void ch_info_clean(ch_info *c) {
  if (c) {
    if (c->kernel_channel) {
      DELETE_TURN_CHANNEL_KERNEL(c->kernel_channel);
      c->kernel_channel = 0;
    }
    IOA_EVENT_DEL(c->lifetime_ev);
    memset(c, 0, sizeof(ch_info));
  }
}

static bool delete_channel_info_from_allocation_map(ur_map_key_type key, ur_map_value_type value) {
  UNUSED_ARG(key);

  if (value) {
    ch_info *chn = (ch_info *)value;

    if (chn->chnum < 1) {
      TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "!!! %s: strange (0) channel to be cleaned: chnum<1\n", __FUNCTION__);
    }

    ch_info_clean(chn);
  }

  return false;
}

void turn_channel_delete(ch_info *chn) {
  if (!chn) {
    return;
  }

  int port = addr_get_port(&(chn->peer_addr));
  if (port < 1) {
    char s[129];
    addr_to_string(&(chn->peer_addr), (uint8_t *)s);
    TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "!!! %s: strange (1) channel to be cleaned: port is empty: %s\n", __FUNCTION__,
                  s);
  }

  turn_permission_info *tinfo = (turn_permission_info *)chn->owner;
  if (tinfo) {
    lm_map_del(&(tinfo->chns), (ur_map_key_type)port, NULL);
  } else {
    TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "!!! %s: strange (2) channel to be cleaned: permission is empty\n",
                  __FUNCTION__);
  }

  delete_channel_info_from_allocation_map((ur_map_key_type)port, (ur_map_value_type)chn);
}

ch_info *allocation_get_new_ch_info(allocation *a, uint16_t chnum, ioa_addr *peer_addr) {
  if (!a) {
    return NULL;
  }

  turn_permission_info *tinfo = get_from_turn_permission_hashtable(&(a->addr_to_perm), peer_addr);
  if (!tinfo) {
    tinfo = allocation_add_permission(a, peer_addr);
    if (!tinfo) {
      return NULL;
    }
  }

  ch_info *chn = ch_map_get(&(a->chns), chnum, 1);
  if (!chn) {
    return NULL;
  }

  chn->allocated = true;
  chn->chnum = chnum;
  chn->port = addr_get_port(peer_addr);
  addr_cpy(&(chn->peer_addr), peer_addr);
  chn->owner = tinfo;

  lm_map_put(&(tinfo->chns), (ur_map_key_type)addr_get_port(peer_addr), (ur_map_value_type)chn);

  return chn;
}

ch_info *allocation_get_ch_info(allocation *a, uint16_t chnum) { return ch_map_get(&(a->chns), chnum, 0); }

ch_info *allocation_get_ch_info_by_peer_addr(allocation *a, ioa_addr *peer_addr) {
  turn_permission_info *tinfo = get_from_turn_permission_hashtable(&(a->addr_to_perm), peer_addr);
  if (tinfo) {
    return get_turn_channel(tinfo, peer_addr);
  }
  return NULL;
}

uint16_t get_turn_channel_number(turn_permission_info *tinfo, ioa_addr *addr) {
  if (tinfo) {
    ur_map_value_type t = 0;
    if (lm_map_get(&(tinfo->chns), (ur_map_key_type)addr_get_port(addr), &t) && t) {
      ch_info *chn = (ch_info *)t;
      if (STUN_VALID_CHANNEL(chn->chnum)) {
        return chn->chnum;
      }
    }
  }

  return 0;
}

ch_info *get_turn_channel(turn_permission_info *tinfo, ioa_addr *addr) {
  if (tinfo) {
    ur_map_value_type t = 0;
    if (lm_map_get(&(tinfo->chns), (ur_map_key_type)addr_get_port(addr), &t) && t) {
      ch_info *chn = (ch_info *)t;
      if (STUN_VALID_CHANNEL(chn->chnum)) {
        return chn;
      }
    }
  }

  return NULL;
}

turn_permission_hashtable *allocation_get_turn_permission_hashtable(allocation *a) { return &(a->addr_to_perm); }

turn_permission_info *allocation_add_permission(allocation *a, const ioa_addr *addr) {
  if (!a || !addr) {
    return NULL;
  }

  turn_permission_hashtable *map = &(a->addr_to_perm);
  uint32_t hash = addr_hash_no_port(addr);
  size_t fds = (size_t)(hash & (TURN_PERMISSION_HASHTABLE_SIZE - 1));

  turn_permission_array *parray = &(map->table[fds]);

  turn_permission_slot *slot = NULL;

  for (size_t i = 0; i < TURN_PERMISSION_ARRAY_SIZE; ++i) {
    slot = &(parray->main_slots[i]);
    if (!(slot->info.allocated)) {
      break;
    } else {
      slot = NULL;
    }
  }

  if (!slot) {
    size_t old_sz = parray->extra_sz;
    turn_permission_slot **slots = parray->extra_slots;

    if (slots) {
      for (size_t i = 0; i < old_sz; ++i) {
        slot = slots[i];
        if (!(slot->info.allocated)) {
          break;
        } else {
          slot = NULL;
        }
      }
    }

    if (!slot) {
      size_t old_sz_mem = old_sz * sizeof(turn_permission_slot *);
      turn_permission_slot **new_slots =
          (turn_permission_slot **)realloc(parray->extra_slots, old_sz_mem + sizeof(turn_permission_slot *));
      if (!new_slots) {
        return NULL;
      }
      parray->extra_slots = new_slots;
      slots = parray->extra_slots;
      parray->extra_sz = old_sz + 1;
      slot = (turn_permission_slot *)malloc(sizeof(turn_permission_slot));
      if (!slot) {
        return NULL;
      }
      slots[old_sz] = slot;
    }
  }

  memset(slot, 0, sizeof(turn_permission_slot));
  slot->info.allocated = true;
  turn_permission_info *elem = &(slot->info);
  addr_cpy(&(elem->addr), addr);
  elem->owner = a;

  return elem;
}

ch_info *ch_map_get(ch_map *const map, const uint16_t chnum, const int new_chn) {
  if (!map) {
    return NULL;
  }

  const size_t index = (size_t)(chnum & (CH_MAP_HASH_SIZE - 1));
  ch_map_array *const a = &(map->table[index]);

  for (size_t i = 0; i < CH_MAP_ARRAY_SIZE; ++i) {
    ch_info *const chi = &(a->main_chns[i]);
    if (chi->allocated) {
      if (!new_chn && (chi->chnum == chnum)) {
        return chi;
      }
    } else if (new_chn) {
      return chi;
    }
  }

  const size_t old_sz = a->extra_sz;
  if (old_sz && a->extra_chns) {
    for (size_t i = 0; i < old_sz; ++i) {
      ch_info *const chi = a->extra_chns[i];
      if (chi) {
        if (chi->allocated) {
          if (!new_chn && (chi->chnum == chnum)) {
            return chi;
          }
        } else if (new_chn) {
          return chi;
        }
      }
    }
  }

  if (new_chn) {
    const size_t old_sz_mem = old_sz * sizeof(ch_info *);
    ch_info **const pTmp = (ch_info **)realloc(a->extra_chns, old_sz_mem + sizeof(ch_info *));
    if (!pTmp) {
      return NULL;
    }
    a->extra_chns = pTmp;
    a->extra_chns[old_sz] = (ch_info *)calloc(1, sizeof(ch_info));
    if (!a->extra_chns[old_sz]) {
      // if the realloc succeeds, but the calloc fails, we don't attempt to shrink the realloc back down
      // by not recording the change to the size, we allow the next call to this function to realloc the
      // block to presumably the same size it already is, which should be fine and not result in any leaks.
      return NULL;
    }

    a->extra_sz += 1;
    return a->extra_chns[old_sz];
  }

  return NULL;
}

void ch_map_clean(ch_map *map) {
  if (!map) {
    return;
  }

  for (size_t index = 0; index < CH_MAP_HASH_SIZE; ++index) {
    ch_map_array *a = &(map->table[index]);

    for (size_t i = 0; i < CH_MAP_ARRAY_SIZE; ++i) {
      ch_info *chi = &(a->main_chns[i]);
      if (chi->allocated) {
        ch_info_clean(chi);
      }
    }

    if (a->extra_chns) {
      for (size_t i = 0; i < a->extra_sz; ++i) {
        ch_info *chi = a->extra_chns[i];
        if (chi) {
          if (chi->allocated) {
            ch_info_clean(chi);
          }
          free(chi);
          a->extra_chns[i] = NULL;
        }
      }
      free(a->extra_chns);
      a->extra_chns = NULL;
    }
    a->extra_sz = 0;
  }
}

////////////////// TCP connections ///////////////////////////////

static void set_new_tc_id(uint8_t server_id, tcp_connection *tc) {
  const uint32_t sid = ((uint32_t)server_id) << 24;
  allocation *const a = (allocation *)(tc->owner);
  if (!a || !a->tcp_connections) {
    return;
  }

  uint32_t newid = 0;
  do {
    do {
      newid = ((uint32_t)turn_random()) & 0x00FFFFFF;
    } while (!newid);
    newid = newid | sid;
  } while (ur_map_get(a->tcp_connections, (ur_map_key_type)newid, NULL));
  tc->id = newid;
  ur_map_put(a->tcp_connections, (ur_map_key_type)newid, (ur_map_value_type)tc);
}

tcp_connection *create_tcp_connection(uint8_t server_id, allocation *a, stun_tid *tid, ioa_addr *peer_addr,
                                      int *err_code) {
  tcp_connection_list *const tcl = &(a->tcs);
  if (!tcl) {
    return NULL;
  }

  if (tcl->elems) {
    for (size_t i = 0; i < tcl->sz; ++i) {
      tcp_connection *otc = tcl->elems[i];
      if (otc) {
        if (addr_eq(&(otc->peer_addr), peer_addr)) {
          *err_code = 446;
          return NULL;
        }
      }
    }
  }

  tcp_connection *tc = (tcp_connection *)calloc(1, sizeof(tcp_connection));
  if (!tc) {
    return NULL;
  }
  addr_cpy(&(tc->peer_addr), peer_addr);
  if (tid) {
    memcpy(&(tc->tid), tid, sizeof(stun_tid));
  }
  tc->owner = a;

  bool found = false;
  if (a->tcs.elems) {
    for (size_t i = 0; i < tcl->sz; ++i) {
      tcp_connection *otc = tcl->elems[i];
      if (!otc) {
        tcl->elems[i] = tc;
        found = true;
        break;
      }
    }
  }

  if (!found) {
    size_t old_sz_mem = a->tcs.sz * sizeof(tcp_connection *);
    tcp_connection **new_elems = realloc(a->tcs.elems, old_sz_mem + sizeof(tcp_connection *));
    if (!new_elems) {
      return NULL;
    }
    a->tcs.elems = new_elems;
    a->tcs.elems[a->tcs.sz] = tc;
    a->tcs.sz += 1;
  }

  set_new_tc_id(server_id, tc);
  return tc;
}

void delete_tcp_connection(tcp_connection *tc) {
  if (!tc) {
    return;
  }

  if (tc->done) {
    TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "!!! %s: check on already closed tcp data connection: %p\n", __FUNCTION__, tc);
    return;
  }
  tc->done = true;

  clear_unsent_buffer(&(tc->ub_to_client));

  IOA_EVENT_DEL(tc->peer_conn_timeout);
  IOA_EVENT_DEL(tc->conn_bind_timeout);
  allocation *a = (allocation *)(tc->owner);
  if (a) {
    ur_map *map = a->tcp_connections;
    if (map) {
      ur_map_del(map, (ur_map_key_type)(tc->id), NULL);
    }
    tcp_connection_list *tcl = &(a->tcs);
    if (tcl->elems) {
      for (size_t i = 0; i < tcl->sz; ++i) {
        if (tcl->elems[i] == tc) {
          tcl->elems[i] = NULL;
          break;
        }
      }
    }
  }
  IOA_CLOSE_SOCKET(tc->client_s);
  IOA_CLOSE_SOCKET(tc->peer_s);
  free(tc);
}

tcp_connection *get_and_clean_tcp_connection_by_id(ur_map *map, tcp_connection_id id) {
  if (map) {
    ur_map_value_type t = 0;
    if (ur_map_get(map, (ur_map_key_type)id, &t) && t) {
      ur_map_del(map, (ur_map_key_type)id, NULL);
      return (tcp_connection *)t;
    }
  }
  return NULL;
}

tcp_connection *get_tcp_connection_by_id(ur_map *map, tcp_connection_id id) {
  if (map) {
    ur_map_value_type t = 0;
    if (ur_map_get(map, (ur_map_key_type)id, &t) && t) {
      return (tcp_connection *)t;
    }
  }
  return NULL;
}

tcp_connection *get_tcp_connection_by_peer(allocation *a, ioa_addr *peer_addr) {
  if (a && peer_addr) {
    tcp_connection_list *tcl = &(a->tcs);
    if (tcl->elems) {
      for (size_t i = 0; i < tcl->sz; ++i) {
        tcp_connection *tc = tcl->elems[i];
        if (tc) {
          if (addr_eq(&(tc->peer_addr), peer_addr)) {
            return tc;
          }
        }
      }
    }
  }
  return NULL;
}

bool can_accept_tcp_connection_from_peer(allocation *a, ioa_addr *peer_addr, int server_relay) {
  if (server_relay) {
    return true;
  }

  if (a && peer_addr) {
    return get_from_turn_permission_hashtable(&(a->addr_to_perm), peer_addr) != NULL;
  }

  return false;
}

//////////////// Unsent buffers //////////////////////

void clear_unsent_buffer(unsent_buffer *ub) {
  if (ub) {
    if (ub->bufs) {
      for (size_t sz = 0; sz < ub->sz; sz++) {
        ioa_network_buffer_handle nbh = ub->bufs[sz];
        if (nbh) {
          ioa_network_buffer_delete(NULL, nbh);
          ub->bufs[sz] = NULL;
        }
      }
      free(ub->bufs);
      ub->bufs = NULL;
    }
    ub->sz = 0;
  }
}

void add_unsent_buffer(unsent_buffer *ub, ioa_network_buffer_handle nbh) {
  if (!ub || (ub->sz >= MAX_UNSENT_BUFFER_SIZE)) {
    ioa_network_buffer_delete(NULL, nbh);
  } else {
    ub->bufs = (ioa_network_buffer_handle *)realloc(ub->bufs, sizeof(ioa_network_buffer_handle) * (ub->sz + 1));
    if (!ub->bufs) {
      TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Memory allocation failed in add_unsent_buffer\n");
      ioa_network_buffer_delete(NULL, nbh);
      return;
    }
    ub->bufs[ub->sz] = nbh;
    ub->sz += 1;
  }
}

ioa_network_buffer_handle top_unsent_buffer(unsent_buffer *ub) {
  ioa_network_buffer_handle ret = NULL;
  if (ub && ub->bufs && ub->sz) {
    for (size_t sz = 0; sz < ub->sz; ++sz) {
      if (ub->bufs[sz]) {
        ret = ub->bufs[sz];
        break;
      }
    }
  }
  return ret;
}

void pop_unsent_buffer(unsent_buffer *ub) {
  if (ub && ub->bufs && ub->sz) {
    for (size_t sz = 0; sz < ub->sz; ++sz) {
      if (ub->bufs[sz]) {
        ub->bufs[sz] = NULL;
        break;
      }
    }
  }
}

//////////////////////////////////////////////////////////////////
