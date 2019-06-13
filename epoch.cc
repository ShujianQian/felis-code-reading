#include <sys/mman.h>
#include <algorithm>
#include <map>
#include <fstream>

#include "epoch.h"
#include "txn.h"
#include "log.h"
#include "vhandle.h"
#include "console.h"
#include "mem.h"
#include "gc.h"
#include "opts.h"

#include "literals.h"

#include "json11/json11.hpp"

namespace felis {

EpochClient *EpochClient::g_workload_client = nullptr;

void EpochCallback::operator()(unsigned long cnt)
{
  auto p = phase;

  trace(TRACE_COMPLETION "callback cnt {} on core {}",
        cnt, go::Scheduler::CurrentThreadPoolId() - 1);

  if (cnt == 0) {
    perf.End();
    perf.Show(label);
    printf("\n");

    // TODO: We might Reset() the PromiseAllocationService, which would free the
    // current go::Routine. Is it necessary to run some function in the another
    // go::Routine?

    static void (EpochClient::*phase_mem_funcs[])() = {
      &EpochClient::OnInsertComplete,
      &EpochClient::OnInitializeComplete,
      &EpochClient::OnExecuteComplete,
    };

    abort_if(go::Scheduler::Current()->current_routine() == &client->control,
             "Cannot call control thread from itself");
    client->control.Reset(phase_mem_funcs[static_cast<int>(phase)]);
    go::Scheduler::Current()->WakeUp(&client->control);
  }
}

EpochClient::EpochClient()
    : control(this),
      callback(EpochCallback(this)),
      completion(0, callback),
      disable_load_balance(false),
      conf(util::Instance<NodeConfiguration>())
{
  callback.perf.End();

  auto cnt_len = conf.nr_nodes() * conf.nr_nodes() * NodeConfiguration::kPromiseMaxLevels;
  unsigned long *cnt_mem = nullptr;
  EpochWorkers *workers_mem = nullptr;

  for (int t = 0; t < NodeConfiguration::g_nr_threads; t++) {
    auto d = std::div(t + NodeConfiguration::g_core_shifting, mem::kNrCorePerNode);
    auto numa_node = d.quot;
    auto numa_offset = d.rem;
    if (numa_offset == 0) {
      cnt_mem = (unsigned long *) mem::MemMapAlloc(
          mem::Epoch,
          cnt_len * sizeof(unsigned long) * mem::kNrCorePerNode,
          numa_node);
      workers_mem = (EpochWorkers *) mem::MemMapAlloc(
          mem::Epoch,
          sizeof(EpochWorkers) * mem::kNrCorePerNode,
          numa_node);
    }
    per_core_cnts[t] = cnt_mem + cnt_len * numa_offset;
    workers[t] = new (workers_mem + numa_offset) EpochWorkers(t, this);
  }
}

EpochTxnSet::EpochTxnSet()
{
  auto nr_threads = NodeConfiguration::g_nr_threads;
  auto d = std::div((int) EpochClient::kTxnPerEpoch, nr_threads);
  for (auto t = 0; t < nr_threads; t++) {
    size_t nr = d.quot;
    if (t < d.rem) nr++;
    auto numa_node = (t + NodeConfiguration::g_core_shifting) / mem::kNrCorePerNode;
    auto p = mem::MemMapAlloc(mem::Txn, (nr + 1) * sizeof(BaseTxn *), numa_node);
    per_core_txns[t] = new (p) TxnSet(nr);
  }
}

EpochTxnSet::~EpochTxnSet()
{
  // TODO: free these pointers via munmap().
}

void EpochClient::GenerateBenchmarks()
{
  all_txns = new EpochTxnSet[kMaxEpoch - 1];
  for (auto i = 1; i < kMaxEpoch; i++) {
    for (uint64_t j = 1; j <= NumberOfTxns(); j++) {
      auto d = std::div((int)(j - 1), NodeConfiguration::g_nr_threads);
      auto t = d.rem, pos = d.quot;
      BaseTxn::g_cur_numa_node = t / mem::kNrCorePerNode;
      all_txns[i - 1].per_core_txns[t]->txns[pos] = CreateTxn(GenerateSerialId(i, j));
    }
  }
}

void EpochClient::Start()
{
  // Ready to start!
  control.Reset(&EpochClient::InitializeEpoch);

  logger->info("load percentage {}%", LoadPercentage());

  perf = PerfLog();
  go::GetSchedulerFromPool(0)->WakeUp(&control);
}

uint64_t EpochClient::GenerateSerialId(uint64_t epoch_nr, uint64_t sequence)
{
  return (epoch_nr << 32)
      | (sequence << 8)
      | (conf.node_id() & 0x00FF);
}

void AllocStateTxnWorker::Run()
{
  for (auto i = 0; i < client->cur_txns->per_core_txns[t]->nr; i++) {
    auto txn = client->cur_txns->per_core_txns[t]->txns[i];
    txn->PrepareState();
  }
}

void CallTxnsWorker::Run()
{
  auto nr_nodes = client->conf.nr_nodes();
  auto cnt = client->per_core_cnts[t];
  auto cnt_len = nr_nodes * nr_nodes * NodeConfiguration::kPromiseMaxLevels;
  std::fill(cnt, cnt + cnt_len, 0);
  // conf.IncrementUrgencyCount(t);

  for (auto i = 0; i < client->cur_txns->per_core_txns[t]->nr; i++) {
    auto txn = client->cur_txns->per_core_txns[t]->txns[i];
    txn->ResetRoot();
    std::invoke(mem_func, txn);
    client->conf.CollectBufferPlan(txn->root_promise(), cnt);
  }

  bool node_finished = client->conf.FlushBufferPlan(client->per_core_cnts[t]);

  if (client->callback.phase == EpochPhase::Execute) {
    VHandle::Quiescence();
    RowEntity::Quiescence();

    mem::GetDataRegion().Quiescence();
  } else if (client->callback.phase == EpochPhase::Initialize) {
    util::Instance<GC>().RunGC();
  }

  auto pq = client->cur_txns->per_core_txns[t];

  for (auto i = 0; i < pq->nr; i++) {
    auto txn = pq->txns[i];
    // Try to assign a default partition scheme if nothing has been
    // assigned. Because transactions are already round-robinned, there is no
    // imbalanced here.
    txn->root_promise()->AssignAffinity(t);
    txn->root_promise()->Complete(VarStr());
  }

  util::Impl<PromiseRoutineTransportService>().FinishPromiseFromQueue(nullptr);
  client->completion.Complete();

  if (node_finished)
    client->completion.Complete();
}

void EpochClient::CallTxns(uint64_t epoch_nr, TxnMemberFunc func, const char *label)
{
  auto nr_threads = NodeConfiguration::g_nr_threads;
  conf.ResetBufferPlan();
  conf.FlushBufferPlanCompletion(epoch_nr);
  callback.label = label;
  callback.perf.Clear();
  callback.perf.Start();

  completion.Increment(conf.nr_nodes() + nr_threads);
  for (auto t = 0; t < nr_threads; t++) {
    auto r = &workers[t]->call_worker;
    r->Reset();
    r->set_function(func);
    go::GetSchedulerFromPool(t + 1)->WakeUp(r);
  }
}

void EpochClient::InitializeEpoch()
{
  auto &mgr = util::Instance<EpochManager>();
  mgr.DoAdvance(this);
  auto epoch_nr = mgr.current_epoch_nr();

  util::Impl<PromiseAllocationService>().Reset();
  util::Impl<PromiseRoutineDispatchService>().Reset();

  auto nr_threads = NodeConfiguration::g_nr_threads;

  disable_load_balance = true;
  cur_txns = &all_txns[epoch_nr - 1];
  total_nr_txn = NumberOfTxns();

  for (auto t = 0; t < nr_threads; t++) {
    auto r = &workers[t]->alloc_state_worker;
    r->Reset();
    r->set_urgent(true);
    go::GetSchedulerFromPool(t + 1)->WakeUp(r);
  }

  callback.phase = EpochPhase::Insert;
  CallTxns(epoch_nr, &BaseTxn::PrepareInsert, "Insert");
}

void EpochClient::OnInsertComplete()
{
  callback.phase = EpochPhase::Initialize;
  CallTxns(
      util::Instance<EpochManager>().current_epoch_nr(),
      &BaseTxn::Prepare,
      "Initialization");
}

void EpochClient::OnInitializeComplete()
{
  callback.phase = EpochPhase::Execute;

  if (NodeConfiguration::g_data_migration && util::Instance<EpochManager>().current_epoch_nr() == 1) {
    logger->info("Starting data scanner thread");
    auto &peer = util::Instance<felis::NodeConfiguration>().config().row_shipper_peer;
    go::GetSchedulerFromPool(NodeConfiguration::g_nr_threads + 1)->WakeUp(
      new felis::RowScannerRoutine());
  }

  CallTxns(
      util::Instance<EpochManager>().current_epoch_nr(),
      &BaseTxn::RunAndAssignSchedulingKey,
      "Execution");
}

void EpochClient::OnExecuteComplete()
{
  if (util::Instance<EpochManager>().current_epoch_nr() + 1 < kMaxEpoch) {
    InitializeEpoch();
  } else {
    // End of the experiment.
    perf.Show("All epochs done in");
    auto thr = NumberOfTxns() * 1000 * (kMaxEpoch - 1) / perf.duration_ms();
    logger->info("Throughput {} txn/s", thr);
    mem::PrintMemStats();
    mem::GetDataRegion().PrintUsageEachClass();

    if (Options::kOutputDir) {
      json11::Json::object result {
        {"cpu", static_cast<int>(NodeConfiguration::g_nr_threads)},
        {"duration", static_cast<int>(perf.duration_ms())},
        {"throughput", static_cast<int>(thr)},
      };
      auto node_name = util::Instance<NodeConfiguration>().config().name;
      time_t tm;
      char now[80];
      time(&tm);
      strftime(now, 80, "-%F-%X", localtime(&tm));
      std::ofstream result_output(
          Options::kOutputDir.Get() + "/" + node_name + now + ".json");
      result_output << json11::Json(result).dump() << std::endl;
    }
    util::Instance<Console>().UpdateServerStatus(Console::ServerStatus::Exiting);
  }
}

size_t EpochExecutionDispatchService::g_max_item = 20_M;
const size_t EpochExecutionDispatchService::kHashTableSize = 100001;

EpochExecutionDispatchService::EpochExecutionDispatchService()
{
  auto max_item_percore = g_max_item / NodeConfiguration::g_nr_threads;
  Queue *qmem = nullptr;

  for (int i = 0; i < NodeConfiguration::g_nr_threads; i++) {
    auto &queue = queues[i];
    auto d = std::div(i + NodeConfiguration::g_core_shifting, mem::kNrCorePerNode);
    auto numa_node = d.quot;
    auto offset_in_node = d.rem;

    if (offset_in_node == 0) {
      qmem = (Queue *) mem::MemMapAlloc(
          mem::EpochQueuePool, sizeof(Queue) * mem::kNrCorePerNode, numa_node);
    }
    queue = qmem + offset_in_node;

    queue->zq.end = queue->zq.start = 0;
    queue->zq.q = (PromiseRoutineWithInput *)
                 mem::MemMapAlloc(
                     mem::EpochQueuePromise,
                     max_item_percore * sizeof(PromiseRoutineWithInput),
                     numa_node);
    queue->pq.len = 0;
    queue->pq.q = (PriorityQueueHeapEntry *)
                 mem::MemMapAlloc(
                     mem::EpochQueueItem,
                     max_item_percore * sizeof(PriorityQueueHeapEntry),
                     numa_node);
    queue->pq.ht = (PriorityQueueHashHeader *)
                  mem::MemMapAlloc(
                      mem::EpochQueueItem,
                      kHashTableSize * sizeof(PriorityQueueHashHeader),
                      numa_node);
    queue->pq.pending.q = (PromiseRoutineWithInput *)
                         mem::MemMapAlloc(
                             mem::EpochQueuePromise,
                             max_item_percore * sizeof(PromiseRoutineWithInput),
                             numa_node);
    queue->pq.pending.start = 0;
    queue->pq.pending.end = 0;

    for (size_t t = 0; t < kHashTableSize; t++) {
      queue->pq.ht[t].Initialize();
    }

    queue->pq.pool = mem::BasicPool(
        mem::EpochQueuePool,
        kPriorityQueuePoolElementSize,
        max_item_percore,
        numa_node);

    queue->pq.pool.Register();

    new (&queue->lock) util::SpinLock();
  }
  tot_bubbles = 0;
}

void EpochExecutionDispatchService::Reset()
{
  for (int i = 0; i < NodeConfiguration::g_nr_threads; i++) {
    auto &q = queues[i];
    q->zq.end = q->zq.start = 0;
    q->pq.len = 0;
  }
  tot_bubbles = 0;
}

static bool Greater(const EpochExecutionDispatchService::PriorityQueueHeapEntry &a,
                    const EpochExecutionDispatchService::PriorityQueueHeapEntry &b)
{
  return a.key > b.key;
}

void EpochExecutionDispatchService::Add(int core_id, PromiseRoutineWithInput *routines,
                                        size_t nr_routines)
{
  bool locked = false;
  bool should_preempt = false;
  auto &lock = queues[core_id]->lock;
  lock.Lock();

  auto &zq = queues[core_id]->zq;
  auto &pq = queues[core_id]->pq.pending;
  size_t i = 0;

  auto max_item_percore = g_max_item / NodeConfiguration::g_nr_threads;

again:
  size_t zdelta = 0,
           zend = zq.end.load(std::memory_order_acquire),
         zlimit = max_item_percore;

  size_t pdelta = 0,
           pend = pq.end.load(std::memory_order_acquire),
         plimit = max_item_percore
                  - (pend - pq.start.load(std::memory_order_acquire));

  for (; i < nr_routines; i++) {
    auto r = routines[i];
    auto key = std::get<0>(r)->sched_key;

    if (key == 0) {
      auto pos = zend + zdelta++;
      abort_if(pos >= zlimit,
               "Preallocation of DispatchService is too small. {} < {}", pos, zlimit);
      zq.q[pos] = r;
    } else {
      auto pos = pend + pdelta++;
      if (pdelta >= plimit) goto again;
      pq.q[pos % max_item_percore] = r;
    }
  }
  if (zdelta)
    zq.end.fetch_add(zdelta, std::memory_order_release);
  if (pdelta)
    pq.end.fetch_add(pdelta, std::memory_order_release);
  lock.Unlock();
  util::Impl<VHandleSyncService>().Notify(1 << core_id);
}

bool
EpochExecutionDispatchService::AddToPriorityQueue(
    PriorityQueue &q, PromiseRoutineWithInput &r,
    BasePromise::ExecutionRoutine *state)
{
  bool smaller = false;
  auto [rt, in] = r;
  auto node = (PriorityQueueValue *) q.pool.Alloc();
  node->promise_routine = r;
  node->state = state;
  auto key = rt->sched_key;

  auto &hl = q.ht[Hash(key) % kHashTableSize];
  auto *ent = hl.next;
  while (ent != &hl) {
    if (ent->object()->key == key)
      goto found;
    ent = ent->next;
  }
  ent = (PriorityQueueHashEntry *) q.pool.Alloc();
  ent->object()->key = key;
  ent->object()->values.Initialize();
  ent->InsertAfter(hl.prev);

  if (q.len > 0 && q.q[0].key > key) {
    smaller = true;
  }
  q.q[q.len++] = {key, ent->object()};
  std::push_heap(q.q, q.q + q.len, Greater);

found:
  node->InsertAfter(ent->object()->values.prev);
  return smaller;
}

void
EpochExecutionDispatchService::ProcessPending(PriorityQueue &q)
{
  size_t pstart = q.pending.start.load(std::memory_order_acquire),
           plen = q.pending.end.load(std::memory_order_acquire) - pstart;

  for (size_t i = 0; i < plen; i++) {
    auto pos = pstart + i;
    AddToPriorityQueue(q, q.pending.q[pos % (g_max_item / NodeConfiguration::g_nr_threads)]);
  }
  if (plen)
    q.pending.start.fetch_add(plen);
}

bool
EpochExecutionDispatchService::Peek(int core_id, DispatchPeekListener &should_pop)
{
  auto &zq = queues[core_id]->zq;
  auto &q = queues[core_id]->pq;
  auto &lock = queues[core_id]->lock;
  auto &state = queues[core_id]->state;
  if (zq.start < zq.end.load(std::memory_order_acquire)) {
    state.running.store(true, std::memory_order_release);
    auto r = zq.q[zq.start];
    if (should_pop(r, nullptr)) {
      zq.start++;
      state.current = r;
      return true;
    }
    return false;
  }

  ProcessPending(q);

  if (q.len > 0) {
    auto node = q.q[0].ent->values.next;

    auto promise_routine = node->object()->promise_routine;

    state.running.store(true, std::memory_order_relaxed);
    if (should_pop(promise_routine, node->object()->state)) {
      node->Remove();
      q.pool.Free(node);

      auto top = q.q[0];
      if (top.ent->values.empty()) {
        std::pop_heap(q.q, q.q + q.len, Greater);
        q.q[q.len - 1].ent = nullptr;
        q.len--;

        top.ent->Remove();
        q.pool.Free(top.ent);
      }

      state.current = promise_routine;
      return true;
    }
    return false;
  }

  state.running.store(false, std::memory_order_relaxed);

  // We do not need locks to protect completion counters. There can only be MT
  // access on Pop() and Add(), the counters are per-core anyway.
  auto &c = state.complete_counter;
  auto n = c.completed;
  auto comp = EpochClient::g_workload_client->completion_object();
  c.completed = 0;

  unsigned long nr_bubbles = tot_bubbles.load();
  while (!tot_bubbles.compare_exchange_strong(nr_bubbles, 0));

  if (n + nr_bubbles > 0) {
    // logger->info("DispatchService on core {} notifies {} completions",
    //             core_id, n + nr_bubbles);
    comp->Complete(n + nr_bubbles);
  }
  return false;
}

void EpochExecutionDispatchService::AddBubble()
{
  tot_bubbles.fetch_add(1);
}

bool EpochExecutionDispatchService::Preempt(int core_id, bool force, BasePromise::ExecutionRoutine *routine_state)
{
  auto &lock = queues[core_id]->lock;
  bool new_routine = true;
  auto &zq = queues[core_id]->zq;
  auto &q = queues[core_id]->pq;
  auto &state = queues[core_id]->state;

  ProcessPending(q);

  lock.Lock();

  auto &r = state.current;
  auto key = std::get<0>(r)->sched_key;

  if (!force && zq.end.load(std::memory_order_relaxed) == zq.start) {
    if (q.len == 0 || key < q.q[0].key) {
      new_routine = false;
      goto done;
    }
  }

  if (key == 0) {
    zq.q[zq.end.load(std::memory_order_relaxed)] = r;
    zq.end.fetch_add(1, std::memory_order_release);
  } else  {
    AddToPriorityQueue(q, r, routine_state);
  }
  state.running.store(false, std::memory_order_relaxed);

done:
  lock.Unlock();
  return new_routine;
}

void EpochExecutionDispatchService::Complete(int core_id)
{
  auto &c = queues[core_id]->state.complete_counter;
  c.completed++;
}

void EpochExecutionDispatchService::PrintInfo()
{
  puts("===================================");
  for (int core_id = 0; core_id < NodeConfiguration::g_nr_threads; core_id++) {
    auto &q = queues[core_id]->pq.q;
    printf("DEBUG: %lu and %lu,%lu on core %d\n",
           q[0].key, q[1].key, q[2].key, core_id);
  }
  puts("===================================");
}

static constexpr size_t kEpochPromiseAllocationWorkerLimit = 512_M;
static constexpr size_t kEpochPromiseAllocationMainLimit = 64_M;
static constexpr size_t kEpochPromiseMiniBrkSize = 4 * CACHE_LINE_SIZE;

EpochPromiseAllocationService::EpochPromiseAllocationService()
{
  size_t acc = 0;
  for (size_t i = 0; i <= NodeConfiguration::g_nr_threads; i++) {
    auto s = kEpochPromiseAllocationWorkerLimit / NodeConfiguration::g_nr_threads;
    int numa_node = -1;
    if (i == 0) {
      s = kEpochPromiseAllocationMainLimit;
    } else {
      numa_node = (i - 1 + NodeConfiguration::g_core_shifting) / mem::kNrCorePerNode;
    }
    brks[i] = mem::Brk::New(mem::MemMapAlloc(mem::Promise, s, numa_node), s);
    acc += s;
    constexpr auto mini_brk_size = 4 * CACHE_LINE_SIZE;
    minibrks[i] = mem::Brk::New(
        brks[i]->Alloc(kEpochPromiseMiniBrkSize),
        kEpochPromiseMiniBrkSize);
  }
  // logger->info("Memory allocated: PromiseAllocator {}GB", acc >> 30);
}

void *EpochPromiseAllocationService::Alloc(size_t size)
{
  int thread_id = go::Scheduler::CurrentThreadPoolId();
  if (size < CACHE_LINE_SIZE) {
    auto b = minibrks[thread_id];
    if (!b->Check(size)) {
      b = mem::Brk::New(
          brks[thread_id]->Alloc(kEpochPromiseMiniBrkSize),
          kEpochPromiseMiniBrkSize);
    }
    return b->Alloc(size);
  } else {
    return brks[thread_id]->Alloc(util::Align(size, CACHE_LINE_SIZE));
  }
}

void EpochPromiseAllocationService::Reset()
{
  for (size_t i = 0; i <= NodeConfiguration::g_nr_threads; i++) {
    // logger->info("  PromiseAllocator {} used {}MB. Resetting now.", i,
    // brks[i].current_size() >> 20);
    brks[i]->Reset();
    minibrks[i] = mem::Brk::New(
        brks[i]->Alloc(kEpochPromiseMiniBrkSize),
        kEpochPromiseMiniBrkSize);
  }
}

static constexpr size_t kEpochMemoryLimitPerCore = 16_M;

EpochMemory::EpochMemory()
{
  logger->info("Allocating EpochMemory");
  auto &conf = util::Instance<NodeConfiguration>();
  for (int i = 0; i < conf.nr_nodes(); i++) {
    node_mem[i].mmap_buf =
        (uint8_t *) mem::MemMap(
            mem::Epoch, nullptr, kEpochMemoryLimitPerCore * conf.g_nr_threads,
            PROT_READ | PROT_WRITE,
            MAP_HUGETLB | MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);
    for (int t = 0; t < conf.g_nr_threads; t++) {
      auto p = node_mem[i].mmap_buf + t * kEpochMemoryLimitPerCore;
      auto numa_node = (t + conf.g_core_shifting) / mem::kNrCorePerNode;
      unsigned long nodemask = 1 << numa_node;
      abort_if(syscall(
          __NR_mbind,
          p, kEpochMemoryLimitPerCore,
          2 /* MPOL_BIND */,
          &nodemask,
          sizeof(unsigned long) * 8,
          1 << 0 /* MPOL_MF_STRICT */) < 0, "mbind failed!");
    }
    abort_if(mlock(node_mem[i].mmap_buf, kEpochMemoryLimitPerCore * conf.g_nr_threads) < 0,
             "Cannot allocate memory. mlock() failed.");
  }
  Reset();
}

EpochMemory::~EpochMemory()
{
  logger->info("Freeing EpochMemory");
  auto &conf = util::Instance<NodeConfiguration>();
  for (int i = 0; i < conf.nr_nodes(); i++) {
    munmap(node_mem[i].mmap_buf, kEpochMemoryLimitPerCore * conf.g_nr_threads);
  }
}

void EpochMemory::Reset()
{
  auto &conf = util::Instance<NodeConfiguration>();
  // I only manage the current node.
  auto node_id = conf.node_id();
  auto &m = node_mem[node_id - 1];
  for (int t = 0; t < conf.g_nr_threads; t++) {
    auto p = m.mmap_buf + t * kEpochMemoryLimitPerCore;
    m.brks[t] = mem::Brk::New(p, kEpochMemoryLimitPerCore);
  }
}

Epoch *EpochManager::epoch(uint64_t epoch_nr) const
{
  abort_if(epoch_nr != cur_epoch_nr, "Confused by epoch_nr {} since current epoch is {}",
           epoch_nr, cur_epoch_nr)
      return cur_epoch;
}

uint8_t *EpochManager::ptr(uint64_t epoch_nr, int node_id, uint64_t offset) const
{
  return epoch(epoch_nr)->mem->node_mem[node_id - 1].mmap_buf + offset;
}

static Epoch *g_epoch; // We don't support concurrent epochs for now.

void EpochManager::DoAdvance(EpochClient *client)
{
  cur_epoch_nr++;
  cur_epoch->~Epoch();
  cur_epoch = new (cur_epoch) Epoch(cur_epoch_nr, client, mem);
  logger->info("We are going into epoch {}", cur_epoch_nr);
}

EpochManager::EpochManager(EpochMemory *mem, Epoch *epoch)
    : cur_epoch_nr(0), cur_epoch(epoch), mem(mem)
{
  cur_epoch->mem = mem;
}

}

namespace util {

using namespace felis;

EpochManager *InstanceInit<EpochManager>::instance = nullptr;

InstanceInit<EpochManager>::InstanceInit()
{
  // We currently do not support concurrent epochs.
  static Epoch g_epoch;
  static EpochMemory mem;
  instance = new EpochManager(&mem, &g_epoch);
}

}
