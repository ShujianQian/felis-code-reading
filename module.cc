#include <iostream>
#include <fstream>

#include "module.h"

#include "node_config.h"
#include "console.h"
#include "index.h"
#include "log.h"

#include "gopp/gopp.h"
#include "gopp/channels.h"


namespace felis {

class LoggingModule : public Module<CoreModule> {
 public:
  LoggingModule() {
    info = {
      .name = "logging",
      .description = "Logging",
    };
    required = true;
  }
  void Init() override {
    InitializeLogger();

    std::ofstream pid_fout("/tmp/dolly.pid");
    pid_fout << (unsigned long) getpid();
  }
};

static LoggingModule logging_module;

class AllocatorModule : public Module<CoreModule> {
 public:
  AllocatorModule() {
    info = {
      .name = "allocator",
      .description = "Memory Allocator"
    };
  }
  void Init() override {
    Module<CoreModule>::InitModule("config");

    auto &console = util::Instance<Console>();
    mem::InitThreadLocalRegions(NodeConfiguration::kNrThreads);
    for (int i = 0; i < NodeConfiguration::kNrThreads; i++) {
      auto &r = mem::GetThreadLocalRegion(i);
      r.ApplyFromConf(console.FindConfigSection("mem"));
      // logger->info("setting up regions {}", i);
      r.InitPools(i / mem::kNrCorePerNode);
    }

    VHandle::InitPools();
  }
};

static AllocatorModule allocator_module;

class CoroutineModule : public Module<CoreModule> {
 public:
  CoroutineModule() {
    info = {
      .name = "coroutine",
      .description = "Coroutine Thread Pool",
    };
    required = true;
  }
  ~CoroutineModule() {
    go::WaitThreadPool();
  }
  void Init() override {
    go::InitThreadPool(NodeConfiguration::kNrThreads);
  }
};

static CoroutineModule coroutine_module;

class NodeServerModule : public Module<CoreModule> {
 public:
  NodeServerModule() {
    info = {
      .name = "node-server",
      .description = "Server for a database node",
    };
  }
  void Init() final override {
    Module<CoreModule>::InitModule("config");
    util::Instance<NodeConfiguration>().RunAllServers();
  }
};

static NodeServerModule server_module;

}