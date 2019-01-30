#include "vhandle.h"
#include "vhandle_sync.h"

namespace felis {

void SpinnerSlot::WaitForData(volatile uintptr_t *addr, uint64_t sid, uint64_t ver,
                              void *handle)
{
  DTRACE_PROBE1(felis, version_read, handle);

  uintptr_t oldval = *addr;
  if (!IsPendingVal(oldval)) return;
  DTRACE_PROBE1(felis, blocking_version_read, handle);

  int core = go::Scheduler::CurrentThreadPoolId() - 1;
  uint64_t mask = 1ULL << core;
  ulong wait_cnt = 0;

  while (true) {
    uintptr_t val = oldval;
    uintptr_t newval = val & ~mask;
    bool notified = false;
    if ((oldval = __sync_val_compare_and_swap(addr, val, newval)) == val) {
      notified = Spin(sid, ver, wait_cnt);
      oldval = *addr;
    }
    if (!IsPendingVal(oldval))
      return;
    oldval = *addr;
  }
}

void SpinnerSlot::OfferData(volatile uintptr_t *addr, uintptr_t obj)
{
  auto oldval = *addr;
  auto newval = obj;

  // installing newval
  while (true) {
    uintptr_t val = __sync_val_compare_and_swap(addr, oldval, newval);
    if (val == oldval) break;
    oldval = val;
  }

  // need to notify according to the bitmaps, which is oldval
  uint64_t mask = (1ULL << 32) - 1;
  uint64_t bitmap = mask - (oldval & mask);
  Notify(bitmap);
}

bool SpinnerSlot::Spin(uint64_t sid, uint64_t ver, ulong &wait_cnt)
{
  int idx = go::Scheduler::CurrentThreadPoolId() - 1;
  auto sched = go::Scheduler::Current();
  auto &transport = util::Impl<PromiseRoutineTransportService>();
  auto routine = sched->current_routine();
  // routine->set_busy_poll(true);

  abort_if(idx < 0, "We should not running on thread pool 0!");

  while (!slots[idx].done) {
    asm("pause" : : :"memory");
    wait_cnt++;

    if ((wait_cnt & 0x7FFFFFF) == 0) {
      printf("Deadlock on core %d? %lu (using %p) waiting for %lu\n", idx, sid, routine, ver);
      printf("UrgencyCnt is %lu\n", transport.UrgencyCount());
      util::Impl<PromiseRoutineDispatchService>().PrintInfo();
    }

    if ((wait_cnt & 0x0FFF) == 0) {
      transport.FlushPromiseRoutine();
      ((BasePromise::ExecutionRoutine *) routine)->Preempt(
          transport.UrgencyCount() > 0);
    }
  }

  asm volatile("" : : :"memory");

  DTRACE_PROBE3(felis, wait_jiffies, wait_cnt, sid, ver);
  slots[idx].done = 0;
  return true;
}

void SpinnerSlot::Notify(uint64_t bitmap)
{
  while (bitmap) {
    int idx = __builtin_ctzll(bitmap);
    slots[idx].done = 1;
    bitmap &= ~(1 << idx);
  }
}

} // namespace felis