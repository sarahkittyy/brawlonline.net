#pragma once
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <thread>

/* this algo is derived from: https://github.com/cdwfs/cds_job */
/* sourced from: https://github.com/injinj/WSQ */

/* ---- MSVC / portability shims ------------------------------------------- */
#ifdef _MSC_VER
#include <intrin.h>
#include <malloc.h>
#define JOB_ALIGNED_ALLOC(align, size) _aligned_malloc((size), (align))
#define JOB_ALIGNED_FREE(ptr) _aligned_free(ptr)
#else
/* C11 aligned_alloc wants size to be a multiple of the alignment (macOS enforces it). */
#define JOB_ALIGNED_ALLOC(align, size)                                                           \
  ::aligned_alloc((align), (((size) + (align) - 1) / (align)) * (align))
#define JOB_ALIGNED_FREE(ptr) ::free(ptr)
#endif

namespace job
{
/* size of the queue for each task (64k limit) */
static const uint32_t MAX_QUEUE_JOBS = 64 * 1024;
/* no more than this number of task threads */
static const uint16_t MAX_TASKS = 64,
                      /* mask to avoid mod operator */
    MASK_JOBS = MAX_QUEUE_JOBS - 1,
                      /* when queue is full, allow space for queue contention */
    FULL_QUEUE_JOBS = MAX_QUEUE_JOBS - MAX_TASKS;

static void pause_thread(void)
{
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
  _mm_pause();
#elif defined(_MSC_VER) && defined(_M_ARM64)
  __yield();
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
  asm volatile("pause" ::: "memory");
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__aarch64__) || defined(__arm__))
  asm volatile("yield" ::: "memory");
#else
  std::this_thread::yield();
#endif
}

/* a random state given to each task for stealing jobs from other
 * threads randomly (xoroshiro128* algo) */
struct XoroRand
{
  uint64_t state[2];
  XoroRand() {}

  void init(uint64_t s0, uint64_t s1)
  { /* init state */
    this->state[0] = s0;
    this->state[1] = s1;
  }

  static inline uint64_t rotl(const uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

  void incr(void)
  {
    const uint64_t s0 = this->state[0];
    const uint64_t s1 = this->state[1] ^ s0;
    this->state[0] = rotl(s0, 55) ^ s1 ^ (s1 << 14); /* a, b */
    this->state[1] = rotl(s1, 36);                   /* c */
  }

  uint64_t next(void)
  {
    const uint64_t result = this->state[0] + this->state[1];
    this->incr();
    return result;
  }
};

struct Job;
struct JobTaskThread;
typedef std::function<void(JobTaskThread&, Job&)> JobFunction;

/* the work stealing queue */
/* the owner of the queue pushes at the bottom and consumes there as well
 * the stealers consume from the top
 *
 *  +--------+ <- entries[ 0 ]
 *  |  top   | <- stealers consume here: job = entries[ top++ ]
 *  |        |
 *  |   ||   |
 *  |        |
 *  |   vv   |
 *  | bottom | <- owner pushes here:    entries[ bottom++ ] = job
 *  |        |    owner consumes here:  job = entries[ --bottom ]
 *  |        |
 *  +--------+ <- entries[ MASK_JOBS ]
 */
struct WSQIndex
{
  uint16_t top, /* the first job pushed */
      bottom,   /* the last job pushed */
      count,    /* count of elems available */
      ocount;   /* old value of count (useful for debugging) */
  WSQIndex() {}
  WSQIndex(uint16_t t, uint16_t b, uint16_t c, uint16_t u)
  {
    this->top = t;
    this->bottom = b;
    this->count = c;
    this->ocount = u;
  }
  WSQIndex(uint64_t v)
  {
    this->top = (uint16_t)(v >> 48);
    this->bottom = (uint16_t)(v >> 32);
    this->count = (uint16_t)(v >> 16);
    this->ocount = (uint16_t)(v >> 0);
  }
  uint64_t u64(void) const
  {
    return ((uint64_t)this->top << 48) | ((uint64_t)this->bottom << 32) |
           ((uint64_t)this->count << 16) | ((uint64_t)this->ocount << 0);
  }
};

struct JobAllocBlock;
struct Job
{
  JobTaskThread& thr;                    /* the initiator thread */
  JobFunction function;                  /* function called to complete job */
  Job* parent;                           /* if a child job */
  void* data;                            /* closure data */
  JobAllocBlock& alloc_block;            /* allocation location for job release */
  std::atomic<uint32_t> unfinished_jobs; /* if children are not yet finished */
  uint16_t execute_worker_id;            /* which thread executed job */
  bool is_done,                          /* set after finished */
      is_waiting;                        /* if a thread is waiting for this job */

  void* operator new(size_t, void* ptr) { return ptr; }
  void operator delete(void*, void*) {} /* placement delete — no-op */
  void operator delete(void*) {}        /* is allocated in alloc_block */

  Job(JobTaskThread& t,  /* the thread where job is queued */
      JobFunction f,     /* the function to execute */
      void* d,           /* closure data for the function */
      Job* p = nullptr); /* the parent, if job is a child */
  /* put the job into run queue, block until queue has space */
  void kick(void);
  /* queue for execute(), if queue is not full */
  bool try_kick(void);
  /* subtract one from ref count, if parent not null */
  void finish(void);
};

/* work stealing queue:  an array of jobs and index of top and bottom, with
 * a counter that tracks how many empty slots are available -- this is
 * different than the count of slots available because stealing threads
 * may not yet have taken a job out of the entries[] array, but they have
 * incremented the counters */
struct WSQ
{
  std::atomic<Job*> entries[MAX_QUEUE_JOBS]; /* queue of jobs */
  std::atomic<uint64_t> idx;                 /* the WSQIndex packed in 64 bits */
  uint8_t pad[64 - 8];                       /* keep idx separate from push_avail */
  const uint16_t worker_id;                  /* owner of queue */
  uint16_t push_avail;                       /* number of push slots available */

  WSQ(uint16_t id) : worker_id(id), push_avail(FULL_QUEUE_JOBS)
  {
    WSQIndex i = {0, 0, 0, 0};
    this->idx.store(i.u64(), std::memory_order_relaxed);
    ::memset((void*)this->entries, 0, sizeof(this->entries));
  }
  /* try_push() can only be called by the thread which owns this queue */
  bool try_push(Job& job)
  {
    uint64_t v = this->idx.load(std::memory_order_relaxed);
    WSQIndex i(v);
    /* if no space left, return false */
    if (i.count == FULL_QUEUE_JOBS)
      return false;
    WSQIndex j = {i.top, (uint16_t)((i.bottom + 1) & MASK_JOBS), (uint16_t)(i.count + 1), i.count};
    /* try to acquire an entries[] index for job */
    if (std::atomic_compare_exchange_strong(&this->idx, &v, j.u64()))
    {
      if (this->push_avail == 0)
      {
        for (;;)
        {
          /* it's possible that a stealing thread incremented idx but did not
           * yet take the job out of the entries[] array */
          Job* old = this->entries[i.bottom].exchange(nullptr, std::memory_order_relaxed);
          /* if old is null, put job in queue */
          if (old == nullptr)
            break;
          /* put old back and pause while stealer is sleeping */
          this->entries[i.bottom].exchange(old, std::memory_order_relaxed);
          pause_thread();
        }
      }
      else
      { /* when push available, already know entries[ bottom ] is null */
        this->push_avail -= 1;
      }
      this->entries[i.bottom].store(&job, std::memory_order_relaxed);
      return true;
    }
    return false; /* failed to acquire idx location */
  }
  /* push multiple items, should only be called when push_avail >= n, the
   * push_avail counter tracks how many empty (null) slots are available */
  void multi_push(Job** jar, uint16_t n)
  {
    this->push_avail -= n;
    for (;;)
    {
      uint64_t v = this->idx.load(std::memory_order_relaxed);
      WSQIndex i(v);
      WSQIndex j = {i.top, (uint16_t)((i.bottom + n) & MASK_JOBS), (uint16_t)(i.count + n),
                    i.count};
      /* try to acquire an entries[] index for job */
      if (std::atomic_compare_exchange_strong(&this->idx, &v, j.u64()))
      {
        for (uint16_t k = 0; k < n; k++)
        {
          this->entries[(i.bottom + k) & MASK_JOBS].store(jar[k], std::memory_order_relaxed);
        }
        return;
      }
    }
  }
  /* pop() can only be called by the thread which owns this queue */
  Job* pop(void)
  {
    for (;;)
    {
      uint64_t v = this->idx.load(std::memory_order_relaxed);
      WSQIndex i(v);
      if (i.count == 0) /* if nothing in the queue */
        return nullptr;
      WSQIndex j = {i.top, (uint16_t)((i.bottom - 1) & MASK_JOBS), (uint16_t)(i.count - 1),
                    i.count};
      /* fetch idx location, it could be stolen first */
      if (std::atomic_compare_exchange_strong(&this->idx, &v, j.u64()))
      {
        Job* job = this->entries[j.bottom].exchange(nullptr, std::memory_order_relaxed);
        assert(job != nullptr); /* should not be empty, it's my queue */
        return job;
      }
    }
  }
  /* steal() must be called by threads which do not own this queue */
  uint16_t steal(uint16_t n, Job** jar)
  {
    uint64_t v = this->idx.load(std::memory_order_relaxed);
    WSQIndex i(v);
    if (i.count == 0) /* nothing available */
      return 0;
    /* if trying to steal multiple items, balance the queues */
    if (n > i.count / 2 + 1)
      n = i.count / 2 + 1;
    WSQIndex j = {(uint16_t)((i.top + n) & MASK_JOBS), i.bottom, (uint16_t)(i.count - n), i.count};
    /* try to fetch the next available index */
    if (!std::atomic_compare_exchange_strong(&this->idx, &v, j.u64()))
      return 0;
    for (uint16_t k = 0;;)
    {
      jar[k] = this->entries[(i.top + k) & MASK_JOBS].exchange(nullptr, std::memory_order_relaxed);
      if (jar[k] != nullptr)
      {
        if (++k == n)
          return k;
      }
      else
      { /* could be null if owner hasn't set the entry yet */
        pause_thread();
      }
    }
  }
  /* test if space available for multi-push */
  uint16_t multi_push_avail(uint16_t maxn)
  {
    if (maxn <= this->push_avail)
      return maxn;
    WSQIndex i = this->idx.load(std::memory_order_relaxed);
    uint16_t k, avail = FULL_QUEUE_JOBS - i.count;
    for (k = 0; k < avail; k++)
    {
      if (this->entries[(i.bottom + k) & MASK_JOBS].load(std::memory_order_relaxed) != nullptr)
        break;
    }
    this->push_avail = k;
    if (maxn > k)
      maxn = k;
    return maxn;
  }
};

struct JobSysCtx;
/* a job task thread owns a queue and a rand state */
/* the queue is used to push/pop jobs and the rand is used to steal jobs */
struct JobTaskThread
{
  WSQ queue;                /* the work stealing queue above */
  XoroRand rand;            /* rand state for choosing a task to steal from */
  JobSysCtx& ctx;           /* contains all of the threads */
  JobAllocBlock* cur_block; /* allocate jobs from this block */
  void* data;               /* application closure for thread */
  const uint16_t worker_id; /* the index of task[] in JobSysCtx for this thr */

  void* operator new(size_t, void* ptr) { return ptr; }
  void operator delete(void*, void*) {}
  void operator delete(void* ptr) { JOB_ALIGNED_FREE(ptr); }

  JobTaskThread(JobSysCtx& c, uint32_t id, uint64_t seed, void* dat)
      : queue(id), ctx(c), cur_block(0), data(dat), worker_id(id)
  {
    this->rand.init(id, seed);
  }
  /* allocate space from cur_block for job */
  void* alloc_job(void);
  /* kick job and do work until it is done */
  void kick_and_wait_for(Job& j);
  /* kick several jobs */
  void kick_jobs(Job** jar, uint16_t n);
  /* do work until there is space for n jobs */
  void do_work_and_kick_jobs(Job** jar, uint16_t n);
  /* do work until sys is running */
  void wait_for_termination(void); /* run jobs until is_sys_active false */
  /* run j */
  void execute(Job& j);
  /* create a job, does not execute until kick()ed */
  Job* create_job(JobFunction f, void* d = nullptr);
  /* create a job as child of j, so that the parent is notified when all
   * children have finished */
  Job* create_job_as_child(Job& j, JobFunction f, void* d = nullptr);
  /* check this thread's queue with pop, then randomly check other threads
   * queue and steal jobs from them */
  Job* get_valid_job(void);
};

struct JobAllocBlock
{
  /* align job on 64 byte cache line */
  static const size_t JOB_SIZE = ((sizeof(Job) + 63) / 64) * 64,
                      NUM_ALLOC_JOBS = (MAX_QUEUE_JOBS > 4096 ? (MAX_QUEUE_JOBS >> 6) : 64) - 1;
  uint8_t mem[JOB_SIZE * NUM_ALLOC_JOBS];
  uint32_t avail_count;            /* how many jobs are available */
  std::atomic<uint32_t> ref_count; /* how many jobs are used */

  void* operator new(size_t, void* ptr) { return ptr; }
  void operator delete(void*, void*) {}
  void operator delete(void* ptr) { JOB_ALIGNED_FREE(ptr); }

  JobAllocBlock() : avail_count(NUM_ALLOC_JOBS)
  {
    /* referenced by each job and by JobTaskThread */
    this->ref_count.store(NUM_ALLOC_JOBS + 1, std::memory_order_relaxed);
  }
  /* while available, return next slot */
  void* new_job(void)
  {
    if (this->avail_count > 0)
    {
      this->avail_count -= 1;
      return &this->mem[JOB_SIZE * this->avail_count];
    }
    return nullptr;
  }
  /* if all freed, delete the block */
  void deref(void)
  {
    uint32_t left = this->ref_count.fetch_sub(1, std::memory_order_relaxed);
    if (left == 1)
      delete this;
  }
};

/* the global state for the tasking system */
struct JobSysCtx
{
  JobTaskThread* task[MAX_TASKS];   /* all of the threads */
  std::atomic<uint32_t> wait_count; /* how many task[] are in waiting */
  std::atomic<uint32_t> task_count; /* how many task[] are used */
  std::atomic<bool> is_sys_active;  /* threads exit when false */

  JobTaskThread* initialize_worker(int64_t seed, void* data);

  /* bumped after every kick; idle workers block on it instead of spinning */
  std::atomic<uint32_t> work_epoch{0};

  /* workers run until is_sys_active is false */
  void activate(void) { this->is_sys_active.store(true, std::memory_order_relaxed); }
  void deactivate(void)
  {
    this->is_sys_active.store(false, std::memory_order_relaxed);
    notify_work();
  }
  void notify_work(void)
  {
    this->work_epoch.fetch_add(1, std::memory_order_release);
    this->work_epoch.notify_all();
  }
  JobSysCtx() : wait_count(0), task_count(0), is_sys_active(false) {}
};

/* construct a new thread worker, including a queue for jobs to run */
inline JobTaskThread* JobSysCtx::initialize_worker(int64_t seed, void* data)
{
  uint32_t count = this->task_count.load(std::memory_order_relaxed);
  /* align task queues and index on a 64 byte cache line */
  void* m = JOB_ALIGNED_ALLOC(64, sizeof(JobTaskThread));
  JobTaskThread* thr = new (m) JobTaskThread(*this, count, seed, data);
  this->task[count] = thr;
  this->task_count.store(count + 1, std::memory_order_relaxed);
  return thr;
}

inline void* JobTaskThread::alloc_job(void)
{
  void* m;
  if (this->cur_block == NULL || (m = this->cur_block->new_job()) == NULL)
  {
    if (this->cur_block != NULL)
      this->cur_block->deref();
    m = JOB_ALIGNED_ALLOC(64, sizeof(JobAllocBlock));
    this->cur_block = new (m) JobAllocBlock();
    m = this->cur_block->new_job();
  }
  return m;
}

/* create a job, does not queue it for running until job.kick() is called  */
inline Job* JobTaskThread::create_job(JobFunction f, void* d)
{
  void* m = this->alloc_job();
  return new (m) Job(*this, f, d);
}

/* create a child job as part of parent job, this allows threads to
 * wait until the parent is done */
inline Job* JobTaskThread::create_job_as_child(Job& j, JobFunction f, void* d)
{
  void* m = this->alloc_job();
  return new (m) Job(*this, f, d, &j);
}

/* find a job to run, look at task's queue
 * if no jobs there, then try to steal a job randomly from another task */
inline Job* JobTaskThread::get_valid_job(void)
{
  Job* j = this->queue.pop();
  if (j != nullptr)
    return j;
  Job* jar[64];
  uint16_t n = this->queue.multi_push_avail(63);
  uint32_t count = this->ctx.task_count.load(std::memory_order_relaxed),
           next = this->rand.next() % count;
  for (uint32_t k = 0; k < count; k++)
  {
    /* don't try to steal from myself */
    if (this->ctx.task[next] != this)
    {
      n = this->ctx.task[next]->queue.steal(n + 1, jar);
      if (n > 0)
      {
        if (n > 1)
          this->queue.multi_push(&jar[1], n - 1);
        return jar[0];
      }
    }
    if (++next == count)
      next = 0;
  }
  return nullptr;
}

/* task blocks/runs jobs until system is shutdown */
inline void JobTaskThread::wait_for_termination(void)
{
  /* Spin briefly for more work (a save or load kicks several jobs in a row), then block until the
   * next kick. Spinning forever kept every helper thread (6 per Dolphin) at 100% CPU even
   * between frames, starving the emulator's own CPU and GPU threads. */
  constexpr uint32_t IDLE_SPINS_BEFORE_SLEEP = 1024;
  bool is_waiting = false;
  uint32_t idle_spins = 0;
  while (this->ctx.is_sys_active.load(std::memory_order_relaxed))
  {
    Job* j = this->get_valid_job();
    if (j != nullptr)
    {
      if (is_waiting)
      {
        is_waiting = false;
        this->ctx.wait_count.fetch_sub(1, std::memory_order_relaxed);
      }
      idle_spins = 0;
      this->execute(*j);
    }
    else
    {
      if (!is_waiting)
      {
        is_waiting = true;
        this->ctx.wait_count.fetch_add(1, std::memory_order_relaxed);
      }
      if (++idle_spins < IDLE_SPINS_BEFORE_SLEEP)
      {
        pause_thread();
        continue;
      }
      /* Read the epoch before the last look at the queues: a kick after this point changes it,
       * so wait() returns at once instead of missing the job. */
      const uint32_t epoch = this->ctx.work_epoch.load(std::memory_order_acquire);
      j = this->get_valid_job();
      if (j != nullptr)
      {
        is_waiting = false;
        this->ctx.wait_count.fetch_sub(1, std::memory_order_relaxed);
        idle_spins = 0;
        this->execute(*j);
        continue;
      }
      if (!this->ctx.is_sys_active.load(std::memory_order_relaxed))
        break;
      this->ctx.work_epoch.wait(epoch, std::memory_order_acquire);
      idle_spins = 0;
    }
  }
}

/* task runs a job */
inline void JobTaskThread::execute(Job& j)
{
  if (j.is_done)
  {
    std::cout << "worker " << this->worker_id << " exec worker " << j.execute_worker_id << " owner "
              << j.thr.worker_id << " is done!!\n";
    assert(0);
  }
  else
  {
    j.execute_worker_id = this->worker_id;
    j.function(*this, j);
    // Jobs live in pooled blocks that are freed without running ~Job, so release the closure
    // (and everything it captured, e.g. an evicted snapshot delta) as soon as it has run.
    j.function = nullptr;
    j.finish();
  }
}

/* task blocks/runs jobs until j is finished */
inline void JobTaskThread::kick_and_wait_for(Job& j)
{
  j.is_waiting = true;
  j.kick();
  while (j.unfinished_jobs.load(std::memory_order_acquire) != 0)
  {
    Job* k = this->get_valid_job();
    if (k != nullptr)
      this->execute(*k);
    else
      pause_thread();
  }
}

/* start multiple jobs by adding them to the queue */
inline void JobTaskThread::kick_jobs(Job** jar, uint16_t n)
{
  uint16_t j;
  for (uint16_t i = 0; i < n; i += j)
  {
    j = this->queue.multi_push_avail(n - i);
    if (j == 0)
    {
      jar[i]->kick();
      j = 1;
    }
    else
    {
      this->queue.multi_push(&jar[i], j);
    }
  }
}

inline void JobTaskThread::do_work_and_kick_jobs(Job** jar, uint16_t n)
{
  uint16_t i = 0, avail = this->queue.push_avail, cnt;
  for (;;)
  {
    if (i == n)
      return;
    if (avail > 0)
    {
      cnt = n - i;
      if (cnt > avail)
        cnt = avail;
      this->queue.multi_push(&jar[i], cnt);
      this->ctx.notify_work();
      i += cnt;
      if (i == n)
        return;
    }
    while ((avail = this->queue.multi_push_avail(n - i)) == 0)
    {
      for (cnt = 0; cnt < n - i; cnt++)
      {
        Job* j = this->queue.pop();
        if (j == nullptr)
          break;
        this->execute(*j);
      }
    }
  }
}

/* constructor for job */
inline Job::Job(JobTaskThread& t, JobFunction f, void* d, Job* p)
    : thr(t), function(f), parent(p), data(d), alloc_block(*t.cur_block), execute_worker_id(0),
      is_done(false), is_waiting(false)
{
  this->unfinished_jobs.store(1, std::memory_order_relaxed);
  if (p != nullptr)
    p->unfinished_jobs.fetch_add(1, std::memory_order_relaxed);
}

inline void Job::kick(void)
{
  for (;;)
  {
    if (this->try_kick())
    {
      this->thr.ctx.notify_work();
      return;
    }
    pause_thread();
  }
}

inline bool Job::try_kick(void)
{
  return this->thr.queue.try_push(*this);
}

inline void Job::finish(void)
{
  /* Everything this job needs after the count drops is read first: once a waited-for root job
   * reaches 0, its waiter releases the job's block (DrainJobsUntilComplete), and the old order
   * wrote is_done into it afterwards. */
  Job* const parent_job = this->parent;
  const bool waited = this->is_waiting;
  JobAllocBlock& block = this->alloc_block;
  this->is_done = true;
  /* Release: what the job wrote (a restored granule, a merged snapshot) is visible to the thread
   * whose acquire load sees the count reach 0 (DrainJobsUntilComplete). Relaxed was enough on
   * x86 only by accident of the hardware, and never stopped the compiler from reordering. */
  uint32_t res = this->unfinished_jobs.fetch_sub(1, std::memory_order_acq_rel);
  if (parent_job != nullptr && res == 1) /* last child */
    parent_job->finish();
  if (!waited) /* a thread is waiting for job, it must release */
    block.deref();
}

static inline job::Job* KickRootJob(job::JobTaskThread* dt, job::JobFunction fn)
{
  job::Job* root = dt->create_job(fn);
  root->is_waiting = true;
  root->kick();
  return root;
}

// Helper: Drain remaining jobs until the root job and all children are complete.
static inline void DrainJobsUntilComplete(job::JobTaskThread* dt, job::Job* root)
{
  while (root->unfinished_jobs.load(std::memory_order_acquire) != 0)
  {
    job::Job* k = dt->get_valid_job();
    if (k != nullptr)
      dt->execute(*k);
    else
      job::pause_thread();
  }
  root->alloc_block.deref();
}

} /* namespace job */
