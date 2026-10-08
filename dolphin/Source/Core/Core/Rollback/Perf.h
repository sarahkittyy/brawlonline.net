#pragma once

// Rollback profiling - Superluminal and Tracy can be active simultaneously.
// Comment out either line below to disable that backend at compile time.
#if defined(HAVE_SUPERLUMINAL_PERFORMANCEAPI)
#define ROLLBACK_PROFILE_SUPERLUMINAL
#endif
#if defined(HAVE_TRACY)
#define ROLLBACK_PROFILE_TRACY
#endif

#define PERF_CONCAT_(a, b) a##b
#define PERF_CONCAT(a, b) PERF_CONCAT_(a, b)

#if defined(HAVE_SUPERLUMINAL_PERFORMANCEAPI) && defined(ROLLBACK_PROFILE_SUPERLUMINAL)
#include <Superluminal/PerformanceAPI_loader.h>

namespace Rollback
{
extern PerformanceAPI_Functions g_perf_api;

struct InstrumentationScope
{
  explicit InstrumentationScope(const char* id)
  {
    if (g_perf_api.BeginEvent)
      g_perf_api.BeginEvent(id, nullptr, PERFORMANCEAPI_DEFAULT_COLOR);
  }
  ~InstrumentationScope()
  {
    if (g_perf_api.EndEvent)
      g_perf_api.EndEvent();
  }
  InstrumentationScope(const InstrumentationScope&) = delete;
  InstrumentationScope& operator=(const InstrumentationScope&) = delete;
};
}  // namespace Rollback

#define ROLLBACK_SL_ZONE_()                                                                        \
  ::Rollback::InstrumentationScope PERF_CONCAT(_sl_, __LINE__)(__FUNCTION__)
#define ROLLBACK_SL_ZONE_N_(name) ::Rollback::InstrumentationScope PERF_CONCAT(_sl_, __LINE__)(name)
#define ROLLBACK_SL_THREAD_NAME_(name)                                                             \
  do                                                                                               \
  {                                                                                                \
    if (::Rollback::g_perf_api.SetCurrentThreadName)                                               \
      ::Rollback::g_perf_api.SetCurrentThreadName(name);                                           \
  } while (0)

#else

#define ROLLBACK_SL_ZONE_()
#define ROLLBACK_SL_ZONE_N_(name)
#define ROLLBACK_SL_THREAD_NAME_(name) ((void)0)

#endif  // HAVE_SUPERLUMINAL_PERFORMANCEAPI && ROLLBACK_PROFILE_SUPERLUMINAL

#if defined(HAVE_TRACY) && defined(ROLLBACK_PROFILE_TRACY)
#include "tracy/tracy/Tracy.hpp"

#define ROLLBACK_TR_ZONE_() ZoneScoped
#define ROLLBACK_TR_ZONE_N_(name) ZoneScopedN(name)
#define ROLLBACK_TR_THREAD_NAME_(name) tracy::SetThreadName(name)

#else

#define ROLLBACK_TR_ZONE_()
#define ROLLBACK_TR_ZONE_N_(name)
#define ROLLBACK_TR_THREAD_NAME_(name) ((void)0)

#endif  // HAVE_TRACY && ROLLBACK_PROFILE_TRACY

namespace Rollback
{
void PerfInit();
}  // namespace Rollback

#define ROLLBACK_ZONE()                                                                            \
  ROLLBACK_SL_ZONE_();                                                                             \
  ROLLBACK_TR_ZONE_()
#define ROLLBACK_ZONE_N(name)                                                                      \
  ROLLBACK_SL_ZONE_N_(name);                                                                       \
  ROLLBACK_TR_ZONE_N_(name)
#define ROLLBACK_THREAD_NAME(name)                                                                 \
  ROLLBACK_SL_THREAD_NAME_(name);                                                                  \
  ROLLBACK_TR_THREAD_NAME_(name)
