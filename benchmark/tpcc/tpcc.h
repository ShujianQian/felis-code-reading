// -*- c++ -*-

#ifndef TPCC_H
#define TPCC_H

#include "table_decl.h"

#include <map>
#include <array>
#include <string>
#include <vector>
#include <cassert>

#include "index.h"
#include "util.h"
#include "sqltypes.h"
#include "epoch.h"

namespace felis {

class BaseTxn;

}

namespace tpcc {

// table and schemas definition
struct Customer {
  using Key = sql::CustomerKey;
  using Value = sql::CustomerValue;
};

struct CustomerNameIdx {
  using Key = sql::CustomerNameIdxKey;
  using Value = sql::CustomerNameIdxValue;
};

struct District {
  using Key = sql::DistrictKey;
  using Value = sql::DistrictValue;
};

struct History {
  using Key = sql::HistoryKey;
  using Value = sql::HistoryValue;
};

struct Item {
  using Key = sql::ItemKey;
  using Value = sql::ItemValue;
};

struct NewOrder {
  using Key = sql::NewOrderKey;
  using Value = sql::NewOrderValue;
};

struct OOrder {
  using Key = sql::OOrderKey;
  using Value = sql::OOrderValue;
};

struct OOrderCIdIdx {
  using Key = sql::OOrderCIdIdxKey;
  using Value = sql::OOrderCIdIdxValue;
};

struct OrderLine {
  using Key = sql::OrderLineKey;
  using Value = sql::OrderLineValue;
};

struct Stock {
  using Key = sql::StockKey;
  using Value = sql::StockValue;
};

struct StockData {
  using Key = sql::StockDataKey;
  using Value = sql::StockDataValue;
};

struct Warehouse {
  using Key = sql::WarehouseKey;
  using Value = sql::WarehouseValue;
};

enum struct TableType : int {
  Customer, CustomerNameIdx, District, History, Item,
  NewOrder, OOrder, OOrderCIdIdx, OrderLine, Stock, StockData, Warehouse,
  NRTable
};

static const char *kTPCCTableNames[] = {
  "customer",
  "customer_name_idx",
  "district",
  "history",
  "item",
  "new_order",
  "oorder",
  "oorder_c_id_idx",
  "order_line",
  "stock",
  "stock_data",
  "warehouse",
};

// We create a full set of table per warehouse
class Util {
 public:
  static felis::Relation &relation(TableType table, unsigned int wid);
  static int partition(uint wid);
};

class ClientBase {
 protected:
  util::FastRandom r;

 protected:
  static constexpr double kWarehouseSpread = 0.0;
  static constexpr double kNewOrderRemoteItem = 0.01;
  static constexpr double kCreditCheckRemoteCustomer = 0.15;
  static constexpr double kPaymentRemoteCustomer = 0.15;
  static constexpr double kPaymentByName = 0.60;

  size_t nr_warehouses() const;
  uint PickWarehouse();
  uint PickDistrict();
  static int CheckBetweenInclusive(int v, int lower, int upper);

  int RandomNumber(int min, int max);
  int RandomNumberExcept(int min, int max, int exception) {
    int r;
    do {
      r = RandomNumber(min, max);
    } while (r == exception);
    return r;
  }

  int NonUniformRandom(int A, int C, int min, int max);

  int GetItemId();
  int GetCustomerId();

  int GetOrderLinesPerCustomer();

  size_t GetCustomerLastName(uint8_t *buf, int num);
  size_t GetCustomerLastName(char *buf, int num) {
    return GetCustomerLastName((uint8_t *) buf, num);
  }
  std::string GetCustomerLastName(int num) {
    std::string ret;
    // all tokens are at most 5 chars long
    ret.resize(5 * 3);
    ret.resize(GetCustomerLastName(&ret[0], num));
    return ret;
  }

  std::string GetNonUniformCustomerLastNameLoad() {
    return GetCustomerLastName(NonUniformRandom(255, 157, 0, 999));
  }

  size_t GetNonUniformCustomerLastNameRun(uint8_t *buf) {
    return GetCustomerLastName(buf, NonUniformRandom(255, 223, 0, 999));
  }
  size_t GetNonUniformCustomerLastNameRun(char *buf) {
    return GetNonUniformCustomerLastNameRun((uint8_t *) buf);
  }
  std::string GetNonUniformCustomerLastNameRun() {
    return GetCustomerLastName(NonUniformRandom(255, 223, 0, 999));
  }

  std::string RandomStr(uint len);
  std::string RandomNStr(uint len);

  uint GetCurrentTime();
 public:
  ClientBase(const util::FastRandom &r) : r(r) {}
};

class TableHandles {
  int table_handles[felis::RelationManager::kMaxNrRelations];

  TableHandles();
  template <typename T> friend T &util::Instance();
public:
  int table_handle(int idx) const {
    assert(idx < felis::RelationManager::kMaxNrRelations);
    return table_handles[idx];
  }

  void InitiateTable(TableType table);
};

// loaders for each table
namespace loaders {

enum LoaderType {
  Warehouse, Item, Stock, District, Customer, Order
};

template <enum LoaderType TLN>
class Loader : public go::Routine, public tpcc::Util, public tpcc::ClientBase {
  std::mutex *m;
  std::atomic_int *count_down;
  int cpu;
public:
  Loader(unsigned long seed, std::mutex *w, std::atomic_int *c, int pin)
      : ClientBase(util::FastRandom(seed)), m(w), count_down(c), cpu(pin) {}
  void DoLoad();
  virtual void Run() {
    DoLoad();
    if (count_down->fetch_sub(1) == 1)
      m->unlock();
    util::PinToCPU(cpu);
  }
};

}

class Client : public felis::EpochClient, public ClientBase {
  static constexpr unsigned long kClientSeed = 0xdeadbeef;
 public:

  Client() : ClientBase(kClientSeed) {}

  template <class T> T GenerateTransactionInput();

  // XXX: hack for delivery transaction
  int last_no_o_ids[10];

  felis::BaseTxn *RunCreateTxn() final override;
};

enum class TxnType : int {
  NewOrder,
  // Delivery,
  // CreditCheck,
  // Payment,

  AllTxn,
};

using TxnFactory = util::Factory<felis::BaseTxn, static_cast<int>(TxnType::AllTxn), Client *>;

}

#endif /* TPCC_H */
