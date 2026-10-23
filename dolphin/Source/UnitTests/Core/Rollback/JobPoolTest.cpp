// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The rollback job pool (Core/Rollback/job.h), started and stopped the way RollbackManager::Init
// and Shutdown do it, many times in one process (GitHub issue #3: each stop left its workers in
// task[], so the tenth start wrote past task[MAX_TASKS - 1]).

#include <atomic>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "Core/Rollback/job.h"

namespace
{
constexpr int NUM_HELPERS = 6;

struct Pool
{
  job::JobSysCtx ctx;
  job::JobTaskThread* dispatch = nullptr;
  std::vector<std::thread> threads;

  void Start()
  {
    ctx.activate();
    dispatch = ctx.initialize_worker(0, nullptr);
    for (int i = 1; i <= NUM_HELPERS; ++i)
    {
      job::JobTaskThread* thr =
          ctx.initialize_worker(static_cast<int64_t>(i) * 0x9e3779b97f4a7c15LL, nullptr);
      threads.emplace_back([thr] { thr->wait_for_termination(); });
    }
  }

  void Stop()
  {
    ctx.deactivate();
    for (auto& t : threads)
      t.join();
    threads.clear();
    ctx.release_workers();
    dispatch = nullptr;
  }

  // A root job with more children than one allocation block holds, as a snapshot save kicks.
  int RunJobs(int children)
  {
    std::atomic<int> ran{0};
    job::Job* root = job::KickRootJob(dispatch, [&](job::JobTaskThread& w, job::Job& r) {
      std::vector<job::Job*> jar;
      for (int i = 0; i < children; ++i)
        jar.push_back(w.create_job_as_child(r, [&](job::JobTaskThread&, job::Job&) { ++ran; }));
      w.do_work_and_kick_jobs(jar.data(), static_cast<uint16_t>(jar.size()));
    });
    job::DrainJobsUntilComplete(dispatch, root);
    return ran.load();
  }
};
}  // namespace

TEST(JobPool, RestartReusesTheSameSlots)
{
  Pool pool;
  for (int cycle = 0; cycle < 3 * job::MAX_TASKS / (NUM_HELPERS + 1); ++cycle)
  {
    pool.Start();
    ASSERT_EQ(pool.ctx.task_count.load(), static_cast<uint32_t>(NUM_HELPERS + 1));
    for (int k = 0; k <= NUM_HELPERS; ++k)
      EXPECT_EQ(pool.ctx.task[k]->worker_id, k);
    EXPECT_EQ(pool.RunJobs(3000), 3000);
    pool.Stop();
    EXPECT_EQ(pool.ctx.task_count.load(), 0u);
    EXPECT_EQ(pool.ctx.wait_count.load(), 0u);
    for (int k = 0; k <= NUM_HELPERS; ++k)
      EXPECT_EQ(pool.ctx.task[k], nullptr);
  }
}

TEST(JobPool, ReleaseUnusedKeepsABlockWithJobsOut)
{
  void* m = JOB_ALIGNED_ALLOC(64, sizeof(job::JobAllocBlock));
  job::JobAllocBlock* block = new (m) job::JobAllocBlock();
  block->new_job();
  block->new_job();
  block->new_job();
  block->deref();
  block->deref();
  // The owner lets go with one job still out: the block stays for that job's deref.
  block->release_unused();
  EXPECT_EQ(block->ref_count.load(), 1u);
  block->deref();  // frees it
}
