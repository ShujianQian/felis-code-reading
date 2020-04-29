#ifndef HASHTABLE_INDEX_IMPL
#define HASHTABLE_INDEX_IMPL

#include <cstdlib>
#include <immintrin.h>

#include "index_common.h"

namespace felis {

typedef uint32_t (*HashFunc)(const VarStr *);

struct HashEntry {
  using Key = std::array<uint8_t, 16>;
  Key key;
  std::atomic<HashEntry *> next;

  // TODO: we have not implemented delete yet
  uint64_t rcu_epoch;

  static Key Convert(const VarStr *k) {
    Key x;
    x.fill(0);
    std::copy(k->data, k->data + k->len, x.begin());
    return x;
  }

  bool Compare(const Key &x) {
    return __builtin_memcmp(key.data(), x.data(), 16) == 0;
  }

  VHandle *value() const;
};

static_assert(sizeof(HashEntry) == 32);

class HashtableIndex final : public Table {
  HashFunc hash;
  size_t nr_buckets;
  std::atomic<HashEntry *> *table;
 public:
  HashtableIndex(std::tuple<HashFunc, size_t> conf);

  VHandle *SearchOrCreate(const VarStr *k, bool *created) override;
  VHandle *SearchOrCreate(const VarStr *k) override;
  VHandle *Search(const VarStr *k) override;
};

uint32_t DefaultHash(const VarStr *);

}

#endif
