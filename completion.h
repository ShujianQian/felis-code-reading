#ifndef _COMPLETION_H
#define _COMPLETION_H

#include <atomic>
#include "log.h"

namespace felis {

template <typename T>
class CompletionObject {
  std::atomic_long comp_count;
  T callback;
 public:
  CompletionObject(unsigned long count, T callback)
      : comp_count(count), callback(callback) {
    logger->info("Completion object at {}", (void *) this);
  }

  void Complete(long dec = 1) {
    // if(dec > 1){
    //   logger->info("Completion decremented {}", dec);
    // }
    callback.PreComplete();
    auto cnt = comp_count.fetch_sub(dec) - dec;
    if (cnt < 0) {
      fprintf(stderr, "Completion handler isn't enough %ld!\n", cnt);
      std::abort();
    }
    callback(cnt);
  }

  void operator()() {
    Complete();
  }

  void Increment(unsigned long inc) {
    // if(inc > 1){
    //   logger->info("Completion incremented {}", inc);
    // }
    comp_count.fetch_add(inc);
  }

  unsigned long left_over() const {
    return comp_count.load();
  }
};


}

#endif
