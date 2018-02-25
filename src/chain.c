#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "hsk-chain.h"
#include "hsk-constants.h"
#include "hsk-error.h"
#include "hsk-header.h"
#include "hsk-map.h"
#include "bn.h"
#include "msg.h"
#include "utils.h"

/*
 * Templates
 */

static int32_t
hsk_chain_init_genesis(hsk_chain_t *chain);

/*
 * Helpers
 */

static int32_t
qsort_cmp(const void *a, const void *b) {
  int64_t x = *((int64_t *)a);
  int64_t y = *((int64_t *)b);

  if (x < y)
    return -1;

  if (x > y)
    return 1;

  return 0;
}

/*
 * Chain
 */

int32_t
hsk_chain_init(hsk_chain_t *chain) {
  if (!chain)
    return HSK_EBADARGS;

  chain->height = 0;
  chain->tip = NULL;
  chain->genesis = NULL;

  hsk_map_init_int_map(&chain->hashes, free);
  hsk_map_init_hash_map(&chain->heights, NULL);
  hsk_map_init_hash_map(&chain->orphans, free);
  hsk_map_init_hash_map(&chain->prevs, NULL);

  return hsk_chain_init_genesis(chain);
}

static int32_t
hsk_chain_init_genesis(hsk_chain_t *chain) {
  if (!chain)
    return HSK_EBADARGS;

  size_t size = hsk_hex_decode_size((char *)HSK_GENESIS);
  uint8_t raw[size];

  assert(hsk_hex_decode((char *)HSK_GENESIS, raw));

  hsk_header_t *tip = hsk_header_alloc();

  if (!tip)
    return HSK_ENOMEM;

  assert(hsk_header_decode(raw, size, tip));
  assert(hsk_header_calc_work(tip, NULL));

  if (!hsk_map_set(&chain->hashes, hsk_header_cache(tip), (void *)tip))
    return HSK_ENOMEM;

  if (!hsk_map_set(&chain->heights, &tip->height, (void *)tip))
    return HSK_ENOMEM;

  chain->height = tip->height;
  chain->tip = tip;
  chain->genesis = tip;

  return HSK_SUCCESS;
}

void
hsk_chain_uninit(hsk_chain_t *chain) {
  if (!chain)
    return;

  hsk_map_uninit(&chain->heights);
  hsk_map_uninit(&chain->hashes);
  hsk_map_uninit(&chain->prevs);
  hsk_map_uninit(&chain->orphans);

  chain->tip = NULL;
  chain->genesis = NULL;
}

hsk_chain_t *
hsk_chain_alloc(void) {
  hsk_chain_t *chain = malloc(sizeof(hsk_chain_t));

  if (!chain)
    return NULL;

  if (hsk_chain_init(chain) != HSK_SUCCESS) {
    free(chain);
    return NULL;
  }

  return chain;
}

void
hsk_chain_free(hsk_chain_t *chain) {
  if (!chain)
    return;

  hsk_chain_uninit(chain);
  free(chain);
}

static void
hsk_chain_log(hsk_chain_t *chain, const char *fmt, ...) {
  printf("chain: ");

  va_list args;
  va_start(args, fmt);
  vprintf(fmt, args);
  va_end(args);
}

void
hsk_chain_get_locator(hsk_chain_t *chain, hsk_getheaders_msg_t *msg) {
  assert(chain && msg);

  int32_t i = 0;
  hsk_header_t *tip = chain->tip;
  int64_t height = chain->height;
  int64_t step = 1;

  hsk_header_hash(tip, msg->hashes[i++]);

  while (height > 0) {
    height -= step;

    if (height < 0)
      height = 0;

    if (i > 10)
      step *= 2;

    if (i == 64 - 1)
      height = 0;

    hsk_header_t *hdr =
      (hsk_header_t *)hsk_map_get(&chain->heights, &height);

    assert(hdr);

    hsk_header_hash(hdr, msg->hashes[i++]);
  }

  msg->hash_count = i;
}

static int64_t
hsk_chain_get_mtp(hsk_chain_t *chain, hsk_header_t *prev) {
  assert(chain);

  if (!prev)
    return 0;

  int32_t timespan = 11;
  int64_t median[11];
  size_t size = 0;
  int32_t i;

  for (i = 0; i < timespan && prev; i++) {
    median[i] = (int64_t)prev->time;
    prev = (hsk_header_t *)hsk_map_get(&chain->hashes, prev->prev_block);
    size += 1;
  }

  qsort((void *)median, size, sizeof(int64_t), qsort_cmp);

  return median[size >> 1];
}

static uint32_t
hsk_chain_retarget(hsk_chain_t *chain, hsk_header_t *prev) {
  assert(chain);

  uint32_t bits = HSK_BITS;
  uint8_t *limit = (uint8_t *)HSK_LIMIT;
  int32_t window = HSK_TARGET_WINDOW;
  int32_t timespan = HSK_TARGET_TIMESPAN;
  int32_t min = HSK_MIN_ACTUAL;
  int32_t max = HSK_MAX_ACTUAL;

  if (!prev)
    return bits;

  bn_t target_bn;
  bignum_init(&target_bn);

  hsk_header_t *last = prev;
  hsk_header_t *first = last;
  int32_t i;

  for (i = 0; first && i < window; i++) {
    uint8_t diff[32];
    assert(hsk_pow_to_target(first->bits, diff));
    bn_t diff_bn;
    bignum_from_array(&diff_bn, diff, 32);
    bignum_add(&target_bn, &diff_bn, &target_bn);
    first = (hsk_header_t *)hsk_map_get(&chain->hashes, first->prev_block);
  }

  if (!first)
    return bits;

  bn_t window_bn;
  bignum_from_int(&window_bn, window);

  bignum_div(&target_bn, &window_bn, &target_bn);

  int64_t start = hsk_chain_get_mtp(chain, first);
  int64_t end = hsk_chain_get_mtp(chain, last);
  int64_t diff = end - start;
  int64_t actual = timespan + ((diff - timespan) / 4);

  assert(actual >= 0);

  if (actual < min)
    actual = min;

  if (actual > max)
    actual = max;

  bn_t actual_bn;
  bignum_from_int(&actual_bn, actual);

  bn_t timespan_bn;
  bignum_from_int(&timespan_bn, timespan);

  bignum_mul(&target_bn, &actual_bn, &target_bn);
  bignum_div(&target_bn, &timespan_bn, &target_bn);

  bn_t limit_bn;
  bignum_from_array(&limit_bn, limit, 32);

  if (bignum_cmp(&target_bn, &limit_bn) > 0)
    return bits;

  uint8_t target[32];
  bignum_to_array(&target_bn, target, 32);

  uint32_t cmpct;

  assert(hsk_pow_to_bits(target, &cmpct));

  return cmpct;
}

static uint32_t
hsk_chain_get_target(hsk_chain_t *chain, int64_t time, hsk_header_t *prev) {
  assert(chain);

  // Genesis
  if (!prev) {
    assert(time == chain->genesis->time);
    return HSK_BITS;
  }

  if (HSK_NO_RETARGETTING)
    return HSK_BITS;

  if (HSK_TARGET_RESET) {
    // Special behavior for testnet:
    if (time > (int64_t)prev->time + HSK_TARGET_SPACING * 2)
      return HSK_BITS;
   }

  return hsk_chain_retarget(chain, prev);
}

static hsk_header_t *
hsk_chain_find_fork(
  hsk_chain_t *chain,
  hsk_header_t *fork,
  hsk_header_t *longer
) {
  assert(chain && fork && longer);

  while (!hsk_header_equal(fork, longer)) {
    while (longer->height > fork->height) {
      longer = hsk_map_get(&chain->hashes, longer->prev_block);
      if (!longer)
        return NULL;
    }

    if (hsk_header_equal(fork, longer))
      return fork;

    fork = hsk_map_get(&chain->hashes, fork->prev_block);

    if (!fork)
      return NULL;
  }

  return fork;
}

static void
hsk_chain_reorganize(hsk_chain_t *chain, hsk_header_t *competitor) {
  assert(chain && competitor);

  hsk_header_t *tip = chain->tip;
  hsk_header_t *fork = hsk_chain_find_fork(chain, tip, competitor);

  assert(fork);

  // Blocks to disconnect.
  hsk_header_t *disconnect = NULL;
  hsk_header_t *entry = tip;
  hsk_header_t *tail = NULL;
  while (!hsk_header_equal(entry, fork)) {
    assert(!entry->next);

    if (!disconnect)
      disconnect = entry;

    if (tail)
      tail->next = entry;

    tail = entry;

    entry = hsk_map_get(&chain->hashes, entry->prev_block);
    assert(entry);
  }

  // Blocks to connect.
  entry = competitor;
  hsk_header_t *connect = NULL;
  while (!hsk_header_equal(entry, fork)) {
    assert(!entry->next);

    if (connect)
      entry->next = connect;

    connect = entry;

    entry = hsk_map_get(&chain->hashes, entry->prev_block);
    assert(entry);
  }

  // Disconnect blocks.
  hsk_header_t *c, *n;
  for (c = disconnect; c; c = n) {
    n = c->next;
    c->next = NULL;
    hsk_map_del(&chain->heights, &c->height);
  }

  // Connect blocks (backwards, save last).
  for (c = connect; c; c = n) {
    n = c->next;
    c->next = NULL;

    if (!n) // halt on last
      break;

    assert(hsk_map_set(&chain->heights, &c->height, (void *)c));
  }
}

int32_t
hsk_chain_add(hsk_chain_t *chain, hsk_header_t *h) {
  if (!chain || !h)
    return HSK_EBADARGS;

  int32_t rc = HSK_SUCCESS;
  hsk_header_t *hdr = hsk_header_clone(h);

  if (!hdr) {
    rc = HSK_ENOMEM;
    goto fail;
  }

  uint8_t *hash = hsk_header_cache(hdr);

  hsk_chain_log(chain, "adding block: %s\n", hsk_hex_encode32(hash));

  if (hdr->time > hsk_now() + 2 * 60 * 60) {
    hsk_chain_log(chain, "  rejected: time-too-new\n");
    rc = HSK_ETIMETOONEW;
    goto fail;
  }

  if (hsk_map_has(&chain->hashes, hash)) {
    hsk_chain_log(chain, "  rejected: duplicate\n");
    rc = HSK_EDUPLICATE;
    goto fail;
  }

  if (hsk_map_has(&chain->orphans, hash)) {
    hsk_chain_log(chain, "  rejected: duplicate-orphan\n");
    rc = HSK_EDUPLICATEORPHAN;
    goto fail;
  }

  rc = hsk_header_verify_pow(hdr);

  if (rc != HSK_SUCCESS) {
    hsk_chain_log(chain, "  rejected: cuckoo error %d\n", rc);
    goto fail;
  }

  hsk_header_t *prev =
    (hsk_header_t *)hsk_map_get(&chain->hashes, hdr->prev_block);

  if (!prev) {
    hsk_chain_log(chain, "  stored as orphan\n");

    if (!hsk_map_set(&chain->orphans, hash, (void *)hdr)) {
      rc = HSK_ENOMEM;
      goto fail;
    }

    if (!hsk_map_set(&chain->prevs, hdr->prev_block, (void *)hdr)) {
      hsk_map_del(&chain->orphans, hash);
      rc = HSK_ENOMEM;
      goto fail;
    }

    return HSK_SUCCESS;
  }

  int64_t mtp = hsk_chain_get_mtp(chain, prev);

  if ((int64_t)hdr->time <= mtp) {
    hsk_chain_log(chain, "  rejected: time-too-old\n");
    rc = HSK_ETIMETOOOLD;
    goto fail;
  }

  uint32_t bits = hsk_chain_get_target(chain, hdr->time, prev);

  if (hdr->bits != bits) {
    hsk_chain_log(chain, "  rejected: bad-diffbits\n");
    rc = HSK_EBADDIFFBITS;
    goto fail;
  }

  hdr->height = prev->height + 1;

  assert(hsk_header_calc_work(hdr, prev));

  if (memcmp(hdr->work, chain->tip->work, 32) <= 0) {
    if (!hsk_map_set(&chain->hashes, hash, (void *)hdr)) {
      rc = HSK_ENOMEM;
      goto fail;
    }
    hsk_chain_log(chain, "  stored on alternate chain\n");
  } else {
    if (memcmp(hdr->prev_block, hsk_header_cache(chain->tip), 32) != 0) {
      hsk_chain_log(chain, "  reorganizing...\n");
      hsk_chain_reorganize(chain, hdr);
    }

    if (!hsk_map_set(&chain->hashes, hash, (void *)hdr)) {
      rc = HSK_ENOMEM;
      goto fail;
    }

    if (!hsk_map_set(&chain->heights, &hdr->height, (void *)hdr)) {
      hsk_map_del(&chain->hashes, hash);
      rc = HSK_ENOMEM;
      goto fail;
    }

    chain->height = hdr->height;
    chain->tip = hdr;

    hsk_chain_log(chain, "  added to main chain\n");
    hsk_chain_log(chain, "  new height: %d\n", chain->height);
  }

  return rc;

fail:
  if (hdr)
    free(hdr);

  return rc;
}
