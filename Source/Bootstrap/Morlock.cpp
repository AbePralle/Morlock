#include <stdio.h>
namespace std {}
using namespace std;
#include "Morlock.h"

//=============================================================================
//  NativeCPP.cpp
//
//  Rogue runtime routines.
//=============================================================================

#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <sys/types.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <inttypes.h>
#include <exception>
#include <cstddef>

#if defined(ROGUE_PLATFORM_WINDOWS)
#  include <sys/timeb.h>
#else
#  include <sys/time.h>
#  include <unistd.h>
#  include <signal.h>
#  include <dirent.h>
#  include <sys/socket.h>
#  include <sys/uio.h>
#  include <sys/stat.h>
#  include <netdb.h>
#  include <errno.h>
#endif

#if defined(ANDROID)
  #include <netinet/in.h>
#endif

#if defined(_WIN32)
#  include <direct.h>
#  define chdir _chdir
#endif

#if TARGET_OS_IPHONE
#  include <sys/types.h>
#  include <sys/sysctl.h>
#endif

#if defined(__APPLE__)
  #include <mach-o/dyld.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef PATH_MAX
#  define PATH_MAX 4096
#endif

//-----------------------------------------------------------------------------
//  GLOBAL PROPERTIES
//-----------------------------------------------------------------------------
bool               Rogue_gc_logging   = false;
int                Rogue_gc_threshold = ROGUE_GC_THRESHOLD_DEFAULT;
int                Rogue_gc_count     = 0; // Purely informational
bool               Rogue_gc_requested = false;
bool               Rogue_gc_active    = false; // Are we collecting right now?
RogueLogical       Rogue_configured = 0;
int                Rogue_argc;
const char**       Rogue_argv;
RogueCallbackInfo  Rogue_on_gc_begin;
RogueCallbackInfo  Rogue_on_gc_trace_finished;
RogueCallbackInfo  Rogue_on_gc_end;
char               RogueDebugTrace::buffer[512];
ROGUE_THREAD_LOCAL RogueDebugTrace* Rogue_call_stack = 0;

struct RogueWeakReference;
RogueWeakReference* Rogue_weak_references = 0;

//-----------------------------------------------------------------------------
//  Multithreading
//-----------------------------------------------------------------------------
#if ROGUE_THREAD_MODE == ROGUE_THREAD_MODE_PTHREADS

#define ROGUE_MUTEX_LOCK(_M) pthread_mutex_lock(&(_M))
#define ROGUE_MUTEX_UNLOCK(_M) pthread_mutex_unlock(&(_M))
#define ROGUE_MUTEX_DEF(_N) pthread_mutex_t _N = PTHREAD_MUTEX_INITIALIZER

#define ROGUE_COND_STARTWAIT(_V,_M) ROGUE_MUTEX_LOCK(_M);
#define ROGUE_COND_DOWAIT(_V,_M,_C) while (_C) pthread_cond_wait(&(_V), &(_M));
#define ROGUE_COND_ENDWAIT(_V,_M) ROGUE_MUTEX_UNLOCK(_M);
#define ROGUE_COND_WAIT(_V,_M,_C) \
  ROGUE_COND_STARTWAIT(_V,_M); \
  ROGUE_COND_DOWAIT(_V,_M,_C); \
  ROGUE_COND_ENDWAIT(_V,_M);
#define ROGUE_COND_DEF(_N) pthread_cond_t _N = PTHREAD_COND_INITIALIZER
#define ROGUE_COND_NOTIFY_ONE(_V,_M,_C)    \
  ROGUE_MUTEX_LOCK(_M);                    \
  _C ;                                     \
  pthread_cond_signal(&(_V));              \
  ROGUE_MUTEX_UNLOCK(_M);
#define ROGUE_COND_NOTIFY_ALL(_V,_M,_C)    \
  ROGUE_MUTEX_LOCK(_M);                    \
  _C ;                                     \
  pthread_cond_broadcast(&(_V));           \
  ROGUE_MUTEX_UNLOCK(_M);

#define ROGUE_THREAD_DEF(_N) pthread_t _N
#define ROGUE_THREAD_JOIN(_T) pthread_join(_T, NULL)
#define ROGUE_THREAD_START(_T, _F) pthread_create(&(_T), NULL, _F, NULL)

#elif ROGUE_THREAD_MODE == ROGUE_THREAD_MODE_CPP

#include <exception>
#include <condition_variable>

#define ROGUE_MUTEX_LOCK(_M) _M.lock()
#define ROGUE_MUTEX_UNLOCK(_M) _M.unlock()
#define ROGUE_MUTEX_DEF(_N) std::mutex _N

#define ROGUE_COND_STARTWAIT(_V,_M) { std::unique_lock<std::mutex> LK(_M);
#define ROGUE_COND_DOWAIT(_V,_M,_C) while (_C) (_V).wait(LK);
#define ROGUE_COND_ENDWAIT(_V,_M) }
#define ROGUE_COND_WAIT(_V,_M,_C) \
  ROGUE_COND_STARTWAIT(_V,_M); \
  ROGUE_COND_DOWAIT(_V,_M,_C); \
  ROGUE_COND_ENDWAIT(_V,_M);
#define ROGUE_COND_DEF(_N) std::condition_variable _N
#define ROGUE_COND_NOTIFY_ONE(_V,_M,_C) {  \
  std::unique_lock<std::mutex> LK2(_M);    \
  _C ;                                     \
  (_V).notify_one(); }
#define ROGUE_COND_NOTIFY_ALL(_V,_M,_C) {  \
  std::unique_lock<std::mutex> LK2(_M);    \
  _C ;                                     \
  (_V).notify_all(); }

#define ROGUE_THREAD_DEF(_N) std::thread _N
#define ROGUE_THREAD_JOIN(_T) (_T).join()
#define ROGUE_THREAD_START(_T, _F) (_T = std::thread([] () {_F(NULL);}),0)

#endif

#if ROGUE_THREAD_MODE != ROGUE_THREAD_MODE_NONE

// Thread mutex locks around creation and destruction of threads
static ROGUE_MUTEX_DEF(Rogue_mt_thread_mutex);
static int Rogue_mt_tc = 0; // Thread count.  Always set under above lock.
static std::atomic_bool Rogue_mt_terminating(false); // True when terminating.

static void Rogue_thread_register ()
{
  ROGUE_MUTEX_LOCK(Rogue_mt_thread_mutex);
#if ROGUE_THREAD_MODE == ROGUE_THREAD_MODE_PTHREADS
  int n = (int)Rogue_mt_tc;
#endif
  ++Rogue_mt_tc;
  ROGUE_MUTEX_UNLOCK(Rogue_mt_thread_mutex);
#if ROGUE_THREAD_MODE == ROGUE_THREAD_MODE_PTHREADS
  char name[64];
  sprintf(name, "Thread-%i", n); // Nice names are good for valgrind

#if ROGUE_PLATFORM_MACOS
  pthread_setname_np(name);
#elif __linux__
  pthread_setname_np(pthread_self(), name);
#endif
// It should be possible to get thread names working on lots of other
// platforms too.  The functions just vary a bit.
#endif
}

static void Rogue_thread_unregister ()
{
  ROGUE_EXIT;
  ROGUE_MUTEX_LOCK(Rogue_mt_thread_mutex);
  ROGUE_ENTER;
  --Rogue_mt_tc;
  ROGUE_MUTEX_UNLOCK(Rogue_mt_thread_mutex);
}


#define ROGUE_THREADS_WAIT_FOR_ALL Rogue_threads_wait_for_all();

void Rogue_threads_wait_for_all ()
{
  Rogue_mt_terminating = true;
  ROGUE_EXIT;
  int wait = 2; // Initial Xms
  int wait_step = 1;
  while (true)
  {
    ROGUE_MUTEX_LOCK(Rogue_mt_thread_mutex);
    if (Rogue_mt_tc <= 1) // Shouldn't ever really be less than 1
    {
      ROGUE_MUTEX_UNLOCK(Rogue_mt_thread_mutex);
      break;
    }
    ROGUE_MUTEX_UNLOCK(Rogue_mt_thread_mutex);
    usleep(1000 * wait);
    wait_step++;
    if (!(wait_step % 15) && (wait < 500)) wait *= 2; // Max backoff ~500ms
  }
  ROGUE_ENTER;
}

#else

#define ROGUE_THREADS_WAIT_FOR_ALL /* no-op if there's only one thread! */

static void Rogue_thread_register ()
{
}
static void Rogue_thread_unregister ()
{
}

#endif

// Singleton handling
#if ROGUE_THREAD_MODE
#define ROGUE_GET_SINGLETON(_S) (_S)->_singleton.load()
#define ROGUE_SET_SINGLETON(_S,_V) (_S)->_singleton.store(_V);
#if ROGUE_THREAD_MODE == ROGUE_THREAD_MODE_PTHREADS
pthread_mutex_t Rogue_thread_singleton_lock;
#define ROGUE_SINGLETON_LOCK ROGUE_MUTEX_LOCK(Rogue_thread_singleton_lock);
#define ROGUE_SINGLETON_UNLOCK ROGUE_MUTEX_UNLOCK(Rogue_thread_singleton_lock);
#else
std::recursive_mutex Rogue_thread_singleton_lock;
#define ROGUE_SINGLETON_LOCK Rogue_thread_singleton_lock.lock();
#define ROGUE_SINGLETON_UNLOCK Rogue_thread_singleton_lock.unlock();
#endif
#else
#define ROGUE_GET_SINGLETON(_S) (_S)->_singleton
#define ROGUE_SET_SINGLETON(_S,_V) (_S)->_singleton = _V;
#define ROGUE_SINGLETON_LOCK
#define ROGUE_SINGLETON_UNLOCK
#endif

//-----------------------------------------------------------------------------
//  GC
//-----------------------------------------------------------------------------
#if ROGUE_GC_MODE_AUTO_MT
// See the Rogue MT GC diagram for an explanation of some of this.

#define ROGUE_GC_VAR static volatile int
// (Curiously, volatile seems to help performance slightly.)

static thread_local bool Rogue_mtgc_is_gc_thread = false;

#define ROGUE_MTGC_BARRIER asm volatile("" : : : "memory");

// Atomic LL insertion
#define ROGUE_LINKED_LIST_INSERT(__OLD,__NEW,__NEW_NEXT)            \
  for(;;) {                                                         \
    auto tmp = __OLD;                                               \
    __NEW_NEXT = tmp;                                               \
    if (__sync_bool_compare_and_swap(&(__OLD), tmp, __NEW)) break;  \
  }

// We assume malloc is safe, but the SOA needs safety if it's being used.
#if ROGUEMM_SMALL_ALLOCATION_SIZE_LIMIT >= 0
static ROGUE_MUTEX_DEF(Rogue_mtgc_soa_mutex);
#define ROGUE_GC_SOA_LOCK    ROGUE_MUTEX_LOCK(Rogue_mtgc_soa_mutex);
#define ROGUE_GC_SOA_UNLOCK  ROGUE_MUTEX_UNLOCK(Rogue_mtgc_soa_mutex);
#else
#define ROGUE_GC_SOA_LOCK
#define ROGUE_GC_SOA_UNLOCK
#endif

static inline void Rogue_collect_garbage_real ();
void Rogue_collect_garbage_real_noinline ()
{
  Rogue_collect_garbage_real();
}

#if ROGUE_THREAD_MODE
#if ROGUE_THREAD_MODE_PTHREADS
#elif ROGUE_THREAD_MODE_CPP
#else
#error Currently, only --threads=pthreads and --threads=cpp are supported with --gc=auto-mt
#endif
#endif

// This is how unlikely() works in the Linux kernel
#define ROGUE_UNLIKELY(_X) __builtin_expect(!!(_X), 0)

#define ROGUE_GC_CHECK if (ROGUE_UNLIKELY(Rogue_mtgc_w) \
  && !ROGUE_UNLIKELY(Rogue_mtgc_is_gc_thread))          \
  Rogue_mtgc_W2_W3_W4(); // W1

 ROGUE_MUTEX_DEF(Rogue_mtgc_w_mutex);
static ROGUE_MUTEX_DEF(Rogue_mtgc_s_mutex);
static ROGUE_COND_DEF(Rogue_mtgc_w_cond);
static ROGUE_COND_DEF(Rogue_mtgc_s_cond);

ROGUE_GC_VAR Rogue_mtgc_w = 0;
ROGUE_GC_VAR Rogue_mtgc_s = 0;

// Only one worker can be "running" (waiting for) the GC at a time.
// To run, set r = 1, and wait for GC to set it to 0.  If r is already
// 1, just wait.
static ROGUE_MUTEX_DEF(Rogue_mtgc_r_mutex);
static ROGUE_COND_DEF(Rogue_mtgc_r_cond);
ROGUE_GC_VAR Rogue_mtgc_r = 0;

static ROGUE_MUTEX_DEF(Rogue_mtgc_g_mutex);
static ROGUE_COND_DEF(Rogue_mtgc_g_cond);
ROGUE_GC_VAR Rogue_mtgc_g = 0; // Should GC

static int Rogue_mtgc_should_quit = 0; // 0:normal 1:should-quit 2:has-quit

static ROGUE_THREAD_DEF(Rogue_mtgc_thread);

static void Rogue_mtgc_W2_W3_W4 (void);
static inline void Rogue_mtgc_W3_W4 (void);

inline void Rogue_mtgc_B1 ()
{
  ROGUE_COND_NOTIFY_ONE(Rogue_mtgc_s_cond, Rogue_mtgc_s_mutex, ++Rogue_mtgc_s);
}

static inline void Rogue_mtgc_B2_etc ()
{
  Rogue_mtgc_W3_W4();
  // We can probably just do GC_CHECK here rather than this more expensive
  // locking version.
  ROGUE_MUTEX_LOCK(Rogue_mtgc_w_mutex);
  auto w = Rogue_mtgc_w;
  ROGUE_MUTEX_UNLOCK(Rogue_mtgc_w_mutex);
  if (ROGUE_UNLIKELY(w)) Rogue_mtgc_W2_W3_W4(); // W1
}

static inline void Rogue_mtgc_W3_W4 ()
{
  // W3
  ROGUE_COND_WAIT(Rogue_mtgc_w_cond, Rogue_mtgc_w_mutex, Rogue_mtgc_w != 0);

  // W4
  ROGUE_MUTEX_LOCK(Rogue_mtgc_s_mutex);
  --Rogue_mtgc_s;
  ROGUE_MUTEX_UNLOCK(Rogue_mtgc_s_mutex);
}

static void Rogue_mtgc_W2_W3_W4 ()
{
  // W2
  ROGUE_COND_NOTIFY_ONE(Rogue_mtgc_s_cond, Rogue_mtgc_s_mutex, ++Rogue_mtgc_s);
  Rogue_mtgc_W3_W4();
}


static thread_local int Rogue_mtgc_entered = 1;

inline void Rogue_mtgc_enter()
{
  if (ROGUE_UNLIKELY(Rogue_mtgc_is_gc_thread)) return;
  if (ROGUE_UNLIKELY(Rogue_mtgc_entered))
#ifdef ROGUE_MTGC_DEBUG
  {
    ROGUE_LOG_ERROR("ALREADY ENTERED\n");
    exit(1);
  }
#else
  {
    ++Rogue_mtgc_entered;
    return;
  }
#endif

  Rogue_mtgc_entered = 1;
  Rogue_mtgc_B2_etc();
}

inline void Rogue_mtgc_exit()
{
  if (ROGUE_UNLIKELY(Rogue_mtgc_is_gc_thread)) return;
  if (ROGUE_UNLIKELY(Rogue_mtgc_entered <= 0))
  {
    ROGUE_LOG_ERROR("Unabalanced Rogue enter/exit\n");
    exit(1);
  }

  --Rogue_mtgc_entered;
  Rogue_mtgc_B1();
}

static void Rogue_mtgc_M1_M2_GC_M3 (int quit)
{
  // M1
  ROGUE_MUTEX_LOCK(Rogue_mtgc_w_mutex);
  Rogue_mtgc_w = 1;
  ROGUE_MUTEX_UNLOCK(Rogue_mtgc_w_mutex);

  // M2
#if (ROGUE_THREAD_MODE == ROGUE_THREAD_MODE_PTHREADS) && ROGUE_MTGC_DEBUG
  ROGUE_MUTEX_LOCK(Rogue_mtgc_s_mutex);
  while (Rogue_mtgc_s != Rogue_mt_tc)
  {
    if (Rogue_mtgc_s > Rogue_mt_tc || Rogue_mtgc_s < 0)
    {
      ROGUE_LOG_ERROR("INVALID VALUE OF S %i %i\n", Rogue_mtgc_s, Rogue_mt_tc);
      exit(1);
    }

    pthread_cond_wait(&Rogue_mtgc_s_cond, &Rogue_mtgc_s_mutex);
  }
  // We should actually be okay holding the S lock until the
  // very end of the function if we want, and this would prevent
  // threads that were blocking from ever leaving B2.  But
  // We should be okay anyway, though S may temporarily != TC.
  //ROGUE_MUTEX_UNLOCK(Rogue_mtgc_s_mutex);
#else
  ROGUE_COND_STARTWAIT(Rogue_mtgc_s_cond, Rogue_mtgc_s_mutex);
  ROGUE_COND_DOWAIT(Rogue_mtgc_s_cond, Rogue_mtgc_s_mutex, Rogue_mtgc_s != Rogue_mt_tc);
#endif

#if ROGUE_MTGC_DEBUG
  ROGUE_MUTEX_LOCK(Rogue_mtgc_w_mutex);
  Rogue_mtgc_w = 2;
  ROGUE_MUTEX_UNLOCK(Rogue_mtgc_w_mutex);
#endif

  // GC
  // Grab the SOA lock for symmetry.  It should actually never
  // be held by another thread since they're all in GC sleep.
  ROGUE_GC_SOA_LOCK;
  Rogue_collect_garbage_real();

  //NOTE: It's possible (Rogue_mtgc_s != Rogue_mt_tc) here, if we gave up the S
  //      lock, though they should quickly go back to equality.

  if (quit)
  {
    // Run a few more times to finish up
    Rogue_collect_garbage_real_noinline();
    Rogue_collect_garbage_real_noinline();

    // Free from the SOA
    RogueAllocator_free_all();
  }
  ROGUE_GC_SOA_UNLOCK;

  // M3
  ROGUE_COND_NOTIFY_ALL(Rogue_mtgc_w_cond, Rogue_mtgc_w_mutex, Rogue_mtgc_w = 0);

  // Could have done this much earlier
  ROGUE_COND_ENDWAIT(Rogue_mtgc_s_cond, Rogue_mtgc_s_mutex);
}

static void * Rogue_mtgc_threadproc (void *)
{
  Rogue_mtgc_is_gc_thread = true;
  int quit = 0;
  while (quit == 0)
  {
    ROGUE_COND_STARTWAIT(Rogue_mtgc_g_cond, Rogue_mtgc_g_mutex);
    ROGUE_COND_DOWAIT(Rogue_mtgc_g_cond, Rogue_mtgc_g_mutex, !Rogue_mtgc_g && !Rogue_mtgc_should_quit);
    Rogue_mtgc_g = 0;
    quit = Rogue_mtgc_should_quit;
    ROGUE_COND_ENDWAIT(Rogue_mtgc_g_cond, Rogue_mtgc_g_mutex);

    ROGUE_MUTEX_LOCK(Rogue_mt_thread_mutex);

    Rogue_mtgc_M1_M2_GC_M3(quit);

    ROGUE_MUTEX_UNLOCK(Rogue_mt_thread_mutex);

    ROGUE_COND_NOTIFY_ALL(Rogue_mtgc_r_cond, Rogue_mtgc_r_mutex, Rogue_mtgc_r = 0);
  }

  ROGUE_MUTEX_LOCK(Rogue_mtgc_g_mutex);
  Rogue_mtgc_should_quit = 2;
  Rogue_mtgc_g = 0;
  ROGUE_MUTEX_UNLOCK(Rogue_mtgc_g_mutex);
  return NULL;
}

// Cause GC to run and wait for a GC to complete.
void Rogue_mtgc_run_gc_and_wait ()
{
  bool again;
  do
  {
    again = false;
    ROGUE_COND_STARTWAIT(Rogue_mtgc_r_cond, Rogue_mtgc_r_mutex);
    if (Rogue_mtgc_r == 0)
    {
      Rogue_mtgc_r = 1;

      // Signal GC to run
      ROGUE_COND_NOTIFY_ONE(Rogue_mtgc_g_cond, Rogue_mtgc_g_mutex, Rogue_mtgc_g = 1);
    }
    else
    {
      // If one or more simultaneous requests to run the GC came in, run it
      // again.
      again = (Rogue_mtgc_r == 1);
      ++Rogue_mtgc_r;
    }
    ROGUE_EXIT;
    ROGUE_COND_DOWAIT(Rogue_mtgc_r_cond, Rogue_mtgc_r_mutex, Rogue_mtgc_r != 0);
    ROGUE_COND_ENDWAIT(Rogue_mtgc_r_cond, Rogue_mtgc_r_mutex);
    ROGUE_ENTER;
  }
  while (again);
}

static void Rogue_mtgc_quit_gc_thread ()
{
  //NOTE: This could probably be simplified (and the quit behavior removed
  //      from Rogue_mtgc_M1_M2_GC_M3) since we now wait for all threads
  //      to stop before calling this.
  // This doesn't quite use the normal condition variable pattern, sadly.
  ROGUE_EXIT;
  timespec ts;
  ts.tv_sec = 0;
  ts.tv_nsec = 1000000 * 10; // 10ms
  while (true)
  {
    bool done = true;
    ROGUE_COND_STARTWAIT(Rogue_mtgc_g_cond, Rogue_mtgc_g_mutex);
    if (Rogue_mtgc_should_quit != 2)
    {
      done = false;
      Rogue_mtgc_g = 1;
      Rogue_mtgc_should_quit = 1;
#if ROGUE_THREAD_MODE == ROGUE_THREAD_MODE_PTHREADS
      pthread_cond_signal(&Rogue_mtgc_g_cond);
#elif ROGUE_THREAD_MODE == ROGUE_THREAD_MODE_CPP
      Rogue_mtgc_g_cond.notify_one();
#endif
    }
    ROGUE_COND_ENDWAIT(Rogue_mtgc_g_cond, Rogue_mtgc_g_mutex);
    if (done) break;
    nanosleep(&ts, NULL);
  }
  ROGUE_THREAD_JOIN(Rogue_mtgc_thread);
  ROGUE_ENTER;
}

void Rogue_configure_gc()
{
  int c = ROGUE_THREAD_START(Rogue_mtgc_thread, Rogue_mtgc_threadproc);
  if (c != 0)
  {
    exit(77); //TODO: Do something better in this (hopefully) rare case.
  }
}

// Used as part of the ROGUE_BLOCKING_CALL macro.
template<typename RT> RT Rogue_mtgc_reenter (RT expr)
{
  ROGUE_ENTER;
  return expr;
}

#include <atomic>

// We do all relaxed operations on this.  It's possible this will lead to
// something "bad", but I don't think *too* bad.  Like an extra GC.
// And I think that'll be rare, since the reset happens when all the
// threads are synced.  But I could be wrong.  Should probably think
// about this harder.
std::atomic_int Rogue_allocation_bytes_until_gc(Rogue_gc_threshold);
#define ROGUE_GC_COUNT_BYTES(__x) Rogue_allocation_bytes_until_gc.fetch_sub(__x, std::memory_order_relaxed);
#define ROGUE_GC_AT_THRESHOLD (Rogue_allocation_bytes_until_gc.load(std::memory_order_relaxed) <= 0)
#define ROGUE_GC_RESET_COUNT Rogue_allocation_bytes_until_gc.store(Rogue_gc_threshold, std::memory_order_relaxed);

#else // Anything besides auto-mt

#define ROGUE_GC_CHECK /* Does nothing in non-auto-mt modes */

#define ROGUE_GC_SOA_LOCK
#define ROGUE_GC_SOA_UNLOCK

int Rogue_allocation_bytes_until_gc = Rogue_gc_threshold;
#define ROGUE_GC_COUNT_BYTES(__x) Rogue_allocation_bytes_until_gc -= (__x);
#define ROGUE_GC_AT_THRESHOLD (Rogue_allocation_bytes_until_gc <= 0)
#define ROGUE_GC_RESET_COUNT Rogue_allocation_bytes_until_gc = Rogue_gc_threshold;


#define ROGUE_MTGC_BARRIER
#define ROGUE_LINKED_LIST_INSERT(__OLD,__NEW,__NEW_NEXT) do {__NEW_NEXT = __OLD; __OLD = __NEW;} while(false)

#endif

//-----------------------------------------------------------------------------
//  Misc Utility
//-----------------------------------------------------------------------------
void Rogue_define_literal_string( int index, const char* st, int count=-1 );

void Rogue_define_literal_string( int index, const char* st, int count )
{
  Rogue_literal_strings[index] = (RogueString*) RogueObject_retain( RogueString_create_from_utf8( st, count ) );
}

//-----------------------------------------------------------------------------
//  RogueDebugTrace
//-----------------------------------------------------------------------------
RogueDebugTrace::RogueDebugTrace( const char* method_signature, const char* filename, int line )
  : method_signature(method_signature), filename(filename), line(line), previous_trace(0)
{
  previous_trace = Rogue_call_stack;
  Rogue_call_stack = this;
}

RogueDebugTrace::~RogueDebugTrace()
{
  Rogue_call_stack = previous_trace;
}

int RogueDebugTrace::count()
{
  int n = 1;
  RogueDebugTrace* current = previous_trace;
  while (current)
  {
    ++n;
    current = current->previous_trace;
  }
  return n;
}

char* RogueDebugTrace::to_c_string()
{
  snprintf( buffer, 512, "[%s %s:%d]", method_signature, filename, line );
  return buffer;
}

//-----------------------------------------------------------------------------
//  RogueType
//-----------------------------------------------------------------------------
RogueArray* RogueType_create_array( int count, int element_size, bool is_reference_array, int element_type_index )
{
  if (count < 0) count = 0;
  int data_size  = count * element_size;
  int total_size = sizeof(RogueArray) + data_size;

  RogueArray* array = (RogueArray*) RogueAllocator_allocate_object( RogueTypeArray->allocator, RogueTypeArray, total_size, element_type_index);

  array->count = count;
  array->element_size = element_size;
  array->is_reference_array = is_reference_array;

  return array;
}

RogueObject* RogueType_create_object( RogueType* THIS, RogueInt32 size )
{
  ROGUE_DEF_LOCAL_REF_NULL(RogueObject*, obj);
  RogueInitFn  fn;
#if ROGUE_GC_MODE_BOEHM_TYPED
  ROGUE_DEBUG_STATEMENT(assert(size == 0 || size == THIS->object_size));
#endif

  obj = RogueAllocator_allocate_object( THIS->allocator, THIS, size ? size : THIS->object_size );

  if ((fn = THIS->init_object_fn)) return fn( obj );
  else                             return obj;
}

RogueLogical RogueType_instance_of( RogueType* THIS, RogueType* ancestor_type )
{
  if (THIS == ancestor_type)
  {
    return true;
  }

  int count = THIS->base_type_count;
  RogueType** base_type_ptr = THIS->base_types - 1;
  while (--count >= 0)
  {
    if (ancestor_type == *(++base_type_ptr))
    {
      return true;
    }
  }

  return false;
}

RogueString* RogueType_name( RogueType* THIS )
{
  return Rogue_literal_strings[ THIS->name_index ];
}

bool RogueType_name_equals( RogueType* THIS, const char* name )
{
  // For debugging purposes
  RogueString* st = Rogue_literal_strings[ THIS->name_index ];
  if ( !st ) return false;

  return (0 == strcmp((char*)st->utf8,name));
}

void RogueType_print_name( RogueType* THIS )
{
  RogueString* st = Rogue_literal_strings[ THIS->name_index ];
  if (st)
  {
    ROGUE_LOG( "%s", st->utf8 );
  }
}

RogueType* RogueType_retire( RogueType* THIS )
{
  if (THIS->base_types)
  {
#if !ROGUE_GC_MODE_BOEHM
    delete [] THIS->base_types;
#endif
    THIS->base_types = 0;
    THIS->base_type_count = 0;
  }

  return THIS;
}

RogueObject* RogueType_singleton( RogueType* THIS )
{
  RogueInitFn fn;
  RogueObject * r = ROGUE_GET_SINGLETON(THIS);
  if (r) return r;

  ROGUE_SINGLETON_LOCK;

#if ROGUE_THREAD_MODE // Very minor optimization: don't check twice if unthreaded
  // We probably need to initialize the singleton, but now that we have the
  // lock, we double check.
  r = ROGUE_GET_SINGLETON(THIS);
  if (r)
  {
    // Ah, someone else just initialized it.  We'll use that.
    ROGUE_SINGLETON_UNLOCK;
  }
  else
#endif
  {
    // Yes, we'll be the one doing the initializing.

    // NOTE: _singleton must be assigned before calling init_object()
    // so we can't just call RogueType_create_object().
    r = RogueAllocator_allocate_object( THIS->allocator, THIS, THIS->object_size );

    ROGUE_SET_SINGLETON(THIS, r);

    if ((fn = THIS->init_object_fn)) r = fn( ROGUE_ARG(r) );

    ROGUE_SINGLETON_UNLOCK;

    if ((fn = THIS->init_fn)) r = fn( THIS->_singleton );
  }

  return r;
}

//-----------------------------------------------------------------------------
//  RogueObject
//-----------------------------------------------------------------------------
RogueObject* RogueObject_as( RogueObject* THIS, RogueType* specialized_type )
{
  if (RogueObject_instance_of(THIS,specialized_type)) return THIS;
  return 0;
}

RogueLogical RogueObject_instance_of( RogueObject* THIS, RogueType* ancestor_type )
{
  if ( !THIS )
  {
    return false;
  }

  return RogueType_instance_of( THIS->type, ancestor_type );
}

RogueLogical RogueObject_is_type( RogueObject* THIS, RogueType* ancestor_type )
{
  return THIS ? (THIS->type == ancestor_type) : false;
}

void* RogueObject_retain( RogueObject* THIS )
{
  ROGUE_INCREF(THIS);
  return THIS;
}

void* RogueObject_release( RogueObject* THIS )
{
  ROGUE_DECREF(THIS);
  return THIS;
}

RogueString* RogueObject_to_string( RogueObject* THIS )
{
  RogueToStringFn fn = THIS->type->to_string_fn;
  if (fn) return fn( THIS );

  return Rogue_literal_strings[ THIS->type->name_index ];
}

void RogueObject_trace( void* obj )
{
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
}

void RogueString_trace( void* obj )
{
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
}

void RogueArray_trace( void* obj )
{
  int count;
  RogueObject** src;
  RogueArray* array = (RogueArray*) obj;

  if ( !array || array->object_size < 0 ) return;
  array->object_size = ~array->object_size;

  if ( !array->is_reference_array ) return;

  count = array->count;
  src = array->as_objects + count;
  while (--count >= 0)
  {
    RogueObject* cur = *(--src);
    if (cur && cur->object_size >= 0)
    {
      cur->type->trace_fn( cur );
    }
  }
}

//-----------------------------------------------------------------------------
//  RogueString
//-----------------------------------------------------------------------------
RogueString* RogueString_create_with_byte_count( int byte_count )
{
  if (byte_count < 0) byte_count = 0;

#if ROGUE_GC_MODE_BOEHM_TYPED
  RogueString* st = (RogueString*) RogueAllocator_allocate_object( RogueTypeString->allocator, RogueTypeString, RogueTypeString->object_size );
  char * data = (char *)GC_malloc_atomic_ignore_off_page( byte_count + 1 );
  data[0] = 0;
  data[byte_count] = 0;
  st->utf8 = (RogueByte*)data;
#else
  int total_size = sizeof(RogueString) + (byte_count+1);

  RogueString* st = (RogueString*) RogueAllocator_allocate_object( RogueTypeString->allocator, RogueTypeString, total_size );
#endif
  st->byte_count = byte_count;

  return st;
}

RogueString* RogueString_create_from_utf8( const char* utf8, int count )
{
  if (count == -1) count = (int) strlen( utf8 );

  if (count >= 3 && (unsigned char)utf8[0] == 0xEF && (unsigned char)utf8[1] == 0xBB && (unsigned char)utf8[2] == 0xBF)
  {
    // Skip Byte Order Mark (BOM)
    utf8  += 3;
    count -= 3;
  }

  RogueString* st = RogueString_create_with_byte_count( count );
  memcpy( st->utf8, utf8, count );
  return RogueString_validate( st );
}

RogueString* RogueString_create_from_characters( RogueCharacter_List* characters )
{
  if ( !characters ) return RogueString_create_with_byte_count(0);

  RogueCharacter* data = characters->data->as_characters;
  int count = characters->count;
  int utf8_count = 0;
  for (int i=count; --i>=0; )
  {
    RogueCharacter ch = data[i];
    if (ch <= 0x7F)         ++utf8_count;
    else if (ch <= 0x7FF)   utf8_count += 2;
    else if (ch <= 0xFFFF)  utf8_count += 3;
    else                    utf8_count += 4;
  }

  RogueString* result = RogueString_create_with_byte_count( utf8_count );
  char*   dest = result->utf8;
  for (int i=0; i<count; ++i)
  {
    RogueCharacter ch = data[i];
    if (ch < 0)
    {
      *(dest++) = 0;
    }
    else if (ch <= 0x7F)
    {
      *(dest++) = (RogueByte) ch;
    }
    else if (ch <= 0x7FF)
    {
      dest[0] = (RogueByte) (0xC0 | ((ch >> 6) & 0x1F));
      dest[1] = (RogueByte) (0x80 | (ch & 0x3F));
      dest += 2;
    }
    else if (ch <= 0xFFFF)
    {
      dest[0] = (RogueByte) (0xE0 | ((ch >> 12) & 0xF));
      dest[1] = (RogueByte) (0x80 | ((ch >> 6) & 0x3F));
      dest[2] = (RogueByte) (0x80 | (ch & 0x3F));
      dest += 3;
    }
    else
    {
      dest[0] = (RogueByte) (0xF0 | ((ch >> 18) & 0x7));
      dest[1] = (RogueByte) (0x80 | ((ch >> 12) & 0x3F));
      dest[2] = (RogueByte) (0x80 | ((ch >> 6) & 0x3F));
      dest[3] = (RogueByte) (0x80 | (ch & 0x3F));
      dest += 4;
    }
  }

  result->character_count = count;

  return RogueString_validate( result );
}

void RogueString_print_string( RogueString* st )
{
  if (st)
  {
    RogueString_print_utf8( st->utf8, st->byte_count );
  }
  else
  {
    ROGUE_LOG( "null" );
  }
}

void RogueString_print_characters( RogueCharacter* characters, int count )
{
  if (characters)
  {
    RogueCharacter* src = characters - 1;
    while (--count >= 0)
    {
      int ch = *(++src);

      if (ch < 0)
      {
        putchar( 0 );
      }
      else if (ch < 0x80)
      {
        // %0xxxxxxx
        putchar( ch );
      }
      else if (ch < 0x800)
      {
        // %110xxxxx 10xxxxxx
        putchar( ((ch >> 6) & 0x1f) | 0xc0 );
        putchar( (ch & 0x3f) | 0x80 );
      }
      else if (ch <= 0xFFFF)
      {
        // %1110xxxx 10xxxxxx 10xxxxxx
        putchar( ((ch >> 12) & 15) | 0xe0 );
        putchar( ((ch >> 6) & 0x3f) | 0x80 );
        putchar( (ch & 0x3f) | 0x80 );
      }
      else
      {
        // Assume 21-bit
        // %11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
        putchar( 0xf0 | ((ch>>18) & 7) );
        putchar( 0x80 | ((ch>>12) & 0x3f) );
        putchar( 0x80 | ((ch>>6)  & 0x3f) );
        putchar( (ch & 0x3f) | 0x80 );
      }
    }
  }
  else
  {
    ROGUE_LOG( "null" );
  }
}

void RogueString_print_utf8( char* utf8, int count )
{
  --utf8;
  while (--count >= 0)
  {
    putchar( *(++utf8) );
  }
}

RogueCharacter RogueString_character_at( RogueString* THIS, int index )
{
  if (THIS->is_ascii) return (RogueCharacter) THIS->utf8[ index ];

  RogueInt32 offset = RogueString_set_cursor( THIS, index );
  char* utf8 = THIS->utf8;

  RogueCharacter ch = utf8[ offset ];
  if (ch & 0x80)
  {
    if (ch & 0x20)
    {
      if (ch & 0x10)
      {
        return ((ch&7)<<18)
            | ((utf8[offset+1] & 0x3F) << 12)
            | ((utf8[offset+2] & 0x3F) << 6)
            | (utf8[offset+3] & 0x3F);
      }
      else
      {
        return ((ch&15)<<12)
            | ((utf8[offset+1] & 0x3F) << 6)
            | (utf8[offset+2] & 0x3F);
      }
    }
    else
    {
      return ((ch&31)<<6)
          | (utf8[offset+1] & 0x3F);
    }
  }
  else
  {
    return ch;
  }
}

RogueInt32 RogueString_set_cursor( RogueString* THIS, int index )
{
  // Sets this string's cursor_offset and cursor_index and returns cursor_offset.
  if (THIS->is_ascii)
  {
    return THIS->cursor_offset = THIS->cursor_index = index;
  }

  char* utf8 = THIS->utf8;

  RogueInt32 c_offset;
  RogueInt32 c_index;
  if (index == 0)
  {
    THIS->cursor_index = 0;
    return THIS->cursor_offset = 0;
  }
  else if (index >= THIS->character_count - 1)
  {
    c_offset = THIS->byte_count;
    c_index = THIS->character_count;
  }
  else
  {
    c_offset  = THIS->cursor_offset;
    c_index = THIS->cursor_index;
  }

  while (c_index < index)
  {
    while ((utf8[++c_offset] & 0xC0) == 0x80) {}
    ++c_index;
  }

  while (c_index > index)
  {
    while ((utf8[--c_offset] & 0xC0) == 0x80) {}
    --c_index;
  }

  THIS->cursor_index = c_index;
  return THIS->cursor_offset = c_offset;
}

RogueString* RogueString_validate( RogueString* THIS )
{
  // Trims any invalid UTF-8, counts the number of characters, and sets the hash code
  THIS->is_ascii = 1;  // assumption

  int character_count = 0;
  int byte_count = THIS->byte_count;
  int i;
  char* utf8 = THIS->utf8;
  for (i=0; i<byte_count; ++character_count)
  {
    int b = utf8[ i ];
    if (b & 0x80)
    {
      THIS->is_ascii = 0;
      if ( !(b & 0x40) ) { break;}  // invalid UTF-8

      if (b & 0x20)
      {
        if (b & 0x10)
        {
          // %11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
          if (b & 0x08) { break;}
          if (i + 4 > byte_count || ((utf8[i+1] & 0xC0) != 0x80) || ((utf8[i+2] & 0xC0) != 0x80)
              || ((utf8[i+3] & 0xC0) != 0x80)) { break;}
          i += 4;
        }
        else
        {
          // %1110xxxx 10xxxxxx 10xxxxxx
          if (i + 3 > byte_count || ((utf8[i+1] & 0xC0) != 0x80) || ((utf8[i+2] & 0xC0) != 0x80)) { break;}
          i += 3;
        }
      }
      else
      {
        // %110x xxxx 10xx xxxx
        if (i + 2 > byte_count || ((utf8[i+1] & 0xC0) != 0x80)) { break; }
        i += 2;
      }
    }
    else
    {
      ++i;
    }
  }

  //if (i != byte_count)
  //{
  //  ROGUE_LOG_ERROR( "*** RogueString validation error - invalid UTF8 (%d/%d):\n", i, byte_count );
  //  ROGUE_LOG_ERROR( "%02x\n", utf8[0] );
  //  ROGUE_LOG_ERROR( "%s\n", utf8 );
  //  utf8[ i ] = 0;
  //  Rogue_print_stack_trace();
  //}

  THIS->byte_count = i;
  THIS->character_count = character_count;

  int code = 0;
  int len = THIS->byte_count;
  char* src = THIS->utf8 - 1;
  while (--len >= 0)
  {
    code = ((code<<3) - code) + *(++src);
  }
  THIS->hash_code = code;
  return THIS;
}

//-----------------------------------------------------------------------------
//  RogueArray
//-----------------------------------------------------------------------------
RogueArray* RogueArray_set( RogueArray* THIS, RogueInt32 dest_i1, RogueArray* src_array, RogueInt32 src_i1, RogueInt32 copy_count )
{
  int element_size;
  int dest_i2, src_i2;

  if ( !src_array || dest_i1 >= THIS->count ) return THIS;
  if (THIS->is_reference_array ^ src_array->is_reference_array) return THIS;

  if (copy_count == -1) src_i2 = src_array->count - 1;
  else                  src_i2 = (src_i1 + copy_count) - 1;

  if (dest_i1 < 0)
  {
    src_i1 -= dest_i1;
    dest_i1 = 0;
  }

  if (src_i1 < 0) src_i1 = 0;
  if (src_i2 >= src_array->count) src_i2 = src_array->count - 1;
  if (src_i1 > src_i2) return THIS;

  copy_count = (src_i2 - src_i1) + 1;
  dest_i2 = dest_i1 + (copy_count - 1);
  if (dest_i2 >= THIS->count)
  {
    dest_i2 = (THIS->count - 1);
    copy_count = (dest_i2 - dest_i1) + 1;
  }
  if ( !copy_count ) return THIS;


#if defined(ROGUE_ARC)
  if (THIS != src_array || dest_i1 >= src_i1 + copy_count || (src_i1 + copy_count) <= dest_i1 || dest_i1 < src_i1)
  {
    // no overlap
    RogueObject** src  = src_array->as_objects + src_i1 - 1;
    RogueObject** dest = THIS->as_objects + dest_i1 - 1;
    while (--copy_count >= 0)
    {
      RogueObject* src_obj, dest_obj;
      if ((src_obj = *(++src))) ROGUE_INCREF(src_obj);
      if ((dest_obj = *(++dest)) && !(ROGUE_DECREF(dest_obj)))
      {
        // TODO: delete dest_obj
        *dest = src_obj;
      }
    }
  }
  else
  {
    // Copying earlier data to later data; copy in reverse order to
    // avoid accidental overwriting
    if (dest_i1 > src_i1)  // if they're equal then we don't need to copy anything!
    {
      RogueObject** src  = src_array->as_objects + src_i2 + 1;
      RogueObject** dest = THIS->as_objects + dest_i2 + 1;
      while (--copy_count >= 0)
      {
        RogueObject* src_obj, dest_obj;
        if ((src_obj = *(--src))) ROGUE_INCREF(src_obj);
        if ((dest_obj = *(--dest)) && !(ROGUE_DECREF(dest_obj)))
        {
          // TODO: delete dest_obj
          *dest = src_obj;
        }
      }
    }
  }
  return THIS;
#endif

  element_size = THIS->element_size;
  RogueByte* src = src_array->as_bytes + src_i1 * element_size;
  RogueByte* dest = THIS->as_bytes + (dest_i1 * element_size);
  int copy_bytes = copy_count * element_size;

  if (src == dest) return THIS;

  if (src >= dest + copy_bytes || (src + copy_bytes) <= dest)
  {
    // Copy region does not overlap
    memcpy( dest, src, copy_count * element_size );
  }
  else
  {
    // Copy region overlaps
    memmove( dest, src, copy_count * element_size );
  }

  return THIS;
}

//-----------------------------------------------------------------------------
//  RogueAllocationPage
//-----------------------------------------------------------------------------
RogueAllocationPage* RogueAllocationPage_create( RogueAllocationPage* next_page )
{
  RogueAllocationPage* result = (RogueAllocationPage*) ROGUE_NEW_BYTES(sizeof(RogueAllocationPage));
  result->next_page = next_page;
  result->cursor = result->data;
  result->remaining = ROGUEMM_PAGE_SIZE;
  return result;
}

#if 0 // This is currently done statically.  Code likely to be removed.
RogueAllocationPage* RogueAllocationPage_delete( RogueAllocationPage* THIS )
{
  if (THIS) ROGUE_DEL_BYTES( THIS );
  return 0;
};
#endif

void* RogueAllocationPage_allocate( RogueAllocationPage* THIS, int size )
{
  // Round size up to multiple of 8.
  if (size > 0) size = (size + 7) & ~7;
  else          size = 8;

  if (size > THIS->remaining) return 0;

  //ROGUE_LOG( "Allocating %d bytes from page.\n", size );
  void* result = THIS->cursor;
  THIS->cursor += size;
  THIS->remaining -= size;
  ((RogueObject*)result)->reference_count = 0;

  //ROGUE_LOG( "%d / %d\n", ROGUEMM_PAGE_SIZE - remaining, ROGUEMM_PAGE_SIZE );
  return result;
}


//-----------------------------------------------------------------------------
//  RogueAllocator
//-----------------------------------------------------------------------------
#if 0 // This is currently done statically.  Code likely to be removed.
RogueAllocator* RogueAllocator_create()
{
  RogueAllocator* result = (RogueAllocator*) ROGUE_NEW_BYTES( sizeof(RogueAllocator) );

  memset( result, 0, sizeof(RogueAllocator) );

  return result;
}

RogueAllocator* RogueAllocator_delete( RogueAllocator* THIS )
{
  while (THIS->pages)
  {
    RogueAllocationPage* next_page = THIS->pages->next_page;
    RogueAllocationPage_delete( THIS->pages );
    THIS->pages = next_page;
  }
  return 0;
}
#endif

void* RogueAllocator_allocate( RogueAllocator* THIS, int size )
{
#if ROGUE_GC_MODE_AUTO_MT
#if ROGUE_MTGC_DEBUG
    ROGUE_MUTEX_LOCK(Rogue_mtgc_w_mutex);
    if (Rogue_mtgc_w == 2)
    {
      ROGUE_LOG_ERROR("ALLOC DURING GC!\n");
      exit(1);
    }
    ROGUE_MUTEX_UNLOCK(Rogue_mtgc_w_mutex);
#endif
#endif

#if ROGUE_GC_MODE_AUTO_ANY
  Rogue_collect_garbage();
#endif
  if (size > ROGUEMM_SMALL_ALLOCATION_SIZE_LIMIT)
  {
    ROGUE_GC_COUNT_BYTES(size);
    void * mem = ROGUE_NEW_BYTES(size);
#if ROGUE_GC_MODE_AUTO_ANY
    if (!mem)
    {
      // Try hard!
      Rogue_collect_garbage(true);
      mem = ROGUE_NEW_BYTES(size);
    }
#endif
    return mem;
  }

  ROGUE_GC_SOA_LOCK;

  size = (size > 0) ? (size + ROGUEMM_GRANULARITY_MASK) & ~ROGUEMM_GRANULARITY_MASK : ROGUEMM_GRANULARITY_SIZE;

  ROGUE_GC_COUNT_BYTES(size);

  int slot;
  ROGUE_DEF_LOCAL_REF(RogueObject*, obj, THIS->available_objects[(slot=(size>>ROGUEMM_GRANULARITY_BITS))]);

  if (obj)
  {
    //ROGUE_LOG( "found free object\n");
    THIS->available_objects[slot] = obj->next_object;
    ROGUE_GC_SOA_UNLOCK;
    return obj;
  }

  // No free objects for requested size.

  // Try allocating a new object from the current page.
  if (THIS->pages )
  {
    obj = (RogueObject*) RogueAllocationPage_allocate( THIS->pages, size );
    if (obj)
    {
      ROGUE_GC_SOA_UNLOCK;
      return obj;
    }

    // Not enough room on allocation page.  Allocate any smaller blocks
    // we're able to and then move on to a new page.
    int s = slot - 1;
    while (s >= 1)
    {
      obj = (RogueObject*) RogueAllocationPage_allocate( THIS->pages, s << ROGUEMM_GRANULARITY_BITS );
      if (obj)
      {
        //ROGUE_LOG( "free obj size %d\n", (s << ROGUEMM_GRANULARITY_BITS) );
        obj->next_object = THIS->available_objects[s];
        THIS->available_objects[s] = obj;
      }
      else
      {
        --s;
      }
    }
  }

  // New page; this will work for sure.
  THIS->pages = RogueAllocationPage_create( THIS->pages );
  void * r = RogueAllocationPage_allocate( THIS->pages, size );
  ROGUE_GC_SOA_UNLOCK;
  return r;
}

#if ROGUE_GC_MODE_BOEHM
void Rogue_Boehm_Finalizer( void* obj, void* data )
{
  RogueObject* o = (RogueObject*)obj;
  o->type->on_cleanup_fn(o);
}

RogueObject* RogueAllocator_allocate_object( RogueAllocator* THIS, RogueType* of_type, int size, int element_type_index )
{
  // We use the "off page" allocations here, which require that somewhere there's a pointer
  // to something within the first 256 bytes.  Since someone should always be holding a
  // reference to the absolute start of the allocation (a reference!), this should always
  // be true.
#if ROGUE_GC_MODE_BOEHM_TYPED
  RogueObject * obj;
  if (of_type->gc_alloc_type == ROGUE_GC_ALLOC_TYPE_TYPED)
  {
    obj = (RogueObject*)GC_malloc_explicitly_typed_ignore_off_page(of_type->object_size, of_type->gc_type_descr);
  }
  else if (of_type->gc_alloc_type == ROGUE_GC_ALLOC_TYPE_ATOMIC)
  {
    obj = (RogueObject*)GC_malloc_atomic_ignore_off_page( of_type->object_size );
    if (obj) memset( obj, 0, of_type->object_size );
  }
  else
  {
    obj = (RogueObject*)GC_malloc_ignore_off_page( of_type->object_size );
  }
  if (!obj)
  {
    Rogue_collect_garbage( true );
    obj = (RogueObject*)GC_MALLOC( of_type->object_size );
  }
  obj->object_size = of_type->object_size;
#else
  RogueObject * obj = (RogueObject*)GC_malloc_ignore_off_page( size );
  if (!obj)
  {
    Rogue_collect_garbage( true );
    obj = (RogueObject*)GC_MALLOC( size );
  }
  obj->object_size = size;
#endif

  ROGUE_GCDEBUG_STATEMENT( ROGUE_LOG( "Allocating " ) );
  ROGUE_GCDEBUG_STATEMENT( RogueType_print_name(of_type) );
  ROGUE_GCDEBUG_STATEMENT( ROGUE_LOG( " %p\n", (RogueObject*)obj ) );
  //ROGUE_GCDEBUG_STATEMENT( Rogue_print_stack_trace() );

#if ROGUE_GC_MODE_BOEHM_TYPED
  // In typed mode, we allocate the array object and the actual data independently so that
  // they can have different GC types.
  if (element_type_index != -1)
  {
    RogueType* el_type = &Rogue_types[element_type_index];
    int data_size = size - of_type->object_size;
    int elements = data_size / el_type->object_size;
    void * data;
    if (el_type->gc_alloc_type == ROGUE_GC_ALLOC_TYPE_TYPED)
    {
      data = GC_calloc_explicitly_typed(elements, el_type->object_size, el_type->gc_type_descr);
    }
    else if (of_type->gc_alloc_type == ROGUE_GC_ALLOC_TYPE_ATOMIC)
    {
      data = GC_malloc_atomic_ignore_off_page( data_size );
      if (data) memset( obj, 0, data_size );
    }
    else
    {
      data = GC_malloc_ignore_off_page( data_size );
    }
    ((RogueArray*)obj)->as_bytes = (RogueByte*)data;
    ROGUE_GCDEBUG_STATEMENT( ROGUE_LOG( "Allocating " ) );
    ROGUE_GCDEBUG_STATEMENT( ROGUE_LOG( "  Elements " ) );
    ROGUE_GCDEBUG_STATEMENT( RogueType_print_name(el_type) );
    ROGUE_GCDEBUG_STATEMENT( ROGUE_LOG( " %p\n", (RogueObject*)data ) );
  }
#endif

  if (of_type->on_cleanup_fn)
  {
    GC_REGISTER_FINALIZER_IGNORE_SELF( obj, Rogue_Boehm_Finalizer, 0, 0, 0 );
  }

  obj->type = of_type;

  return obj;
}
#else
RogueObject* RogueAllocator_allocate_object( RogueAllocator* THIS, RogueType* of_type, int size, int element_type_index )
{
  void * mem = RogueAllocator_allocate( THIS, size );
  memset( mem, 0, size );

  ROGUE_DEF_LOCAL_REF(RogueObject*, obj, (RogueObject*)mem);

  ROGUE_GCDEBUG_STATEMENT( ROGUE_LOG( "Allocating " ) );
  ROGUE_GCDEBUG_STATEMENT( RogueType_print_name(of_type) );
  ROGUE_GCDEBUG_STATEMENT( ROGUE_LOG( " %p\n", (RogueObject*)obj ) );
  //ROGUE_GCDEBUG_STATEMENT( Rogue_print_stack_trace() );

  obj->type = of_type;
  obj->object_size = size;

  ROGUE_MTGC_BARRIER; // Probably not necessary

  if (of_type->on_cleanup_fn)
  {
    ROGUE_LINKED_LIST_INSERT(THIS->objects_requiring_cleanup, obj, obj->next_object);
  }
  else
  {
    ROGUE_LINKED_LIST_INSERT(THIS->objects, obj, obj->next_object);
  }

  return obj;
}
#endif

void* RogueAllocator_free( RogueAllocator* THIS, void* data, int size )
{
  if (data)
  {
    ROGUE_GCDEBUG_STATEMENT(memset(data,0,size));
    if (size > ROGUEMM_SMALL_ALLOCATION_SIZE_LIMIT)
    {
      // When debugging GC, it can be very useful to log the object types of
      // freed objects.  When valgrind points out access to freed memory, you
      // can then see what it was.
      #if 0
      RogueObject* obj = (RogueObject*) data;
      ROGUE_LOG("DEL %i %p ", (int)pthread_self(), data);
      RogueType_print_name( obj-> type );
      ROGUE_LOG("\n");
      #endif
      ROGUE_DEL_BYTES( data );
    }
    else
    {
      // Return object to small allocation pool
      RogueObject* obj = (RogueObject*) data;
      int slot = (size + ROGUEMM_GRANULARITY_MASK) >> ROGUEMM_GRANULARITY_BITS;
      if (slot <= 0) slot = 1;
      obj->next_object = THIS->available_objects[slot];
      THIS->available_objects[slot] = obj;
    }
  }

  // Always returns null, allowing a pointer to be freed and assigned null in
  // a single step.
  return 0;
}


void RogueAllocator_free_objects( RogueAllocator* THIS )
{
  RogueObject* objects = THIS->objects;
  while (objects)
  {
    RogueObject* next_object = objects->next_object;
    RogueAllocator_free( THIS, objects, objects->object_size );
    objects = next_object;
  }

  THIS->objects = 0;
}

void RogueAllocator_free_all( )
{
  for (int i=0; i<Rogue_allocator_count; ++i)
  {
    RogueAllocator_free_objects( &Rogue_allocators[i] );
  }
}

void RogueAllocator_collect_garbage( RogueAllocator* THIS )
{
  // Global program objects have already been traced through.

  // Trace through all as-yet unreferenced objects that are manually retained.
  RogueObject* cur = THIS->objects;
  while (cur)
  {
    if (cur->object_size >= 0 && cur->reference_count > 0)
    {
      cur->type->trace_fn( cur );
    }
    cur = cur->next_object;
  }

  cur = THIS->objects_requiring_cleanup;
  while (cur)
  {
    if (cur->object_size >= 0 && cur->reference_count > 0)
    {
      cur->type->trace_fn( cur );
    }
    cur = cur->next_object;
  }

  // For any unreferenced objects requiring clean-up, we'll:
  //   1.  Reference them and move them to a separate short-term list.
  //   2.  Finish the regular GC.
  //   3.  Call on_cleanup() on each of them, which may create new
  //       objects (which is why we have to wait until after the GC).
  //   4.  Move them to the list of regular objects.
  cur = THIS->objects_requiring_cleanup;
  RogueObject* unreferenced_on_cleanup_objects = 0;
  RogueObject* survivors = 0;  // local var for speed
  while (cur)
  {
    RogueObject* next_object = cur->next_object;
    if (cur->object_size < 0)
    {
      // Referenced.
      cur->next_object = survivors;
      survivors = cur;
    }
    else
    {
      // Unreferenced - go ahead and trace it since we'll call on_cleanup
      // on it.
      cur->type->trace_fn( cur );
      cur->next_object = unreferenced_on_cleanup_objects;
      unreferenced_on_cleanup_objects = cur;
    }
    cur = next_object;
  }
  THIS->objects_requiring_cleanup = survivors;

  // All objects are in a state where a non-negative size means that the object is
  // due to be deleted.
  Rogue_on_gc_trace_finished.call();

  // Now that on_gc_trace_finished() has been called we can reset the "collected" status flag
  // on all objects requiring cleanup.
  cur = THIS->objects_requiring_cleanup;
  while (cur)
  {
    if (cur->object_size < 0) cur->object_size = ~cur->object_size;
    cur = cur->next_object;
  }

  // Reset or delete each general object
  cur = THIS->objects;
  THIS->objects = 0;
  survivors = 0;  // local var for speed

  while (cur)
  {
    RogueObject* next_object = cur->next_object;
    if (cur->object_size < 0)
    {
      cur->object_size = ~cur->object_size;
      cur->next_object = survivors;
      survivors = cur;
    }
    else
    {
      ROGUE_GCDEBUG_STATEMENT( ROGUE_LOG( "Freeing " ) );
      ROGUE_GCDEBUG_STATEMENT( RogueType_print_name(cur->type) );
      ROGUE_GCDEBUG_STATEMENT( ROGUE_LOG( " %p\n", cur ) );
      RogueAllocator_free( THIS, cur, cur->object_size );
    }
    cur = next_object;
  }

  THIS->objects = survivors;


  // Call on_cleanup() on unreferenced objects requiring cleanup
  // and move them to the general objects list so they'll be deleted
  // the next time they're unreferenced.  Calling on_cleanup() may
  // create additional objects so THIS->objects may change during a
  // on_cleanup() call.
  cur = unreferenced_on_cleanup_objects;
  while (cur)
  {
    RogueObject* next_object = cur->next_object;

    cur->type->on_cleanup_fn( cur );

    cur->object_size = ~cur->object_size;
    cur->next_object = THIS->objects;
    THIS->objects = cur;

    cur = next_object;
  }

  if (Rogue_gc_logging)
  {
    int byte_count = 0;
    int object_count = 0;

    for (int i=0; i<Rogue_allocator_count; ++i)
    {
      RogueAllocator* allocator = &Rogue_allocators[i];

      RogueObject* cur = allocator->objects;
      while (cur)
      {
        ++object_count;
        byte_count += cur->object_size;
        cur = cur->next_object;
      }

      cur = allocator->objects_requiring_cleanup;
      while (cur)
      {
        ++object_count;
        byte_count += cur->object_size;
        cur = cur->next_object;
      }
    }

    ROGUE_LOG( "Post-GC: %d objects, %d bytes used.\n", object_count, byte_count );
  }
}

void Rogue_print_stack_trace ( bool leading_newline )
{
  RogueDebugTrace* current = Rogue_call_stack;
  if (current && leading_newline) ROGUE_LOG( "\n" );
  while (current)
  {
    ROGUE_LOG( "%s\n", current->to_c_string() );
    current = current->previous_trace;
  }
  ROGUE_LOG("\n");
}

#if defined(ROGUE_PLATFORM_WINDOWS)
void Rogue_segfault_handler( int signal )
{
  ROGUE_LOG_ERROR( "Access violation\n" );
#else
void Rogue_segfault_handler( int signal, siginfo_t *si, void *arg )
{
  if (si->si_addr < (void*)4096)
  {
    // Probably a null pointer dereference.
    ROGUE_LOG_ERROR( "Null reference error (accessing memory at %p)\n",
            si->si_addr );
  }
  else
  {
    if (si->si_code == SEGV_MAPERR)
      ROGUE_LOG_ERROR( "Access to unmapped memory at " );
    else if (si->si_code == SEGV_ACCERR)
      ROGUE_LOG_ERROR( "Access to forbidden memory at " );
    else
      ROGUE_LOG_ERROR( "Unknown segfault accessing " );
    ROGUE_LOG_ERROR("%p\n", si->si_addr);
  }
#endif

  Rogue_print_stack_trace( true );

  exit(1);
}

void Rogue_update_weak_references_during_gc()
{
  RogueWeakReference* cur = Rogue_weak_references;
  while (cur)
  {
    if (cur->value && cur->value->object_size >= 0)
    {
      // The value held by this weak reference is about to be deleted by the
      // GC system; null out the value.
      cur->value = 0;
    }
    cur = cur->next_weak_reference;
  }
}


void Rogue_configure_types()
{
#if ROGUE_THREAD_MODE == ROGUE_THREAD_MODE_PTHREADS
_rogue_init_mutex(&Rogue_thread_singleton_lock);
#endif

  int i;
  const int* next_type_info = Rogue_type_info_table;

#if defined(ROGUE_PLATFORM_WINDOWS)
  // Use plain old signal() instead of sigaction()
  signal( SIGSEGV, Rogue_segfault_handler );
#else
  // Install seg fault handler
  struct sigaction sa;

  memset( &sa, 0, sizeof(sa) );
  sigemptyset( &sa.sa_mask );
  sa.sa_sigaction = Rogue_segfault_handler;
  sa.sa_flags     = SA_SIGINFO;

  sigaction( SIGSEGV, &sa, NULL );
#endif

  // Initialize allocators
  memset( Rogue_allocators, 0, sizeof(RogueAllocator)*Rogue_allocator_count );

#ifdef ROGUE_INTROSPECTION
  int global_property_pointer_cursor = 0;
  int property_offset_cursor = 0;
#endif

  // Initialize types
  for (i=0; i<Rogue_type_count; ++i)
  {
    int j;
    RogueType* type = &Rogue_types[i];
    const int* type_info = next_type_info;
    next_type_info += *(type_info++) + 1;

    memset( type, 0, sizeof(RogueType) );

    type->index = i;
    type->name_index = Rogue_type_name_index_table[i];
    type->object_size = Rogue_object_size_table[i];
#ifdef ROGUE_INTROSPECTION
    type->attributes = Rogue_attributes_table[i];
#endif
    type->allocator = &Rogue_allocators[ *(type_info++) ];
    type->methods = Rogue_dynamic_method_table + *(type_info++);
    type->base_type_count = *(type_info++);
    if (type->base_type_count)
    {
#if ROGUE_GC_MODE_BOEHM
      type->base_types = new (NoGC) RogueType*[ type->base_type_count ];
#else
      type->base_types = new RogueType*[ type->base_type_count ];
#endif
      for (j=0; j<type->base_type_count; ++j)
      {
        type->base_types[j] = &Rogue_types[ *(type_info++) ];
      }
    }

    type->global_property_count = *(type_info++);
    type->global_property_name_indices = type_info;
    type_info += type->global_property_count;
    type->global_property_type_indices = type_info;
    type_info += type->global_property_count;

    type->property_count = *(type_info++);
    type->property_name_indices = type_info;
    type_info += type->property_count;
    type->property_type_indices = type_info;
    type_info += type->property_count;

#if ROGUE_GC_MODE_BOEHM_TYPED
    type->gc_alloc_type = *(type_info++);
#endif

#ifdef ROGUE_INTROSPECTION
    if (((type->attributes & ROGUE_ATTRIBUTE_TYPE_MASK) == ROGUE_ATTRIBUTE_IS_CLASS)
      || ((type->attributes & ROGUE_ATTRIBUTE_TYPE_MASK) == ROGUE_ATTRIBUTE_IS_COMPOUND))
    {
      type->global_property_pointers = Rogue_global_property_pointers + global_property_pointer_cursor;
      global_property_pointer_cursor += type->global_property_count;
      type->property_offsets = Rogue_property_offsets + property_offset_cursor;
      property_offset_cursor += type->property_count;
    }
#endif
    type->method_count = *(type_info++);
    type->global_method_count = *(type_info++);

    type->trace_fn = Rogue_trace_fn_table[i];
    type->init_object_fn = Rogue_init_object_fn_table[i];
    type->init_fn        = Rogue_init_fn_table[i];
    type->on_cleanup_fn  = Rogue_on_cleanup_fn_table[i];
    type->to_string_fn   = Rogue_to_string_fn_table[i];

    ROGUE_DEBUG_STATEMENT(assert(type_info <= next_type_info));
  }

  Rogue_on_gc_trace_finished.add( Rogue_update_weak_references_during_gc );

#if ROGUE_GC_MODE_BOEHM_TYPED
  Rogue_init_boehm_type_info();
#endif
}

#if ROGUE_GC_MODE_BOEHM
static GC_ToggleRefStatus Rogue_Boehm_ToggleRefStatus( void * o )
{
  RogueObject* obj = (RogueObject*)o;
  if (obj->reference_count > 0) return GC_TOGGLE_REF_STRONG;
  return GC_TOGGLE_REF_DROP;
}

static void Rogue_Boehm_on_collection_event( GC_EventType event )
{
  if (event == GC_EVENT_START)
  {
    Rogue_on_gc_begin.call();
  }
  else if (event == GC_EVENT_END)
  {
    Rogue_on_gc_end.call();
  }
}

void Rogue_configure_gc()
{
  // Initialize Boehm collector
  //GC_set_finalize_on_demand(0);
  GC_set_toggleref_func(Rogue_Boehm_ToggleRefStatus);
  GC_set_on_collection_event(Rogue_Boehm_on_collection_event);
  //GC_set_all_interior_pointers(0);
  GC_INIT();
}
#elif ROGUE_GC_MODE_AUTO_MT
// Rogue_configure_gc already defined above.
#else
void Rogue_configure_gc()
{
}
#endif

#if ROGUE_GC_MODE_BOEHM
bool Rogue_collect_garbage( bool forced )
{
  if (forced)
  {
    GC_gcollect();
    return true;
  }

  return GC_collect_a_little();
}
#else // Auto or manual

static inline void Rogue_collect_garbage_real(void);

bool Rogue_collect_garbage( bool forced )
{
  if (!forced && !Rogue_gc_requested & !ROGUE_GC_AT_THRESHOLD) return false;

#if ROGUE_GC_MODE_AUTO_MT
  Rogue_mtgc_run_gc_and_wait();
#else
  Rogue_collect_garbage_real();
#endif

  return true;
}

static inline void Rogue_collect_garbage_real()
{
  Rogue_gc_requested = false;
  if (Rogue_gc_active) return;
  Rogue_gc_active = true;
  ++ Rogue_gc_count;

//ROGUE_LOG( "GC %d\n", Rogue_allocation_bytes_until_gc );
  ROGUE_GC_RESET_COUNT;

  Rogue_on_gc_begin.call();

  Rogue_trace();

  for (int i=0; i<Rogue_allocator_count; ++i)
  {
    RogueAllocator_collect_garbage( &Rogue_allocators[i] );
  }

  Rogue_on_gc_end.call();
  Rogue_gc_active = false;
}

#endif

void Rogue_quit()
{
  int i;

  if ( !Rogue_configured ) return;
  Rogue_configured = 0;

  RogueGlobal__call_exit_functions( (RogueClassGlobal*) ROGUE_SINGLETON(Global) );

  ROGUE_THREADS_WAIT_FOR_ALL;

#if ROGUE_GC_MODE_AUTO_MT
  Rogue_mtgc_quit_gc_thread();
#else
  // Give a few GC's to allow objects requiring clean-up to do so.
  Rogue_collect_garbage( true );
  Rogue_collect_garbage( true );
  Rogue_collect_garbage( true );

  RogueAllocator_free_all();
#endif

  for (i=0; i<Rogue_type_count; ++i)
  {
    RogueType_retire( &Rogue_types[i] );
  }

  Rogue_thread_unregister();

  Rogue_gc_logging = false;
  Rogue_gc_count = 0;
  Rogue_gc_requested = 0;
  Rogue_gc_active = 0;
  Rogue_call_stack = 0;
  Rogue_weak_references = 0;
}

#if ROGUE_GC_MODE_BOEHM

void Rogue_Boehm_IncRef (RogueObject* o)
{
  ++o->reference_count;
  if (o->reference_count == 1)
  {
    GC_toggleref_add(o, 1);
  }
}
void Rogue_Boehm_DecRef (RogueObject* o)
{
  --o->reference_count;
  if (o->reference_count < 0)
  {
    o->reference_count = 0;
  }
}
#endif


//-----------------------------------------------------------------------------
//  Exception handling
//-----------------------------------------------------------------------------
void Rogue_terminate_handler()
{
  ROGUE_LOG_ERROR( "Uncaught exception.\n" );
  exit(1);
}
//=============================================================================
        const RogueUInt32 Rogue_crc32_table[256] =
        {
          0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
          0xe963a535, 0x9e6495a3, 0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
          0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
          0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
          0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9,
          0xfa0f3d63, 0x8d080df5, 0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
          0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b, 0x35b5a8fa, 0x42b2986c,
          0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
          0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423,
          0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
          0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d, 0x76dc4190, 0x01db7106,
          0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
          0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d,
          0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
          0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
          0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
          0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7,
          0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
          0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa,
          0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
          0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
          0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
          0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84,
          0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
          0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
          0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
          0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e,
          0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
          0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55,
          0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
          0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28,
          0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
          0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
          0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
          0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
          0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
          0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69,
          0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
          0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
          0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
          0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693,
          0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
          0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
        };
      void Rogue_fwrite( const char* utf8, int byte_count, int out )
      {
        while (byte_count)
        {
          int n = (int) write( out, utf8, byte_count );
          if (n == -1) return;
          byte_count -= n;
        }
      }
      /*
       * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
       * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
       * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
       * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
       * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
       * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
       * OTHER DEALINGS IN THE SOFTWARE.
       */
      #define __STDC_WANT_LIB_EXT1__ 1

      #include <errno.h>
      #include <sys/stat.h>
      #include <time.h>

      #if defined(_WIN32) || defined(__WIN32__) || defined(_MSC_VER) ||              \
          defined(__MINGW32__)
      /* Win32, DOS, MSVC, MSVS */
      #include <direct.h>

      #define STRCLONE(STR) ((STR) ? _strdup(STR) : NULL)
      #define HAS_DEVICE(P)                                                          \
        ((((P)[0] >= 'A' && (P)[0] <= 'Z') || ((P)[0] >= 'a' && (P)[0] <= 'z')) &&   \
         (P)[1] == ':')
      #define FILESYSTEM_PREFIX_LEN(P) (HAS_DEVICE(P) ? 2 : 0)

      #else

      #include <unistd.h> // needed for symlink()
      #define STRCLONE(STR) ((STR) ? strdup(STR) : NULL)

      #endif

      #ifdef __MINGW32__
      #include <sys/types.h>
      #include <unistd.h>
      #endif

      //#include "miniz.h"
      //#include "zip.h"

      #ifdef _MSC_VER
      #include <io.h>

      #define ftruncate(fd, sz) (-(_chsize_s((fd), (sz)) != 0))
      #define fileno _fileno
      #endif

      #ifndef HAS_DEVICE
      #define HAS_DEVICE(P) 0
      #endif

      #ifndef FILESYSTEM_PREFIX_LEN
      #define FILESYSTEM_PREFIX_LEN(P) 0
      #endif

      #ifndef ISSLASH
      #define ISSLASH(C) ((C) == '/' || (C) == '\\')
      #endif

      #define CLEANUP(ptr)                                                           \
        do {                                                                         \
          if (ptr) {                                                                 \
            free((void *)ptr);                                                       \
            ptr = NULL;                                                              \
          }                                                                          \
        } while (0)

      struct zip_entry_t {
        int index;
        char *name;
        mz_uint64 uncomp_size;
        mz_uint64 comp_size;
        mz_uint32 uncomp_crc32;
        mz_uint64 offset;
        mz_uint8 header[MZ_ZIP_LOCAL_DIR_HEADER_SIZE];
        mz_uint64 header_offset;
        mz_uint16 method;
        mz_zip_writer_add_state state;
        tdefl_compressor comp;
        mz_uint32 external_attr;
        time_t m_time;
      };

      struct zip_t {
        mz_zip_archive archive;
        mz_uint level;
        struct zip_entry_t entry;
      };

      enum zip_modify_t {
        MZ_KEEP = 0,
        MZ_DELETE = 1,
        MZ_MOVE = 2,
      };

      struct zip_entry_mark_t {
        int file_index;
        enum zip_modify_t type;
        mz_uint64 m_local_header_ofs;
        size_t lf_length;
      };

      static const char *const zip_errlist[30] = {
          NULL,
          "not initialized\0",
          "invalid entry name\0",
          "entry not found\0",
          "invalid zip mode\0",
          "invalid compression level\0",
          "no zip 64 support\0",
          "memset error\0",
          "cannot write data to entry\0",
          "cannot initialize tdefl compressor\0",
          "invalid index\0",
          "header not found\0",
          "cannot flush tdefl buffer\0",
          "cannot write entry header\0",
          "cannot create entry header\0",
          "cannot write to central dir\0",
          "cannot open file\0",
          "invalid entry type\0",
          "extracting data using no memory allocation\0",
          "file not found\0",
          "no permission\0",
          "out of memory\0",
          "invalid zip archive name\0",
          "make dir error\0",
          "symlink error\0",
          "close archive error\0",
          "capacity size too small\0",
          "fseek error\0",
          "fread error\0",
          "fwrite error\0",
      };

      const char *zip_strerror(int errnum) {
        errnum = -errnum;
        if (errnum <= 0 || errnum >= 30) {
          return NULL;
        }

        return zip_errlist[errnum];
      }

      static const char *zip_basename(const char *name) {
        char const *p;
        char const *base = name += FILESYSTEM_PREFIX_LEN(name);
        int all_slashes = 1;

        for (p = name; *p; p++) {
          if (ISSLASH(*p))
            base = p + 1;
          else
            all_slashes = 0;
        }

        /* If NAME is all slashes, arrange to return `/'. */
        if (*base == '\0' && ISSLASH(*name) && all_slashes)
          --base;

        return base;
      }

      static int zip_mkpath(char *path) {
        char *p;
        char npath[MAX_PATH + 1];
        int len = 0;
        int has_device = HAS_DEVICE(path);

        memset(npath, 0, MAX_PATH + 1);
        if (has_device) {
          // only on windows
          npath[0] = path[0];
          npath[1] = path[1];
          len = 2;
        }
        for (p = path + len; *p && len < MAX_PATH; p++) {
          if (ISSLASH(*p) && ((!has_device && len > 0) || (has_device && len > 2))) {
      #if defined(_WIN32) || defined(__WIN32__) || defined(_MSC_VER) ||              \
          defined(__MINGW32__)
      #else
            if ('\\' == *p) {
              *p = '/';
            }
      #endif

            if (MZ_MKDIR(npath) == -1) {
              if (errno != EEXIST) {
                return ZIP_EMKDIR;
              }
            }
          }
          npath[len++] = *p;
        }

        return 0;
      }

      static char *zip_strrpl(const char *str, size_t n, char oldchar, char newchar) {
        char c;
        size_t i;
        char *rpl = (char *)calloc((1 + n), sizeof(char));
        char *begin = rpl;
        if (!rpl) {
          return NULL;
        }

        for (i = 0; (i < n) && (c = *str++); ++i) {
          if (c == oldchar) {
            c = newchar;
          }
          *rpl++ = c;
        }

        return begin;
      }

      static char *zip_name_normalize(char *name, char *const nname, size_t len) {
        size_t offn = 0;
        size_t offnn = 0, ncpy = 0;

        if (name == NULL || nname == NULL || len <= 0) {
          return NULL;
        }
        // skip trailing '/'
        while (ISSLASH(*name))
          name++;

        for (; offn < len; offn++) {
          if (ISSLASH(name[offn])) {
            if (ncpy > 0 && strcmp(&nname[offnn], ".\0") &&
                strcmp(&nname[offnn], "..\0")) {
              offnn += ncpy;
              nname[offnn++] = name[offn]; // append '/'
            }
            ncpy = 0;
          } else {
            nname[offnn + ncpy] = name[offn];
            ncpy++;
          }
        }

        // at the end, extra check what we've already copied
        if (ncpy == 0 || !strcmp(&nname[offnn], ".\0") ||
            !strcmp(&nname[offnn], "..\0")) {
          nname[offnn] = 0;
        }
        return nname;
      }

      static mz_bool zip_name_match(const char *name1, const char *name2) {
        size_t len2 = strlen(name2);
        char *nname2 = zip_strrpl(name2, len2, '\\', '/');
        if (!nname2) {
          return MZ_FALSE;
        }

        mz_bool res = (strcmp(name1, nname2) == 0) ? MZ_TRUE : MZ_FALSE;
        CLEANUP(nname2);
        return res;
      }

      static int zip_archive_truncate(mz_zip_archive *pzip) {
        mz_zip_internal_state *pState = pzip->m_pState;
        mz_uint64 file_size = pzip->m_archive_size;
        if ((pzip->m_pWrite == mz_zip_heap_write_func) && (pState->m_pMem)) {
          return 0;
        }
        if (pzip->m_zip_mode == MZ_ZIP_MODE_WRITING_HAS_BEEN_FINALIZED) {
          if (pState->m_pFile) {
            int fd = fileno(pState->m_pFile);
            return ftruncate(fd, file_size);
          }
        }
        return 0;
      }

      static int zip_archive_extract(mz_zip_archive *zip_archive, const char *dir,
                                     int (*on_extract)(const char *filename,
                                                       void *arg),
                                     void *arg) {
        int err = 0;
        mz_uint i, n;
        char path[MAX_PATH + 1];
        char symlink_to[MAX_PATH + 1];
        mz_zip_archive_file_stat info;
        size_t dirlen = 0;
        mz_uint32 xattr = 0;

        memset(path, 0, sizeof(path));
        memset(symlink_to, 0, sizeof(symlink_to));

        dirlen = strlen(dir);
        if (dirlen + 1 > MAX_PATH) {
          return ZIP_EINVENTNAME;
        }

        memset((void *)&info, 0, sizeof(mz_zip_archive_file_stat));

      #if defined(_MSC_VER)
        strcpy_s(path, MAX_PATH, dir);
      #else
        strcpy(path, dir);
      #endif

        if (!ISSLASH(path[dirlen - 1])) {
      #if defined(_WIN32) || defined(__WIN32__)
          path[dirlen] = '\\';
      #else
          path[dirlen] = '/';
      #endif
          ++dirlen;
        }

        // Get and print information about each file in the archive.
        n = mz_zip_reader_get_num_files(zip_archive);
        for (i = 0; i < n; ++i) {
          if (!mz_zip_reader_file_stat(zip_archive, i, &info)) {
            // Cannot get information about zip archive;
            err = ZIP_ENOENT;
            goto out;
          }

          if (!zip_name_normalize(info.m_filename, info.m_filename,
                                  strlen(info.m_filename))) {
            // Cannot normalize file name;
            err = ZIP_EINVENTNAME;
            goto out;
          }
      #if defined(_MSC_VER)
          strncpy_s(&path[dirlen], MAX_PATH - dirlen, info.m_filename,
                    MAX_PATH - dirlen);
      #else
          strncpy(&path[dirlen], info.m_filename, MAX_PATH - dirlen);
      #endif
          err = zip_mkpath(path);
          if (err < 0) {
            // Cannot make a path
            goto out;
          }

          if ((((info.m_version_made_by >> 8) == 3) ||
               ((info.m_version_made_by >> 8) ==
                19)) // if zip is produced on Unix or macOS (3 and 19 from
                     // section 4.4.2.2 of zip standard)
              && info.m_external_attr &
                     (0x20 << 24)) { // and has sym link attribute (0x80 is file, 0x40
                                     // is directory)
      #if defined(_WIN32) || defined(__WIN32__) || defined(_MSC_VER) ||              \
          defined(__MINGW32__)
      #else
            if (info.m_uncomp_size > MAX_PATH ||
                !mz_zip_reader_extract_to_mem_no_alloc(zip_archive, i, symlink_to,
                                                       MAX_PATH, 0, NULL, 0)) {
              err = ZIP_EMEMNOALLOC;
              goto out;
            }
            symlink_to[info.m_uncomp_size] = '\0';
            if (symlink(symlink_to, path) != 0) {
              err = ZIP_ESYMLINK;
              goto out;
            }
      #endif
          } else {
            if (!mz_zip_reader_is_file_a_directory(zip_archive, i)) {
              if (!mz_zip_reader_extract_to_file(zip_archive, i, path, 0)) {
                // Cannot extract zip archive to file
                err = ZIP_ENOFILE;
                goto out;
              }
            }

      #if defined(_MSC_VER)
            (void)xattr; // unused
      #else
            xattr = (info.m_external_attr >> 16) & 0xFFFF;
            if (xattr > 0) {
              if (chmod(path, (mode_t)xattr) < 0) {
                err = ZIP_ENOPERM;
                goto out;
              }
            }
      #endif
          }

          if (on_extract) {
            if (on_extract(path, arg) < 0) {
              goto out;
            }
          }
        }

      out:
        // Close the archive, freeing any resources it was using
        if (!mz_zip_reader_end(zip_archive)) {
          // Cannot end zip reader
          err = ZIP_ECLSZIP;
        }
        return err;
      }

      static inline void zip_archive_finalize(mz_zip_archive *pzip) {
        mz_zip_writer_finalize_archive(pzip);
        zip_archive_truncate(pzip);
      }

      static ssize_t zip_entry_mark(struct zip_t *zip,
                                    struct zip_entry_mark_t *entry_mark, int n,
                                    char *const entries[], const size_t len) {
        int i = 0;
        ssize_t err = 0;
        if (!zip || !entry_mark || !entries) {
          return ZIP_ENOINIT;
        }

        mz_zip_archive_file_stat file_stat;
        mz_uint64 d_pos = ~0UL;
        for (i = 0; i < n; ++i) {
          if ((err = zip_entry_openbyindex(zip, i))) {
            return (ssize_t)err;
          }

          mz_bool name_matches = MZ_FALSE;
          for (int j = 0; j < (const int)len; ++j) {
            if (zip_name_match(zip->entry.name, entries[j])) {
              name_matches = MZ_TRUE;
              break;
            }
          }
          if (name_matches) {
            entry_mark[i].type = MZ_DELETE;
          } else {
            entry_mark[i].type = MZ_KEEP;
          }

          if (!mz_zip_reader_file_stat(&zip->archive, i, &file_stat)) {
            return ZIP_ENOENT;
          }

          zip_entry_close(zip);

          entry_mark[i].m_local_header_ofs = file_stat.m_local_header_ofs;
          entry_mark[i].file_index = -1;
          entry_mark[i].lf_length = 0;
          if ((entry_mark[i].type) == MZ_DELETE &&
              (d_pos > entry_mark[i].m_local_header_ofs)) {
            d_pos = entry_mark[i].m_local_header_ofs;
          }
        }

        for (i = 0; i < n; ++i) {
          if ((entry_mark[i].m_local_header_ofs > d_pos) &&
              (entry_mark[i].type != MZ_DELETE)) {
            entry_mark[i].type = MZ_MOVE;
          }
        }
        return err;
      }

      static int zip_index_next(mz_uint64 *local_header_ofs_array, int cur_index) {
        int new_index = 0;
        for (int i = cur_index - 1; i >= 0; --i) {
          if (local_header_ofs_array[cur_index] > local_header_ofs_array[i]) {
            new_index = i + 1;
            return new_index;
          }
        }
        return new_index;
      }

      static int zip_sort(mz_uint64 *local_header_ofs_array, int cur_index) {
        int nxt_index = zip_index_next(local_header_ofs_array, cur_index);

        if (nxt_index != cur_index) {
          mz_uint64 temp = local_header_ofs_array[cur_index];
          for (int i = cur_index; i > nxt_index; i--) {
            local_header_ofs_array[i] = local_header_ofs_array[i - 1];
          }
          local_header_ofs_array[nxt_index] = temp;
        }
        return nxt_index;
      }

      static int zip_index_update(struct zip_entry_mark_t *entry_mark, int last_index,
                                  int nxt_index) {
        for (int j = 0; j < last_index; j++) {
          if (entry_mark[j].file_index >= nxt_index) {
            entry_mark[j].file_index += 1;
          }
        }
        entry_mark[nxt_index].file_index = last_index;
        return 0;
      }

      static int zip_entry_finalize(struct zip_t *zip,
                                    struct zip_entry_mark_t *entry_mark,
                                    const int n) {

        int i = 0;
        mz_uint64 *local_header_ofs_array = (mz_uint64 *)calloc(n, sizeof(mz_uint64));
        if (!local_header_ofs_array) {
          return ZIP_EOOMEM;
        }

        for (i = 0; i < n; ++i) {
          local_header_ofs_array[i] = entry_mark[i].m_local_header_ofs;
          int index = zip_sort(local_header_ofs_array, i);

          if (index != i) {
            zip_index_update(entry_mark, i, index);
          }
          entry_mark[i].file_index = index;
        }

        size_t *length = (size_t *)calloc(n, sizeof(size_t));
        if (!length) {
          CLEANUP(local_header_ofs_array);
          return ZIP_EOOMEM;
        }
        for (i = 0; i < n - 1; i++) {
          length[i] =
              (size_t)(local_header_ofs_array[i + 1] - local_header_ofs_array[i]);
        }
        length[n - 1] =
            (size_t)(zip->archive.m_archive_size - local_header_ofs_array[n - 1]);

        for (i = 0; i < n; i++) {
          entry_mark[i].lf_length = length[entry_mark[i].file_index];
        }

        CLEANUP(length);
        CLEANUP(local_header_ofs_array);
        return 0;
      }

      static ssize_t zip_entry_set(struct zip_t *zip,
                                   struct zip_entry_mark_t *entry_mark, int n,
                                   char *const entries[], const size_t len) {
        ssize_t err = 0;

        if ((err = zip_entry_mark(zip, entry_mark, n, entries, len)) < 0) {
          return err;
        }
        if ((err = zip_entry_finalize(zip, entry_mark, n)) < 0) {
          return err;
        }
        return 0;
      }

      static ssize_t zip_file_move(MZ_FILE *m_pFile, const mz_uint64 to,
                                   const mz_uint64 from, const size_t length,
                                   mz_uint8 *move_buf, const size_t capacity_size) {
        if (length > capacity_size) {
          return ZIP_ECAPSIZE;
        }
        if (MZ_FSEEK64(m_pFile, from, SEEK_SET)) {
          MZ_FCLOSE(m_pFile);
          return ZIP_EFSEEK;
        }

        if (fread(move_buf, 1, length, m_pFile) != length) {
          MZ_FCLOSE(m_pFile);
          return ZIP_EFREAD;
        }
        if (MZ_FSEEK64(m_pFile, to, SEEK_SET)) {
          MZ_FCLOSE(m_pFile);
          return ZIP_EFSEEK;
        }
        if (fwrite(move_buf, 1, length, m_pFile) != length) {
          MZ_FCLOSE(m_pFile);
          return ZIP_EFWRITE;
        }
        return (ssize_t)length;
      }

      static ssize_t zip_files_move(MZ_FILE *m_pFile, mz_uint64 writen_num,
                                    mz_uint64 read_num, size_t length) {
        ssize_t n = 0;
        const size_t page_size = 1 << 12; // 4K
        mz_uint8 *move_buf = (mz_uint8 *)calloc(1, page_size);
        if (!move_buf) {
          return ZIP_EOOMEM;
        }

        ssize_t moved_length = 0;
        ssize_t move_count = 0;
        while ((mz_int64)length > 0) {
          move_count = (length >= page_size) ? page_size : length;
          n = zip_file_move(m_pFile, writen_num, read_num, move_count, move_buf,
                            page_size);
          if (n < 0) {
            moved_length = n;
            goto cleanup;
          }

          if (n != move_count) {
            goto cleanup;
          }

          writen_num += move_count;
          read_num += move_count;
          length -= move_count;
          moved_length += move_count;
        }

      cleanup:
        CLEANUP(move_buf);
        return moved_length;
      }

      static int zip_central_dir_move(mz_zip_internal_state *pState, int begin,
                                      int end, int entry_num) {
        if (begin == entry_num) {
          return 0;
        }

        size_t l_size = 0;
        size_t r_size = 0;
        mz_uint32 d_size = 0;
        mz_uint8 *next = NULL;
        mz_uint8 *deleted = &MZ_ZIP_ARRAY_ELEMENT(
            &pState->m_central_dir, mz_uint8,
            MZ_ZIP_ARRAY_ELEMENT(&pState->m_central_dir_offsets, mz_uint32, begin));
        l_size = (size_t)(deleted - (mz_uint8 *)(pState->m_central_dir.m_p));
        if (end == entry_num) {
          r_size = 0;
        } else {
          next = &MZ_ZIP_ARRAY_ELEMENT(
              &pState->m_central_dir, mz_uint8,
              MZ_ZIP_ARRAY_ELEMENT(&pState->m_central_dir_offsets, mz_uint32, end));
          r_size = pState->m_central_dir.m_size -
                   (mz_uint32)(next - (mz_uint8 *)(pState->m_central_dir.m_p));
          d_size = (mz_uint32)(next - deleted);
        }

        if (l_size == 0) {
          memmove(pState->m_central_dir.m_p, next, r_size);
          pState->m_central_dir.m_p = MZ_REALLOC(pState->m_central_dir.m_p, r_size);
          for (int i = end; i < entry_num; i++) {
            MZ_ZIP_ARRAY_ELEMENT(&pState->m_central_dir_offsets, mz_uint32, i) -=
                d_size;
          }
        }

        if (l_size * r_size != 0) {
          memmove(deleted, next, r_size);
          for (int i = end; i < entry_num; i++) {
            MZ_ZIP_ARRAY_ELEMENT(&pState->m_central_dir_offsets, mz_uint32, i) -=
                d_size;
          }
        }

        pState->m_central_dir.m_size = l_size + r_size;
        return 0;
      }

      static int zip_central_dir_delete(mz_zip_internal_state *pState,
                                        int *deleted_entry_index_array,
                                        int entry_num) {
        int i = 0;
        int begin = 0;
        int end = 0;
        int d_num = 0;
        while (i < entry_num) {
          while ((!deleted_entry_index_array[i]) && (i < entry_num)) {
            i++;
          }
          begin = i;

          while ((deleted_entry_index_array[i]) && (i < entry_num)) {
            i++;
          }
          end = i;
          zip_central_dir_move(pState, begin, end, entry_num);
        }

        i = 0;
        while (i < entry_num) {
          while ((!deleted_entry_index_array[i]) && (i < entry_num)) {
            i++;
          }
          begin = i;
          if (begin == entry_num) {
            break;
          }
          while ((deleted_entry_index_array[i]) && (i < entry_num)) {
            i++;
          }
          end = i;
          int k = 0;
          for (int j = end; j < entry_num; j++) {
            MZ_ZIP_ARRAY_ELEMENT(&pState->m_central_dir_offsets, mz_uint32,
                                 begin + k) =
                (mz_uint32)MZ_ZIP_ARRAY_ELEMENT(&pState->m_central_dir_offsets,
                                                mz_uint32, j);
            k++;
          }
          d_num += end - begin;
        }

        pState->m_central_dir_offsets.m_size =
            sizeof(mz_uint32) * (entry_num - d_num);
        return 0;
      }

      static ssize_t zip_entries_delete_mark(struct zip_t *zip,
                                             struct zip_entry_mark_t *entry_mark,
                                             int entry_num) {
        mz_uint64 writen_num = 0;
        mz_uint64 read_num = 0;
        size_t deleted_length = 0;
        size_t move_length = 0;
        int i = 0;
        size_t deleted_entry_num = 0;
        ssize_t n = 0;

        mz_bool *deleted_entry_flag_array =
            (mz_bool *)calloc(entry_num, sizeof(mz_bool));
        if (deleted_entry_flag_array == NULL) {
          return ZIP_EOOMEM;
        }

        mz_zip_internal_state *pState = zip->archive.m_pState;
        zip->archive.m_zip_mode = MZ_ZIP_MODE_WRITING;

        if (MZ_FSEEK64(pState->m_pFile, 0, SEEK_SET)) {
          CLEANUP(deleted_entry_flag_array);
          return ZIP_ENOENT;
        }

        while (i < entry_num) {
          while ((entry_mark[i].type == MZ_KEEP) && (i < entry_num)) {
            writen_num += entry_mark[i].lf_length;
            read_num = writen_num;
            i++;
          }

          while ((entry_mark[i].type == MZ_DELETE) && (i < entry_num)) {
            deleted_entry_flag_array[i] = MZ_TRUE;
            read_num += entry_mark[i].lf_length;
            deleted_length += entry_mark[i].lf_length;
            i++;
            deleted_entry_num++;
          }

          while ((entry_mark[i].type == MZ_MOVE) && (i < entry_num)) {
            move_length += entry_mark[i].lf_length;
            mz_uint8 *p = &MZ_ZIP_ARRAY_ELEMENT(
                &pState->m_central_dir, mz_uint8,
                MZ_ZIP_ARRAY_ELEMENT(&pState->m_central_dir_offsets, mz_uint32, i));
            if (!p) {
              CLEANUP(deleted_entry_flag_array);
              return ZIP_ENOENT;
            }
            mz_uint32 offset = MZ_READ_LE32(p + MZ_ZIP_CDH_LOCAL_HEADER_OFS);
            offset -= (mz_uint32)deleted_length;
            MZ_WRITE_LE32(p + MZ_ZIP_CDH_LOCAL_HEADER_OFS, offset);
            i++;
          }

          n = zip_files_move(pState->m_pFile, writen_num, read_num, move_length);
          if (n != (ssize_t)move_length) {
            CLEANUP(deleted_entry_flag_array);
            return n;
          }
          writen_num += move_length;
          read_num += move_length;
        }

        zip->archive.m_archive_size -= (mz_uint64)deleted_length;
        zip->archive.m_total_files =
            (mz_uint32)entry_num - (mz_uint32)deleted_entry_num;

        zip_central_dir_delete(pState, deleted_entry_flag_array, entry_num);
        CLEANUP(deleted_entry_flag_array);

        return (ssize_t)deleted_entry_num;
      }

      struct zip_t *zip_open(const char *zipname, int level, char mode) {
        struct zip_t *zip = NULL;

        if (!zipname || strlen(zipname) < 1) {
          // zip_t archive name is empty or NULL
          goto cleanup;
        }

        if (level < 0)
          level = MZ_DEFAULT_LEVEL;
        if ((level & 0xF) > MZ_UBER_COMPRESSION) {
          // Wrong compression level
          goto cleanup;
        }

        zip = (struct zip_t *)calloc((size_t)1, sizeof(struct zip_t));
        if (!zip)
          goto cleanup;

        zip->level = (mz_uint)level;
        switch (mode) {
        case 'w':
          // Create a new archive.
          if (!mz_zip_writer_init_file(&(zip->archive), zipname, 0)) {
            // Cannot initialize zip_archive writer
            goto cleanup;
          }
          break;

        case 'r':
        case 'a':
        case 'd':
          if (!mz_zip_reader_init_file(
                  &(zip->archive), zipname,
                  zip->level | MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY)) {
            // An archive file does not exist or cannot initialize
            // zip_archive reader
            goto cleanup;
          }
          if ((mode == 'a' || mode == 'd') &&
              !mz_zip_writer_init_from_reader(&(zip->archive), zipname)) {
            mz_zip_reader_end(&(zip->archive));
            goto cleanup;
          }
          break;

        default:
          goto cleanup;
        }

        return zip;

      cleanup:
        CLEANUP(zip);
        return NULL;
      }

      void zip_close(struct zip_t *zip) {
        if (zip) {
          // Always finalize, even if adding failed for some reason, so we have a
          // valid central directory.
          mz_zip_writer_finalize_archive(&(zip->archive));
          zip_archive_truncate(&(zip->archive));
          mz_zip_writer_end(&(zip->archive));
          mz_zip_reader_end(&(zip->archive));

          CLEANUP(zip);
        }
      }

      int zip_is64(struct zip_t *zip) {
        if (!zip || !zip->archive.m_pState) {
          // zip_t handler or zip state is not initialized
          return ZIP_ENOINIT;
        }

        return (int)zip->archive.m_pState->m_zip64;
      }

      int zip_entry_open(struct zip_t *zip, const char *entryname) {
        size_t entrylen = 0;
        mz_zip_archive *pzip = NULL;
        mz_uint num_alignment_padding_bytes, level;
        mz_zip_archive_file_stat stats;
        int err = 0;

        if (!zip) {
          return ZIP_ENOINIT;
        }

        if (!entryname) {
          return ZIP_EINVENTNAME;
        }

        entrylen = strlen(entryname);
        if (entrylen == 0) {
          return ZIP_EINVENTNAME;
        }

        /*
          .ZIP File Format Specification Version: 6.3.3

          4.4.17.1 The name of the file, with optional relative path.
          The path stored MUST not contain a drive or
          device letter, or a leading slash.  All slashes
          MUST be forward slashes '/' as opposed to
          backwards slashes '\' for compatibility with Amiga
          and UNIX file systems etc.  If input came from standard
          input, there is no file name field.
        */
        if (zip->entry.name) {
          CLEANUP(zip->entry.name);
        }
        zip->entry.name = zip_strrpl(entryname, entrylen, '\\', '/');
        if (!zip->entry.name) {
          // Cannot parse zip entry name
          return ZIP_EINVENTNAME;
        }

        pzip = &(zip->archive);
        if (pzip->m_zip_mode == MZ_ZIP_MODE_READING) {
          zip->entry.index =
              mz_zip_reader_locate_file(pzip, zip->entry.name, NULL, 0);
          if (zip->entry.index < 0) {
            err = ZIP_ENOENT;
            goto cleanup;
          }

          if (!mz_zip_reader_file_stat(pzip, (mz_uint)zip->entry.index, &stats)) {
            err = ZIP_ENOENT;
            goto cleanup;
          }

          zip->entry.comp_size = stats.m_comp_size;
          zip->entry.uncomp_size = stats.m_uncomp_size;
          zip->entry.uncomp_crc32 = stats.m_crc32;
          zip->entry.offset = stats.m_central_dir_ofs;
          zip->entry.header_offset = stats.m_local_header_ofs;
          zip->entry.method = stats.m_method;
          zip->entry.external_attr = stats.m_external_attr;
      #ifndef MINIZ_NO_TIME
          zip->entry.m_time = stats.m_time;
      #endif

          return 0;
        }

        zip->entry.index = (int)zip->archive.m_total_files;
        zip->entry.comp_size = 0;
        zip->entry.uncomp_size = 0;
        zip->entry.uncomp_crc32 = MZ_CRC32_INIT;
        zip->entry.offset = zip->archive.m_archive_size;
        zip->entry.header_offset = zip->archive.m_archive_size;
        memset(zip->entry.header, 0, MZ_ZIP_LOCAL_DIR_HEADER_SIZE * sizeof(mz_uint8));
        zip->entry.method = 0;

        // UNIX or APPLE
      #if MZ_PLATFORM == 3 || MZ_PLATFORM == 19
        // regular file with rw-r--r-- persmissions
        zip->entry.external_attr = (mz_uint32)(0100644) << 16;
      #else
        zip->entry.external_attr = 0;
      #endif

        num_alignment_padding_bytes =
            mz_zip_writer_compute_padding_needed_for_file_alignment(pzip);

        if (!pzip->m_pState || (pzip->m_zip_mode != MZ_ZIP_MODE_WRITING)) {
          // Invalid zip mode
          err = ZIP_EINVMODE;
          goto cleanup;
        }
        if (zip->level & MZ_ZIP_FLAG_COMPRESSED_DATA) {
          // Invalid zip compression level
          err = ZIP_EINVLVL;
          goto cleanup;
        }
        // no zip64 support yet
        if ((pzip->m_total_files == 0xFFFF) ||
            ((pzip->m_archive_size + num_alignment_padding_bytes +
              MZ_ZIP_LOCAL_DIR_HEADER_SIZE + MZ_ZIP_CENTRAL_DIR_HEADER_SIZE +
              entrylen) > 0xFFFFFFFF)) {
          // No zip64 support yet
          err = ZIP_ENOSUP64;
          goto cleanup;
        }
        if (!mz_zip_writer_write_zeros(pzip, zip->entry.offset,
                                       num_alignment_padding_bytes +
                                           sizeof(zip->entry.header))) {
          // Cannot memset zip entry header
          err = ZIP_EMEMSET;
          goto cleanup;
        }

        zip->entry.header_offset += num_alignment_padding_bytes;
        if (pzip->m_file_offset_alignment) {
          MZ_ASSERT(
              (zip->entry.header_offset & (pzip->m_file_offset_alignment - 1)) == 0);
        }
        zip->entry.offset += num_alignment_padding_bytes + sizeof(zip->entry.header);

        if (pzip->m_pWrite(pzip->m_pIO_opaque, zip->entry.offset, zip->entry.name,
                           entrylen) != entrylen) {
          // Cannot write data to zip entry
          err = ZIP_EWRTENT;
          goto cleanup;
        }

        zip->entry.offset += entrylen;
        level = zip->level & 0xF;
        if (level) {
          zip->entry.state.m_pZip = pzip;
          zip->entry.state.m_cur_archive_file_ofs = zip->entry.offset;
          zip->entry.state.m_comp_size = 0;

          if (tdefl_init(&(zip->entry.comp), mz_zip_writer_add_put_buf_callback,
                         &(zip->entry.state),
                         (int)tdefl_create_comp_flags_from_zip_params(
                             (int)level, -15, MZ_DEFAULT_STRATEGY)) !=
              TDEFL_STATUS_OKAY) {
            // Cannot initialize the zip compressor
            err = ZIP_ETDEFLINIT;
            goto cleanup;
          }
        }

        zip->entry.m_time = time(NULL);

        return 0;

      cleanup:
        CLEANUP(zip->entry.name);
        return err;
      }

      int zip_entry_openbyindex(struct zip_t *zip, int index) {
        mz_zip_archive *pZip = NULL;
        mz_zip_archive_file_stat stats;
        mz_uint namelen;
        const mz_uint8 *pHeader;
        const char *pFilename;

        if (!zip) {
          // zip_t handler is not initialized
          return ZIP_ENOINIT;
        }

        pZip = &(zip->archive);
        if (pZip->m_zip_mode != MZ_ZIP_MODE_READING) {
          // open by index requires readonly mode
          return ZIP_EINVMODE;
        }

        if (index < 0 || (mz_uint)index >= pZip->m_total_files) {
          // index out of range
          return ZIP_EINVIDX;
        }

        if (!(pHeader = &MZ_ZIP_ARRAY_ELEMENT(
                  &pZip->m_pState->m_central_dir, mz_uint8,
                  MZ_ZIP_ARRAY_ELEMENT(&pZip->m_pState->m_central_dir_offsets,
                                       mz_uint32, index)))) {
          // cannot find header in central directory
          return ZIP_ENOHDR;
        }

        namelen = MZ_READ_LE16(pHeader + MZ_ZIP_CDH_FILENAME_LEN_OFS);
        pFilename = (const char *)pHeader + MZ_ZIP_CENTRAL_DIR_HEADER_SIZE;

        /*
          .ZIP File Format Specification Version: 6.3.3

          4.4.17.1 The name of the file, with optional relative path.
          The path stored MUST not contain a drive or
          device letter, or a leading slash.  All slashes
          MUST be forward slashes '/' as opposed to
          backwards slashes '\' for compatibility with Amiga
          and UNIX file systems etc.  If input came from standard
          input, there is no file name field.
        */
        if (zip->entry.name) {
          CLEANUP(zip->entry.name);
        }
        zip->entry.name = zip_strrpl(pFilename, namelen, '\\', '/');
        if (!zip->entry.name) {
          // local entry name is NULL
          return ZIP_EINVENTNAME;
        }

        if (!mz_zip_reader_file_stat(pZip, (mz_uint)index, &stats)) {
          return ZIP_ENOENT;
        }

        zip->entry.index = index;
        zip->entry.comp_size = stats.m_comp_size;
        zip->entry.uncomp_size = stats.m_uncomp_size;
        zip->entry.uncomp_crc32 = stats.m_crc32;
        zip->entry.offset = stats.m_central_dir_ofs;
        zip->entry.header_offset = stats.m_local_header_ofs;
        zip->entry.method = stats.m_method;
        zip->entry.external_attr = stats.m_external_attr;
      #ifndef MINIZ_NO_TIME
        zip->entry.m_time = stats.m_time;
      #endif

        return 0;
      }

      int zip_entry_close(struct zip_t *zip) {
        mz_zip_archive *pzip = NULL;
        mz_uint level;
        tdefl_status done;
        mz_uint16 entrylen;
        mz_uint16 dos_time = 0, dos_date = 0;
        int err = 0;
        if (!zip) {
          // zip_t handler is not initialized
          err = ZIP_ENOINIT;
          goto cleanup;
        }

        pzip = &(zip->archive);
        if (pzip->m_zip_mode == MZ_ZIP_MODE_READING) {
          goto cleanup;
        }

        level = zip->level & 0xF;
        if (level) {
          done = tdefl_compress_buffer(&(zip->entry.comp), "", 0, TDEFL_FINISH);
          if (done != TDEFL_STATUS_DONE && done != TDEFL_STATUS_OKAY) {
            // Cannot flush compressed buffer
            err = ZIP_ETDEFLBUF;
            goto cleanup;
          }
          zip->entry.comp_size = zip->entry.state.m_comp_size;
          zip->entry.offset = zip->entry.state.m_cur_archive_file_ofs;
          zip->entry.method = MZ_DEFLATED;
        }

        entrylen = (mz_uint16)strlen(zip->entry.name);
        if ((zip->entry.comp_size > 0xFFFFFFFF) || (zip->entry.offset > 0xFFFFFFFF)) {
          // No zip64 support, yet
          err = ZIP_ENOSUP64;
          goto cleanup;
        }

      #ifndef MINIZ_NO_TIME
        mz_zip_time_t_to_dos_time(zip->entry.m_time, &dos_time, &dos_date);
      #endif

        if (!mz_zip_writer_create_local_dir_header(
                pzip, zip->entry.header, entrylen, 0, zip->entry.uncomp_size,
                zip->entry.comp_size, zip->entry.uncomp_crc32, zip->entry.method, 0,
                dos_time, dos_date)) {
          // Cannot create zip entry header
          err = ZIP_ECRTHDR;
          goto cleanup;
        }

        if (pzip->m_pWrite(pzip->m_pIO_opaque, zip->entry.header_offset,
                           zip->entry.header,
                           sizeof(zip->entry.header)) != sizeof(zip->entry.header)) {
          // Cannot write zip entry header
          err = ZIP_EWRTHDR;
          goto cleanup;
        }

        if (!mz_zip_writer_add_to_central_dir(
                pzip, zip->entry.name, entrylen, NULL, 0, "", 0,
                zip->entry.uncomp_size, zip->entry.comp_size, zip->entry.uncomp_crc32,
                zip->entry.method, 0, dos_time, dos_date, zip->entry.header_offset,
                zip->entry.external_attr, NULL, 0)) {
          // Cannot write to zip central dir
          err = ZIP_EWRTDIR;
          goto cleanup;
        }

        pzip->m_total_files++;
        pzip->m_archive_size = zip->entry.offset;

      cleanup:
        if (zip) {
          zip->entry.m_time = 0;
          CLEANUP(zip->entry.name);
        }
        return err;
      }

      const char *zip_entry_name(struct zip_t *zip) {
        if (!zip) {
          // zip_t handler is not initialized
          return NULL;
        }

        return zip->entry.name;
      }

      int zip_entry_index(struct zip_t *zip) {
        if (!zip) {
          // zip_t handler is not initialized
          return ZIP_ENOINIT;
        }

        return zip->entry.index;
      }

      int zip_entry_isdir(struct zip_t *zip) {
        if (!zip) {
          // zip_t handler is not initialized
          return ZIP_ENOINIT;
        }

        if (zip->entry.index < 0) {
          // zip entry is not opened
          return ZIP_EINVIDX;
        }

        return (int)mz_zip_reader_is_file_a_directory(&zip->archive,
                                                      (mz_uint)zip->entry.index);
      }

      unsigned long long zip_entry_size(struct zip_t *zip) {
        return zip ? zip->entry.uncomp_size : 0;
      }

      unsigned int zip_entry_crc32(struct zip_t *zip) {
        return zip ? zip->entry.uncomp_crc32 : 0;
      }

      int zip_entry_write(struct zip_t *zip, const void *buf, size_t bufsize) {
        mz_uint level;
        mz_zip_archive *pzip = NULL;
        tdefl_status status;

        if (!zip) {
          // zip_t handler is not initialized
          return ZIP_ENOINIT;
        }

        pzip = &(zip->archive);
        if (buf && bufsize > 0) {
          zip->entry.uncomp_size += bufsize;
          zip->entry.uncomp_crc32 = (mz_uint32)mz_crc32(
              zip->entry.uncomp_crc32, (const mz_uint8 *)buf, bufsize);

          level = zip->level & 0xF;
          if (!level) {
            if ((pzip->m_pWrite(pzip->m_pIO_opaque, zip->entry.offset, buf,
                                bufsize) != bufsize)) {
              // Cannot write buffer
              return ZIP_EWRTENT;
            }
            zip->entry.offset += bufsize;
            zip->entry.comp_size += bufsize;
          } else {
            status = tdefl_compress_buffer(&(zip->entry.comp), buf, bufsize,
                                           TDEFL_NO_FLUSH);
            if (status != TDEFL_STATUS_DONE && status != TDEFL_STATUS_OKAY) {
              // Cannot compress buffer
              return ZIP_ETDEFLBUF;
            }
          }
        }

        return 0;
      }

      int zip_entry_fwrite(struct zip_t *zip, const char *filename) {
        int err = 0;
        size_t n = 0;
        MZ_FILE *stream = NULL;
        mz_uint8 buf[MZ_ZIP_MAX_IO_BUF_SIZE];
        struct MZ_FILE_STAT_STRUCT file_stat;

        if (!zip) {
          // zip_t handler is not initialized
          return ZIP_ENOINIT;
        }

        memset(buf, 0, MZ_ZIP_MAX_IO_BUF_SIZE);
        memset((void *)&file_stat, 0, sizeof(struct MZ_FILE_STAT_STRUCT));
        if (MZ_FILE_STAT(filename, &file_stat) != 0) {
          // problem getting information - check errno
          return ZIP_ENOENT;
        }

        if ((file_stat.st_mode & 0200) == 0) {
          // MS-DOS read-only attribute
          zip->entry.external_attr |= 0x01;
        }
        zip->entry.external_attr |= (mz_uint32)((file_stat.st_mode & 0xFFFF) << 16);
        zip->entry.m_time = file_stat.st_mtime;

        if (!(stream = MZ_FOPEN(filename, "rb"))) {
          // Cannot open filename
          return ZIP_EOPNFILE;
        }

        while ((n = fread(buf, sizeof(mz_uint8), MZ_ZIP_MAX_IO_BUF_SIZE, stream)) >
               0) {
          if (zip_entry_write(zip, buf, n) < 0) {
            err = ZIP_EWRTENT;
            break;
          }
        }
        fclose(stream);

        return err;
      }

      ssize_t zip_entry_read(struct zip_t *zip, void **buf, size_t *bufsize) {
        mz_zip_archive *pzip = NULL;
        mz_uint idx;
        size_t size = 0;

        if (!zip) {
          // zip_t handler is not initialized
          return (ssize_t)ZIP_ENOINIT;
        }

        pzip = &(zip->archive);
        if (pzip->m_zip_mode != MZ_ZIP_MODE_READING || zip->entry.index < 0) {
          // the entry is not found or we do not have read access
          return (ssize_t)ZIP_ENOENT;
        }

        idx = (mz_uint)zip->entry.index;
        if (mz_zip_reader_is_file_a_directory(pzip, idx)) {
          // the entry is a directory
          return (ssize_t)ZIP_EINVENTTYPE;
        }

        *buf = mz_zip_reader_extract_to_heap(pzip, idx, &size, 0);
        if (*buf && bufsize) {
          *bufsize = size;
        }
        return (ssize_t)size;
      }

      ssize_t zip_entry_noallocread(struct zip_t *zip, void *buf, size_t bufsize) {
        mz_zip_archive *pzip = NULL;

        if (!zip) {
          // zip_t handler is not initialized
          return (ssize_t)ZIP_ENOINIT;
        }

        pzip = &(zip->archive);
        if (pzip->m_zip_mode != MZ_ZIP_MODE_READING || zip->entry.index < 0) {
          // the entry is not found or we do not have read access
          return (ssize_t)ZIP_ENOENT;
        }

        if (!mz_zip_reader_extract_to_mem_no_alloc(pzip, (mz_uint)zip->entry.index,
                                                   buf, bufsize, 0, NULL, 0)) {
          return (ssize_t)ZIP_EMEMNOALLOC;
        }

        return (ssize_t)zip->entry.uncomp_size;
      }

      int zip_entry_fread(struct zip_t *zip, const char *filename) {
        mz_zip_archive *pzip = NULL;
        mz_uint idx;
        mz_uint32 xattr = 0;
        mz_zip_archive_file_stat info;

        if (!zip) {
          // zip_t handler is not initialized
          return ZIP_ENOINIT;
        }

        memset((void *)&info, 0, sizeof(mz_zip_archive_file_stat));
        pzip = &(zip->archive);
        if (pzip->m_zip_mode != MZ_ZIP_MODE_READING || zip->entry.index < 0) {
          // the entry is not found or we do not have read access
          return ZIP_ENOENT;
        }

        idx = (mz_uint)zip->entry.index;
        if (mz_zip_reader_is_file_a_directory(pzip, idx)) {
          // the entry is a directory
          return ZIP_EINVENTTYPE;
        }

        if (!mz_zip_reader_extract_to_file(pzip, idx, filename, 0)) {
          return ZIP_ENOFILE;
        }

      #if defined(_MSC_VER)
        (void)xattr; // unused
      #else
        if (!mz_zip_reader_file_stat(pzip, idx, &info)) {
          // Cannot get information about zip archive;
          return ZIP_ENOFILE;
        }

        xattr = (info.m_external_attr >> 16) & 0xFFFF;
        if (xattr > 0) {
          if (chmod(filename, (mode_t)xattr) < 0) {
            return ZIP_ENOPERM;
          }
        }
      #endif

        return 0;
      }

      int zip_entry_extract(struct zip_t *zip,
                            size_t (*on_extract)(void *arg, uint64_t offset,
                                                 const void *buf, size_t bufsize),
                            void *arg) {
        mz_zip_archive *pzip = NULL;
        mz_uint idx;

        if (!zip) {
          // zip_t handler is not initialized
          return ZIP_ENOINIT;
        }

        pzip = &(zip->archive);
        if (pzip->m_zip_mode != MZ_ZIP_MODE_READING || zip->entry.index < 0) {
          // the entry is not found or we do not have read access
          return ZIP_ENOENT;
        }

        idx = (mz_uint)zip->entry.index;
        return (mz_zip_reader_extract_to_callback(pzip, idx, on_extract, arg, 0))
                   ? 0
                   : ZIP_EINVIDX;
      }

      ssize_t zip_entries_total(struct zip_t *zip) {
        if (!zip) {
          // zip_t handler is not initialized
          return ZIP_ENOINIT;
        }

        return (ssize_t)zip->archive.m_total_files;
      }

      ssize_t zip_entries_delete(struct zip_t *zip, char *const entries[],
                                 size_t len) {
        ssize_t n = 0;
        ssize_t err = 0;
        struct zip_entry_mark_t *entry_mark = NULL;

        if (zip == NULL || (entries == NULL && len != 0)) {
          return ZIP_ENOINIT;
        }

        if (entries == NULL && len == 0) {
          return 0;
        }

        n = zip_entries_total(zip);

        entry_mark = (struct zip_entry_mark_t *)calloc(
            (size_t)n, sizeof(struct zip_entry_mark_t));
        if (!entry_mark) {
          return ZIP_EOOMEM;
        }

        zip->archive.m_zip_mode = MZ_ZIP_MODE_READING;

        err = zip_entry_set(zip, entry_mark, (int)n, entries, len);
        if (err < 0) {
          CLEANUP(entry_mark);
          return err;
        }

        err = zip_entries_delete_mark(zip, entry_mark, (int)n);
        CLEANUP(entry_mark);
        return err;
      }

      int zip_stream_extract(const char *stream, size_t size, const char *dir,
                             int (*on_extract)(const char *filename, void *arg),
                             void *arg) {
        mz_zip_archive zip_archive;
        if (!stream || !dir) {
          // Cannot parse zip archive stream
          return ZIP_ENOINIT;
        }
        if (!memset(&zip_archive, 0, sizeof(mz_zip_archive))) {
          // Cannot memset zip archive
          return ZIP_EMEMSET;
        }
        if (!mz_zip_reader_init_mem(&zip_archive, stream, size, 0)) {
          // Cannot initialize zip_archive reader
          return ZIP_ENOINIT;
        }

        return zip_archive_extract(&zip_archive, dir, on_extract, arg);
      }

      struct zip_t *zip_stream_open(const char *stream, size_t size, int level,
                                    char mode) {
        struct zip_t *zip = (struct zip_t *)calloc((size_t)1, sizeof(struct zip_t));
        if (!zip) {
          return NULL;
        }

        if (level < 0) {
          level = MZ_DEFAULT_LEVEL;
        }
        if ((level & 0xF) > MZ_UBER_COMPRESSION) {
          // Wrong compression level
          goto cleanup;
        }
        zip->level = (mz_uint)level;

        if ((stream != NULL) && (size > 0) && (mode == 'r')) {
          if (!mz_zip_reader_init_mem(&(zip->archive), stream, size, 0)) {
            goto cleanup;
          }
        } else if ((stream == NULL) && (size == 0) && (mode == 'w')) {
          // Create a new archive.
          if (!mz_zip_writer_init_heap(&(zip->archive), 0, 1024)) {
            // Cannot initialize zip_archive writer
            goto cleanup;
          }
        } else {
          goto cleanup;
        }
        return zip;

      cleanup:
        CLEANUP(zip);
        return NULL;
      }

      ssize_t zip_stream_copy(struct zip_t *zip, void **buf, size_t *bufsize) {
        size_t n;

        if (!zip) {
          return (ssize_t)ZIP_ENOINIT;
        }
        zip_archive_finalize(&(zip->archive));

        n = (size_t)zip->archive.m_archive_size;
        if (bufsize != NULL) {
          *bufsize = n;
        }

        *buf = calloc(sizeof(unsigned char), n);
        memcpy(*buf, zip->archive.m_pState->m_pMem, n);

        return (ssize_t)n;
      }

      void zip_stream_close(struct zip_t *zip) {
        if (zip) {
          mz_zip_writer_end(&(zip->archive));
          mz_zip_reader_end(&(zip->archive));
          CLEANUP(zip);
        }
      }

      int zip_create(const char *zipname, const char *filenames[], size_t len) {
        int err = 0;
        size_t i;
        mz_zip_archive zip_archive;
        struct MZ_FILE_STAT_STRUCT file_stat;
        mz_uint32 ext_attributes = 0;

        if (!zipname || strlen(zipname) < 1) {
          // zip_t archive name is empty or NULL
          return ZIP_EINVZIPNAME;
        }

        // Create a new archive.
        if (!memset(&(zip_archive), 0, sizeof(zip_archive))) {
          // Cannot memset zip archive
          return ZIP_EMEMSET;
        }

        if (!mz_zip_writer_init_file(&zip_archive, zipname, 0)) {
          // Cannot initialize zip_archive writer
          return ZIP_ENOINIT;
        }

        if (!memset((void *)&file_stat, 0, sizeof(struct MZ_FILE_STAT_STRUCT))) {
          return ZIP_EMEMSET;
        }

        for (i = 0; i < len; ++i) {
          const char *name = filenames[i];
          if (!name) {
            err = ZIP_EINVENTNAME;
            break;
          }

          if (MZ_FILE_STAT(name, &file_stat) != 0) {
            // problem getting information - check errno
            err = ZIP_ENOFILE;
            break;
          }

          if ((file_stat.st_mode & 0200) == 0) {
            // MS-DOS read-only attribute
            ext_attributes |= 0x01;
          }
          ext_attributes |= (mz_uint32)((file_stat.st_mode & 0xFFFF) << 16);

          if (!mz_zip_writer_add_file(&zip_archive, zip_basename(name), name, "", 0,
                                      ZIP_DEFAULT_COMPRESSION_LEVEL,
                                      ext_attributes)) {
            // Cannot add file to zip_archive
            err = ZIP_ENOFILE;
            break;
          }
        }

        mz_zip_writer_finalize_archive(&zip_archive);
        mz_zip_writer_end(&zip_archive);
        return err;
      }

      int zip_extract(const char *zipname, const char *dir,
                      int (*on_extract)(const char *filename, void *arg), void *arg) {
        mz_zip_archive zip_archive;

        if (!zipname || !dir) {
          // Cannot parse zip archive name
          return ZIP_EINVZIPNAME;
        }

        if (!memset(&zip_archive, 0, sizeof(mz_zip_archive))) {
          // Cannot memset zip archive
          return ZIP_EMEMSET;
        }

        // Now try to open the archive.
        if (!mz_zip_reader_init_file(&zip_archive, zipname, 0)) {
          // Cannot initialize zip_archive reader
          return ZIP_ENOINIT;
        }

        return zip_archive_extract(&zip_archive, dir, on_extract, arg);
      }
typedef void*(*ROGUEM0)(void*);
typedef void(*ROGUEM1)(void*);
typedef RogueInt32(*ROGUEM2)(void*);
typedef void*(*ROGUEM3)(void*,RogueInt32);
typedef void*(*ROGUEM4)(void*,void*);
typedef RogueLogical(*ROGUEM5)(void*);
typedef void*(*ROGUEM6)(void*,void*,void*);
typedef RogueInt64(*ROGUEM7)(void*);
typedef void*(*ROGUEM8)(void*,void*,RogueInt32);
typedef RogueLogical(*ROGUEM9)(void*,void*);
typedef RogueClassTableKeysIterator_String_Value_(*ROGUEM10)(void*);
typedef RogueLogical(*ROGUEM11)(void*,void*,void*);
typedef RogueLogical(*ROGUEM12)(void*,RogueLogical);
typedef RogueLogical(*ROGUEM13)(void*,RogueLogical,RogueLogical,RogueLogical);
typedef void(*ROGUEM14)(void*,void*);

// Object.description()
extern void* Rogue_call_ROGUEM0( int i, void* THIS )
{
  return ((ROGUEM0)(((RogueObject*)THIS)->type->methods[i]))( THIS );
}

// (Function()).call()
extern void Rogue_call_ROGUEM1( int i, void* THIS )
{
  ((ROGUEM1)(((RogueObject*)THIS)->type->methods[i]))( THIS );
}

// Value.count()
extern RogueInt32 Rogue_call_ROGUEM2( int i, void* THIS )
{
  return ((ROGUEM2)(((RogueObject*)THIS)->type->methods[i]))( THIS );
}

// Value.at(Int32)
extern void* Rogue_call_ROGUEM3( int i, void* THIS, RogueInt32 p0 )
{
  return ((ROGUEM3)(((RogueObject*)THIS)->type->methods[i]))( THIS, p0 );
}

// Value.first((Function(Value)->Logical))
extern void* Rogue_call_ROGUEM4( int i, void* THIS, void* p0 )
{
  return ((ROGUEM4)(((RogueObject*)THIS)->type->methods[i]))( THIS, p0 );
}

// Value.is_collection()
extern RogueLogical Rogue_call_ROGUEM5( int i, void* THIS )
{
  return ((ROGUEM5)(((RogueObject*)THIS)->type->methods[i]))( THIS );
}

// Value.set(String,Value)
extern void* Rogue_call_ROGUEM6( int i, void* THIS, void* p0, void* p1 )
{
  return ((ROGUEM6)(((RogueObject*)THIS)->type->methods[i]))( THIS, p0, p1 );
}

// Value.to_Int64()
extern RogueInt64 Rogue_call_ROGUEM7( int i, void* THIS )
{
  return ((ROGUEM7)(((RogueObject*)THIS)->type->methods[i]))( THIS );
}

// Value.to_json(StringBuilder,Int32)
extern void* Rogue_call_ROGUEM8( int i, void* THIS, void* p0, RogueInt32 p1 )
{
  return ((ROGUEM8)(((RogueObject*)THIS)->type->methods[i]))( THIS, p0, p1 );
}

// (Function(Value)->Logical).call(Value)
extern RogueLogical Rogue_call_ROGUEM9( int i, void* THIS, void* p0 )
{
  return ((ROGUEM9)(((RogueObject*)THIS)->type->methods[i]))( THIS, p0 );
}

// (Function(TableEntry<<String,Value>>,TableEntry<<String,Value>>)->Logical).call(TableEntry<<String,Value>>,TableEntry<<String,Value>>)
extern RogueLogical Rogue_call_ROGUEM11( int i, void* THIS, void* p0, void* p1 )
{
  return ((ROGUEM11)(((RogueObject*)THIS)->type->methods[i]))( THIS, p0, p1 );
}

// Process.update_io(Logical)
extern RogueLogical Rogue_call_ROGUEM12( int i, void* THIS, RogueLogical p0 )
{
  return ((ROGUEM12)(((RogueObject*)THIS)->type->methods[i]))( THIS, p0 );
}

// (Function(String)).call(String)
extern void Rogue_call_ROGUEM14( int i, void* THIS, void* p0 )
{
  ((ROGUEM14)(((RogueObject*)THIS)->type->methods[i]))( THIS, p0 );
}


// GLOBAL PROPERTIES
RogueString_List* RogueSystem_command_line_arguments = 0;
RogueString* RogueSystem_executable_filepath = 0;
RogueClassStringValue* RogueStringValue_empty_string = 0;
RogueClassLogicalValue* RogueLogicalValue_true_value = 0;
RogueClassLogicalValue* RogueLogicalValue_false_value = 0;
RogueString_List* RogueStringEncoding_names = 0;
RogueInt32_List* RogueStringEncoding_values = 0;
RogueString_List* RogueConsoleEventType_names = 0;
RogueInt32_List* RogueConsoleEventType_values = 0;
RogueByte_List* RogueZipEntry_buffer = 0;
RogueString_List* RogueUnixConsoleMouseEventType_names = 0;
RogueInt32_List* RogueUnixConsoleMouseEventType_values = 0;
ROGUE_THREAD_LOCAL RogueByte_List* RogueStringBuilder_work_bytes = 0;
ROGUE_THREAD_LOCAL RogueClassStringBuilderPool* RogueStringBuilder_pool = 0;
ROGUE_THREAD_LOCAL RogueGenericListRewriter_List* RogueListRewriter_String__pool = 0;

void RogueGlobal_trace( void* obj );
void RogueStringBuilder_trace( void* obj );
void RogueByte_List_trace( void* obj );
void RogueStringBuilderPool_trace( void* obj );
void RogueStringBuilder_List_trace( void* obj );
void Rogue_Function____List_trace( void* obj );
void RogueStackTrace_trace( void* obj );
void RogueString_List_trace( void* obj );
void RogueCharacter_List_trace( void* obj );
void RogueFile_trace( void* obj );
void RogueInt32_List_trace( void* obj );
void RogueValueTable_trace( void* obj );
void RogueTable_String_Value__trace( void* obj );
void RogueTableEntry_String_Value__trace( void* obj );
void RogueSystemEnvironment_trace( void* obj );
void RogueStringTable_String__trace( void* obj );
void RogueTable_String_String__trace( void* obj );
void RogueTableEntry_String_String__trace( void* obj );
void RogueStringValue_trace( void* obj );
void RogueValueList_trace( void* obj );
void RogueValue_List_trace( void* obj );
void RogueJSONParser_trace( void* obj );
void RogueScanner_trace( void* obj );
void RogueStringConsolidationTable_trace( void* obj );
void RogueFileWriter_trace( void* obj );
void RogueConsole_trace( void* obj );
void RogueConsoleErrorPrinter_trace( void* obj );
void RogueConsoleEvent_List_trace( void* obj );
void RogueFileReader_trace( void* obj );
void RogueLineReader_trace( void* obj );
void RogueProcess_trace( void* obj );
void RogueProcessResult_trace( void* obj );
void RogueWindowsProcess_trace( void* obj );
void RogueWindowsProcessReader_trace( void* obj );
void RogueWindowsProcessWriter_trace( void* obj );
void RoguePosixProcess_trace( void* obj );
void RoguePosixProcessReader_trace( void* obj );
void RogueFDReader_trace( void* obj );
void RogueFDWriter_trace( void* obj );
void RogueMorlock_trace( void* obj );
void RogueFunction_868_trace( void* obj );
void RoguePackage_trace( void* obj );
void RoguePackageInfo_trace( void* obj );
void RogueFileListing_trace( void* obj );
void RogueFileListing_collect_String__trace( void* obj );
void RogueListRewriter_String__trace( void* obj );
void RogueGenericListRewriter_List_trace( void* obj );
void RoguePlatforms_trace( void* obj );
void RogueZip_trace( void* obj );
void RogueFunction_1041_trace( void* obj );
void RogueFunction_1043_trace( void* obj );
void RogueExtendedASCIIReader_trace( void* obj );
void RogueUTF8Reader_trace( void* obj );
void RogueWeakReference_trace( void* obj );
void RogueFunction_2437_trace( void* obj );
void RogueException_trace( void* obj );
void RogueError_trace( void* obj );
void RogueOutOfBoundsError_trace( void* obj );
void RogueJSONParseError_trace( void* obj );
void RogueIOError_trace( void* obj );
void RoguePackageError_trace( void* obj );
void RogueTableKeysIterator_String_Value__trace( void* obj );
void RogueOptionalString_trace( void* obj );
void RogueTableKeysIterator_String_String__trace( void* obj );
void RogueFilePattern_trace( void* obj );
void RogueOptionalFilePattern_trace( void* obj );
void RogueVersionNumber_trace( void* obj );
void RogueBest_String__trace( void* obj );
void RogueZipEntry_trace( void* obj );

void RogueGlobal_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassGlobal*)obj)->global_output_buffer)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassGlobal*)obj)->console)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassGlobal*)obj)->exit_functions)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueStringBuilder_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueStringBuilder*)obj)->utf8)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueByte_List_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueByte_List*)obj)->data)) RogueArray_trace( link );
}

void RogueStringBuilderPool_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassStringBuilderPool*)obj)->available)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueStringBuilder_List_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueStringBuilder_List*)obj)->data)) RogueArray_trace( link );
}

void Rogue_Function____List_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((Rogue_Function____List*)obj)->data)) RogueArray_trace( link );
}

void RogueStackTrace_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassStackTrace*)obj)->entries)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueString_List_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueString_List*)obj)->data)) RogueArray_trace( link );
}

void RogueCharacter_List_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueCharacter_List*)obj)->data)) RogueArray_trace( link );
}

void RogueFile_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassFile*)obj)->filepath)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueInt32_List_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueInt32_List*)obj)->data)) RogueArray_trace( link );
}

void RogueValueTable_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassValueTable*)obj)->data)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueTable_String_Value__trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassTable_String_Value_*)obj)->bins)) RogueArray_trace( link );
  if ((link=((RogueClassTable_String_Value_*)obj)->first_entry)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassTable_String_Value_*)obj)->last_entry)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassTable_String_Value_*)obj)->cur_entry)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassTable_String_Value_*)obj)->sort_function)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueTableEntry_String_Value__trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassTableEntry_String_Value_*)obj)->key)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassTableEntry_String_Value_*)obj)->value)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassTableEntry_String_Value_*)obj)->adjacent_entry)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassTableEntry_String_Value_*)obj)->next_entry)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassTableEntry_String_Value_*)obj)->previous_entry)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueSystemEnvironment_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassSystemEnvironment*)obj)->definitions)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassSystemEnvironment*)obj)->names)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueStringTable_String__trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassStringTable_String_*)obj)->bins)) RogueArray_trace( link );
  if ((link=((RogueClassStringTable_String_*)obj)->first_entry)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassStringTable_String_*)obj)->last_entry)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassStringTable_String_*)obj)->cur_entry)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassStringTable_String_*)obj)->sort_function)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueTable_String_String__trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassTable_String_String_*)obj)->bins)) RogueArray_trace( link );
  if ((link=((RogueClassTable_String_String_*)obj)->first_entry)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassTable_String_String_*)obj)->last_entry)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassTable_String_String_*)obj)->cur_entry)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassTable_String_String_*)obj)->sort_function)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueTableEntry_String_String__trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassTableEntry_String_String_*)obj)->key)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassTableEntry_String_String_*)obj)->value)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassTableEntry_String_String_*)obj)->adjacent_entry)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassTableEntry_String_String_*)obj)->next_entry)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassTableEntry_String_String_*)obj)->previous_entry)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueStringValue_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassStringValue*)obj)->value)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueValueList_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassValueList*)obj)->data)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueValue_List_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueValue_List*)obj)->data)) RogueArray_trace( link );
}

void RogueJSONParser_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassJSONParser*)obj)->reader)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueScanner_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassScanner*)obj)->data)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassScanner*)obj)->source)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueStringConsolidationTable_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassStringConsolidationTable*)obj)->bins)) RogueArray_trace( link );
  if ((link=((RogueClassStringConsolidationTable*)obj)->first_entry)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassStringConsolidationTable*)obj)->last_entry)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassStringConsolidationTable*)obj)->cur_entry)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassStringConsolidationTable*)obj)->sort_function)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueFileWriter_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassFileWriter*)obj)->filepath)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassFileWriter*)obj)->buffer)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueConsole_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassConsole*)obj)->output_buffer)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassConsole*)obj)->error)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassConsole*)obj)->events)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassConsole*)obj)->input_buffer)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassConsole*)obj)->_input_bytes)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueConsoleErrorPrinter_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassConsoleErrorPrinter*)obj)->output_buffer)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueConsoleEvent_List_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueConsoleEvent_List*)obj)->data)) RogueArray_trace( link );
}

void RogueFileReader_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassFileReader*)obj)->filepath)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassFileReader*)obj)->buffer)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueLineReader_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassLineReader*)obj)->source)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassLineReader*)obj)->next)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassLineReader*)obj)->buffer)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueProcess_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassProcess*)obj)->args)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassProcess*)obj)->output_reader)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassProcess*)obj)->error_reader)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassProcess*)obj)->input_writer)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassProcess*)obj)->result)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueProcessResult_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassProcessResult*)obj)->output_bytes)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassProcessResult*)obj)->error_bytes)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassProcessResult*)obj)->output_string)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassProcessResult*)obj)->error_string)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueWindowsProcess_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassWindowsProcess*)obj)->args)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassWindowsProcess*)obj)->output_reader)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassWindowsProcess*)obj)->error_reader)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassWindowsProcess*)obj)->input_writer)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassWindowsProcess*)obj)->result)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassWindowsProcess*)obj)->arg_string)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueWindowsProcessReader_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassWindowsProcessReader*)obj)->buffer)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueWindowsProcessWriter_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassWindowsProcessWriter*)obj)->buffer)) ((RogueObject*)link)->type->trace_fn( link );
}

void RoguePosixProcess_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassPosixProcess*)obj)->args)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassPosixProcess*)obj)->output_reader)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassPosixProcess*)obj)->error_reader)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassPosixProcess*)obj)->input_writer)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassPosixProcess*)obj)->result)) ((RogueObject*)link)->type->trace_fn( link );
}

void RoguePosixProcessReader_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassPosixProcessReader*)obj)->process)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassPosixProcessReader*)obj)->fd_reader)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueFDReader_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassFDReader*)obj)->buffer)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueFDWriter_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassFDWriter*)obj)->buffer)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueMorlock_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassMorlock*)obj)->HOME)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueFunction_868_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassFunction_868*)obj)->binpath)) ((RogueObject*)link)->type->trace_fn( link );
}

void RoguePackage_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassPackage*)obj)->action)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassPackage*)obj)->name)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassPackage*)obj)->host)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassPackage*)obj)->provider)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassPackage*)obj)->repo)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassPackage*)obj)->app_name)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassPackage*)obj)->specified_version)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassPackage*)obj)->version)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassPackage*)obj)->url)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassPackage*)obj)->morlock_home)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassPackage*)obj)->launcher_folder)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassPackage*)obj)->install_folder)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassPackage*)obj)->bin_folder)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassPackage*)obj)->archive_filename)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassPackage*)obj)->archive_folder)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassPackage*)obj)->is_unpacked)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassPackage*)obj)->releases)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassPackage*)obj)->properties)) ((RogueObject*)link)->type->trace_fn( link );
}

void RoguePackageInfo_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassPackageInfo*)obj)->name)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassPackageInfo*)obj)->provider)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassPackageInfo*)obj)->app_name)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassPackageInfo*)obj)->repo)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassPackageInfo*)obj)->version)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassPackageInfo*)obj)->host)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassPackageInfo*)obj)->folder)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassPackageInfo*)obj)->filepath)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassPackageInfo*)obj)->url)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassPackageInfo*)obj)->installed_versions)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassPackageInfo*)obj)->morlock_home)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueFileListing_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassFileListing*)obj)->folder)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassFileListing*)obj)->pattern)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassFileListing*)obj)->path_segments)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassFileListing*)obj)->pattern_segments)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassFileListing*)obj)->filepath_segments)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassFileListing*)obj)->empty_segments)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassFileListing*)obj)->results)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassFileListing*)obj)->callback)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueFileListing_collect_String__trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassFileListing_collect_String_*)obj)->context)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueListRewriter_String__trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassListRewriter_String_*)obj)->list)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueGenericListRewriter_List_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueGenericListRewriter_List*)obj)->data)) RogueArray_trace( link );
}

void RoguePlatforms_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassPlatforms*)obj)->combined)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueZip_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassZip*)obj)->filepath)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueFunction_1041_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassFunction_1041*)obj)->app_name)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueFunction_1043_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassFunction_1043*)obj)->app_name)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueExtendedASCIIReader_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassExtendedASCIIReader*)obj)->byte_reader)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueUTF8Reader_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassUTF8Reader*)obj)->byte_reader)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueWeakReference_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueWeakReference*)obj)->next_weak_reference)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueFunction_2437_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassFunction_2437*)obj)->console)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueException_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueException*)obj)->message)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueException*)obj)->stack_trace)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueError_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassError*)obj)->message)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassError*)obj)->stack_trace)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueOutOfBoundsError_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassOutOfBoundsError*)obj)->message)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassOutOfBoundsError*)obj)->stack_trace)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueJSONParseError_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassJSONParseError*)obj)->message)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassJSONParseError*)obj)->stack_trace)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueIOError_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassIOError*)obj)->message)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassIOError*)obj)->stack_trace)) ((RogueObject*)link)->type->trace_fn( link );
}

void RoguePackageError_trace( void* obj )
{
  void* link;
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
  if ((link=((RogueClassPackageError*)obj)->message)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassPackageError*)obj)->stack_trace)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassPackageError*)obj)->package_name)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueTableKeysIterator_String_Value__trace( void* obj )
{
  void* link;
  if ((link=((RogueClassTableKeysIterator_String_Value_*)obj)->cur)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueOptionalString_trace( void* obj )
{
  void* link;
  if ((link=((RogueOptionalString*)obj)->value)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueTableKeysIterator_String_String__trace( void* obj )
{
  void* link;
  if ((link=((RogueClassTableKeysIterator_String_String_*)obj)->cur)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueFilePattern_trace( void* obj )
{
  void* link;
  if ((link=((RogueClassFilePattern*)obj)->pattern)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueOptionalFilePattern_trace( void* obj )
{
  RogueFilePattern_trace( &((RogueOptionalFilePattern*)obj)->value );
}

void RogueVersionNumber_trace( void* obj )
{
  void* link;
  if ((link=((RogueClassVersionNumber*)obj)->version)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueBest_String__trace( void* obj )
{
  void* link;
  if ((link=((RogueClassBest_String_*)obj)->better_fn)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassBest_String_*)obj)->value)) ((RogueObject*)link)->type->trace_fn( link );
}

void RogueZipEntry_trace( void* obj )
{
  void* link;
  if ((link=((RogueClassZipEntry*)obj)->zip)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=((RogueClassZipEntry*)obj)->name)) ((RogueObject*)link)->type->trace_fn( link );
}


const int Rogue_type_name_index_table[] =
{
  324,445,446,327,376,328,447,384,329,448,341,379,390,449,380,352,
  391,326,351,377,386,450,451,452,325,375,389,330,331,382,393,332,
  424,403,404,333,387,334,346,335,336,425,337,385,338,340,339,342,
  405,406,407,408,378,388,343,344,345,453,444,347,454,348,455,373,
  383,394,349,456,350,457,395,353,359,435,354,355,436,356,357,358,
  360,361,426,427,362,363,364,365,437,428,438,366,458,381,392,367,
  439,368,440,369,441,442,443,370,429,430,431,432,433,371,372,418,
  374,396,114,13,402,420,421,422,423,459,460,461,462,463,464,465,
  466,467,468,469,470,471,472,473,474,475,476
};
RogueInitFn Rogue_init_object_fn_table[] =
{
  (RogueInitFn) RogueGlobal__init_object,
  0,
  0,
  (RogueInitFn) RogueStringBuilder__init_object,
  (RogueInitFn) RogueByte_List__init_object,
  (RogueInitFn) RogueGenericList__init_object,
  0,
  0,
  0,
  0,
  (RogueInitFn) RogueStringBuilderPool__init_object,
  (RogueInitFn) RogueStringBuilder_List__init_object,
  0,
  0,
  (RogueInitFn) Rogue_Function____List__init_object,
  (RogueInitFn) Rogue_Function_____init_object,
  0,
  0,
  (RogueInitFn) RogueStackTrace__init_object,
  (RogueInitFn) RogueString_List__init_object,
  0,
  0,
  0,
  0,
  (RogueInitFn) RogueValue__init_object,
  (RogueInitFn) RogueCharacter_List__init_object,
  0,
  (RogueInitFn) Rogue_Function_Value_RETURNSLogical___init_object,
  (RogueInitFn) RogueFile__init_object,
  (RogueInitFn) RogueInt32_List__init_object,
  0,
  (RogueInitFn) RogueRuntime__init_object,
  (RogueInitFn) RogueUndefinedValue__init_object,
  (RogueInitFn) RogueNullValue__init_object,
  (RogueInitFn) RogueValueTable__init_object,
  (RogueInitFn) RogueTable_String_Value___init_object,
  0,
  (RogueInitFn) RogueTableEntry_String_Value___init_object,
  (RogueInitFn) Rogue_Function_TableEntry_String_Value__TableEntry_String_Value__RETURNSLogical___init_object,
  (RogueInitFn) RogueSystem__init_object,
  (RogueInitFn) RogueSystemEnvironment__init_object,
  (RogueInitFn) RogueStringTable_String___init_object,
  (RogueInitFn) RogueTable_String_String___init_object,
  0,
  (RogueInitFn) RogueTableEntry_String_String___init_object,
  (RogueInitFn) Rogue_Function_TableEntry_String_String__TableEntry_String_String__RETURNSLogical___init_object,
  (RogueInitFn) Rogue_Function_String_RETURNSLogical___init_object,
  (RogueInitFn) RogueMath__init_object,
  (RogueInitFn) RogueStringValue__init_object,
  (RogueInitFn) RogueReal64Value__init_object,
  (RogueInitFn) RogueLogicalValue__init_object,
  (RogueInitFn) RogueValueList__init_object,
  (RogueInitFn) RogueValue_List__init_object,
  0,
  (RogueInitFn) RogueJSON__init_object,
  (RogueInitFn) RogueJSONParser__init_object,
  (RogueInitFn) RogueScanner__init_object,
  0,
  (RogueInitFn) RogueStringConsolidationTable__init_object,
  (RogueInitFn) RogueFileWriter__init_object,
  0,
  (RogueInitFn) RogueConsole__init_object,
  0,
  (RogueInitFn) RogueConsoleErrorPrinter__init_object,
  (RogueInitFn) RogueConsoleEvent_List__init_object,
  0,
  (RogueInitFn) RogueFileReader__init_object,
  0,
  (RogueInitFn) RogueLineReader__init_object,
  0,
  (RogueInitFn) RogueFunction_819__init_object,
  (RogueInitFn) RogueProcess__init_object,
  (RogueInitFn) RogueProcessResult__init_object,
  (RogueInitFn) RogueWindowsProcess__init_object,
  (RogueInitFn) RogueWindowsProcessReader__init_object,
  (RogueInitFn) RogueWindowsProcessWriter__init_object,
  (RogueInitFn) RoguePosixProcess__init_object,
  (RogueInitFn) RoguePosixProcessReader__init_object,
  (RogueInitFn) RogueFDReader__init_object,
  (RogueInitFn) RogueFDWriter__init_object,
  (RogueInitFn) RogueMorlock__init_object,
  (RogueInitFn) RogueBootstrap__init_object,
  (RogueInitFn) RogueFunction_863__init_object,
  (RogueInitFn) RogueFunction_868__init_object,
  (RogueInitFn) RoguePackage__init_object,
  (RogueInitFn) RoguePackageInfo__init_object,
  (RogueInitFn) RogueFileListing__init_object,
  (RogueInitFn) Rogue_Function_String____init_object,
  (RogueInitFn) RogueFileListing_collect_String___init_object,
  (RogueInitFn) RogueFunction_896__init_object,
  (RogueInitFn) RogueListRewriter_String___init_object,
  (RogueInitFn) RogueGenericListRewriter__init_object,
  0,
  (RogueInitFn) RogueGenericListRewriter_List__init_object,
  0,
  (RogueInitFn) Rogue_Function_String_String_RETURNSLogical___init_object,
  (RogueInitFn) RogueFunction_948__init_object,
  (RogueInitFn) RogueQuicksort_String___init_object,
  (RogueInitFn) RogueFunction_950__init_object,
  (RogueInitFn) RoguePlatforms__init_object,
  (RogueInitFn) RogueFunction_983__init_object,
  (RogueInitFn) RogueFunction_999__init_object,
  (RogueInitFn) RogueFunction_1003__init_object,
  (RogueInitFn) RogueZip__init_object,
  (RogueInitFn) RogueFunction_1033__init_object,
  (RogueInitFn) RogueFunction_1039__init_object,
  (RogueInitFn) RogueFunction_1040__init_object,
  (RogueInitFn) RogueFunction_1041__init_object,
  (RogueInitFn) RogueFunction_1043__init_object,
  (RogueInitFn) RogueExtendedASCIIReader__init_object,
  (RogueInitFn) RogueUTF8Reader__init_object,
  (RogueInitFn) RogueFunction_1088__init_object,
  (RogueInitFn) RogueWeakReference__init_object,
  (RogueInitFn) RogueFunction_2437__init_object,
  0,
  (RogueInitFn) RogueException__init_object,
  (RogueInitFn) RogueError__init_object,
  (RogueInitFn) RogueOutOfBoundsError__init_object,
  (RogueInitFn) RogueJSONParseError__init_object,
  (RogueInitFn) RogueIOError__init_object,
  (RogueInitFn) RoguePackageError__init_object,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0
};

RogueInitFn Rogue_init_fn_table[] =
{
  (RogueInitFn) RogueGlobal__init,
  0,
  0,
  (RogueInitFn) RogueStringBuilder__init,
  (RogueInitFn) RogueByte_List__init,
  (RogueInitFn) RogueObject__init,
  0,
  0,
  0,
  0,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueStringBuilder_List__init,
  0,
  0,
  (RogueInitFn) Rogue_Function____List__init,
  (RogueInitFn) RogueObject__init,
  0,
  0,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueString_List__init,
  0,
  0,
  0,
  0,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueCharacter_List__init,
  0,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueInt32_List__init,
  0,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueValueTable__init,
  (RogueInitFn) RogueTable_String_Value___init,
  0,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueTable_String_String___init,
  (RogueInitFn) RogueTable_String_String___init,
  0,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueValueList__init,
  (RogueInitFn) RogueValue_List__init,
  0,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  0,
  (RogueInitFn) RogueTable_String_String___init,
  (RogueInitFn) RogueObject__init,
  0,
  (RogueInitFn) RogueConsole__init,
  0,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueConsoleEvent_List__init,
  0,
  (RogueInitFn) RogueObject__init,
  0,
  (RogueInitFn) RogueObject__init,
  0,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueWindowsProcessReader__init,
  (RogueInitFn) RogueWindowsProcessWriter__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RoguePackage__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  0,
  (RogueInitFn) RogueGenericListRewriter_List__init,
  0,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  (RogueInitFn) RogueObject__init,
  0,
  (RogueInitFn) RogueException__init,
  (RogueInitFn) RogueException__init,
  (RogueInitFn) RogueException__init,
  (RogueInitFn) RogueException__init,
  (RogueInitFn) RogueException__init,
  (RogueInitFn) RogueException__init,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0
};

RogueCleanUpFn Rogue_on_cleanup_fn_table[] =
{
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  (RogueCleanUpFn) RogueFileWriter__on_cleanup,
  0,
  0,
  0,
  0,
  0,
  0,
  (RogueCleanUpFn) RogueFileReader__on_cleanup,
  0,
  0,
  0,
  0,
  (RogueCleanUpFn) RogueProcess__on_cleanup,
  0,
  (RogueCleanUpFn) RogueWindowsProcess__on_cleanup,
  0,
  0,
  (RogueCleanUpFn) RogueProcess__on_cleanup,
  0,
  (RogueCleanUpFn) RogueFDReader__on_cleanup,
  (RogueCleanUpFn) RogueFDWriter__on_cleanup,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  (RogueCleanUpFn) RogueZip__on_cleanup,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  (RogueCleanUpFn) RogueWeakReference__on_cleanup,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0
};

RogueToStringFn Rogue_to_string_fn_table[] =
{
  (RogueToStringFn) RogueObject__to_String,
  0,
  0,
  (RogueToStringFn) RogueStringBuilder__to_String,
  (RogueToStringFn) RogueByte_List__to_String,
  (RogueToStringFn) RogueObject__to_String,
  0,
  0,
  0,
  0,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueStringBuilder_List__to_String,
  0,
  0,
  (RogueToStringFn) Rogue_Function____List__to_String,
  (RogueToStringFn) RogueObject__to_String,
  0,
  0,
  (RogueToStringFn) RogueStackTrace__to_String,
  (RogueToStringFn) RogueString_List__to_String,
  0,
  0,
  0,
  0,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueCharacter_List__to_String,
  0,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueFile__to_String,
  (RogueToStringFn) RogueInt32_List__to_String,
  0,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueUndefinedValue__to_String,
  (RogueToStringFn) RogueNullValue__to_String,
  (RogueToStringFn) RogueValueTable__to_String,
  (RogueToStringFn) RogueTable_String_Value___to_String,
  0,
  (RogueToStringFn) RogueTableEntry_String_Value___to_String,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueTable_String_String___to_String,
  (RogueToStringFn) RogueTable_String_String___to_String,
  0,
  (RogueToStringFn) RogueTableEntry_String_String___to_String,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueStringValue__to_String,
  (RogueToStringFn) RogueReal64Value__to_String,
  (RogueToStringFn) RogueLogicalValue__to_String,
  (RogueToStringFn) RogueValueList__to_String,
  (RogueToStringFn) RogueValue_List__to_String,
  0,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueScanner__to_String,
  0,
  (RogueToStringFn) RogueTable_String_String___to_String,
  (RogueToStringFn) RogueObject__to_String,
  0,
  (RogueToStringFn) RogueConsole__to_String,
  0,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueConsoleEvent_List__to_String,
  0,
  (RogueToStringFn) RogueFileReader__to_String,
  0,
  (RogueToStringFn) RogueLineReader__to_String,
  0,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueProcessResult__to_String,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueWindowsProcessReader__to_String,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RoguePosixProcessReader__to_String,
  (RogueToStringFn) RogueFDReader__to_String,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueObject__to_String,
  0,
  (RogueToStringFn) RogueGenericListRewriter_List__to_String,
  0,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RoguePlatforms__to_String,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueExtendedASCIIReader__to_String,
  (RogueToStringFn) RogueUTF8Reader__to_String,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueObject__to_String,
  (RogueToStringFn) RogueObject__to_String,
  0,
  (RogueToStringFn) RogueException__to_String,
  (RogueToStringFn) RogueException__to_String,
  (RogueToStringFn) RogueException__to_String,
  (RogueToStringFn) RogueException__to_String,
  (RogueToStringFn) RogueException__to_String,
  (RogueToStringFn) RogueException__to_String,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0
};

RogueTraceFn Rogue_trace_fn_table[] =
{
  RogueGlobal_trace,
  0,
  0,
  RogueStringBuilder_trace,
  RogueByte_List_trace,
  RogueObject_trace,
  0,
  RogueObject_trace,
  RogueArray_trace,
  0,
  RogueStringBuilderPool_trace,
  RogueStringBuilder_List_trace,
  RogueArray_trace,
  0,
  Rogue_Function____List_trace,
  RogueObject_trace,
  RogueObject_trace,
  RogueObject_trace,
  RogueStackTrace_trace,
  RogueString_List_trace,
  RogueObject_trace,
  0,
  0,
  0,
  RogueObject_trace,
  RogueCharacter_List_trace,
  RogueObject_trace,
  RogueObject_trace,
  RogueFile_trace,
  RogueInt32_List_trace,
  RogueObject_trace,
  RogueObject_trace,
  RogueObject_trace,
  RogueObject_trace,
  RogueValueTable_trace,
  RogueTable_String_Value__trace,
  RogueArray_trace,
  RogueTableEntry_String_Value__trace,
  RogueObject_trace,
  RogueObject_trace,
  RogueSystemEnvironment_trace,
  RogueStringTable_String__trace,
  RogueTable_String_String__trace,
  RogueArray_trace,
  RogueTableEntry_String_String__trace,
  RogueObject_trace,
  RogueObject_trace,
  RogueObject_trace,
  RogueStringValue_trace,
  RogueObject_trace,
  RogueObject_trace,
  RogueValueList_trace,
  RogueValue_List_trace,
  RogueObject_trace,
  RogueObject_trace,
  RogueJSONParser_trace,
  RogueScanner_trace,
  0,
  RogueStringConsolidationTable_trace,
  RogueFileWriter_trace,
  0,
  RogueConsole_trace,
  0,
  RogueConsoleErrorPrinter_trace,
  RogueConsoleEvent_List_trace,
  RogueObject_trace,
  RogueFileReader_trace,
  0,
  RogueLineReader_trace,
  0,
  RogueObject_trace,
  RogueProcess_trace,
  RogueProcessResult_trace,
  RogueWindowsProcess_trace,
  RogueWindowsProcessReader_trace,
  RogueWindowsProcessWriter_trace,
  RoguePosixProcess_trace,
  RoguePosixProcessReader_trace,
  RogueFDReader_trace,
  RogueFDWriter_trace,
  RogueMorlock_trace,
  RogueObject_trace,
  RogueObject_trace,
  RogueFunction_868_trace,
  RoguePackage_trace,
  RoguePackageInfo_trace,
  RogueFileListing_trace,
  RogueObject_trace,
  RogueFileListing_collect_String__trace,
  RogueObject_trace,
  RogueListRewriter_String__trace,
  RogueObject_trace,
  0,
  RogueGenericListRewriter_List_trace,
  RogueObject_trace,
  RogueObject_trace,
  RogueObject_trace,
  RogueObject_trace,
  RogueObject_trace,
  RoguePlatforms_trace,
  RogueObject_trace,
  RogueObject_trace,
  RogueObject_trace,
  RogueZip_trace,
  RogueObject_trace,
  RogueObject_trace,
  RogueObject_trace,
  RogueFunction_1041_trace,
  RogueFunction_1043_trace,
  RogueExtendedASCIIReader_trace,
  RogueUTF8Reader_trace,
  RogueObject_trace,
  RogueWeakReference_trace,
  RogueFunction_2437_trace,
  RogueObject_trace,
  RogueException_trace,
  RogueError_trace,
  RogueOutOfBoundsError_trace,
  RogueJSONParseError_trace,
  RogueIOError_trace,
  RoguePackageError_trace,
  0,
  RogueTableKeysIterator_String_Value__trace,
  RogueOptionalString_trace,
  0,
  0,
  0,
  0,
  0,
  0,
  RogueTableKeysIterator_String_String__trace,
  RogueFilePattern_trace,
  RogueOptionalFilePattern_trace,
  0,
  RogueVersionNumber_trace,
  RogueBest_String__trace,
  RogueZipEntry_trace,
  0,
  0
};

void Rogue_trace()
{
  void* link;
  int i;

  // Trace GLOBAL PROPERTIES
  if ((link=RogueStringBuilder_work_bytes)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=RogueStringBuilder_pool)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=RogueSystem_command_line_arguments)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=RogueSystem_executable_filepath)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=RogueStringValue_empty_string)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=RogueLogicalValue_true_value)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=RogueLogicalValue_false_value)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=RogueListRewriter_String__pool)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=RogueStringEncoding_names)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=RogueStringEncoding_values)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=RogueConsoleEventType_names)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=RogueConsoleEventType_values)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=RogueZipEntry_buffer)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=RogueUnixConsoleMouseEventType_names)) ((RogueObject*)link)->type->trace_fn( link );
  if ((link=RogueUnixConsoleMouseEventType_values)) ((RogueObject*)link)->type->trace_fn( link );

  // Trace Class objects and singletons
  for (i=Rogue_type_count; --i>=0; )
  {
    RogueType* type = &Rogue_types[i];
    {
      auto singleton = ROGUE_GET_SINGLETON(type);
      if (singleton) singleton->type->trace_fn( singleton );
    }
  }
}

const void* Rogue_dynamic_method_table[] =
{
  (void*) (ROGUEM0) RogueGlobal__init_object, // Global
  (void*) (ROGUEM0) RogueGlobal__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueGlobal__type_name,
  0, // BufferedPrintWriter<<global_output_buffer>>.close() // BufferedPrintWriter<<global_output_buffer>>
  0, // PrintWriter.close() // PrintWriter
  (void*) (ROGUEM0) RogueStringBuilder__init_object, // StringBuilder
  (void*) (ROGUEM0) RogueStringBuilder__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // StringBuilder.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueStringBuilder__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueStringBuilder__type_name,
  (void*) (ROGUEM0) RogueByte_List__init_object, // Byte[]
  (void*) (ROGUEM0) RogueByte_List__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Byte[].to_value_package()
  (void*) (ROGUEM0) RogueByte_List__to_String,
  0, // Byte[].to_Value()
  0, // Object.type_info()
  0, // Byte[].unpack(Value)
  (void*) (ROGUEM0) RogueByte_List__type_name,
  (void*) (ROGUEM0) RogueGenericList__init_object, // GenericList
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueGenericList__type_name,
  (void*) (ROGUEM1) RogueObject__init_object, // Array<<Byte>>
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueArray_Byte___type_name,
  (void*) (ROGUEM1) RogueObject__init_object, // Array
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueArray__type_name,
  0, // Int32.clamped_low(Int32) // Int32
  (void*) (ROGUEM0) RogueStringBuilderPool__init_object, // StringBuilderPool
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueStringBuilderPool__type_name,
  (void*) (ROGUEM0) RogueStringBuilder_List__init_object, // StringBuilder[]
  (void*) (ROGUEM0) RogueStringBuilder_List__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // StringBuilder[].to_value_package()
  (void*) (ROGUEM0) RogueStringBuilder_List__to_String,
  0, // StringBuilder[].to_Value()
  0, // Object.type_info()
  0, // StringBuilder[].unpack(Value)
  (void*) (ROGUEM0) RogueStringBuilder_List__type_name,
  (void*) (ROGUEM1) RogueObject__init_object, // Array<<StringBuilder>>
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueArray_StringBuilder___type_name,
  0, // Logical.to_String() // Logical
  (void*) (ROGUEM0) Rogue_Function____List__init_object, // (Function())[]
  (void*) (ROGUEM0) Rogue_Function____List__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // (Function())[].to_value_package()
  (void*) (ROGUEM0) Rogue_Function____List__to_String,
  0, // (Function())[].to_Value()
  0, // Object.type_info()
  0, // (Function())[].unpack(Value)
  (void*) (ROGUEM0) Rogue_Function____List__type_name,
  (void*) (ROGUEM0) Rogue_Function_____init_object, // (Function())
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) Rogue_Function_____type_name,
  (void*) (ROGUEM1) Rogue_Function_____call,
  (void*) (ROGUEM1) RogueObject__init_object, // Array<<(Function())>>
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueArray__Function______type_name,
  (void*) (ROGUEM1) RogueObject__init_object, // String
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  (void*) (ROGUEM2) RogueString__hash_code,
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueString__to_String,
  0, // String.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueString__type_name,
  (void*) (ROGUEM0) RogueStackTrace__init_object, // StackTrace
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueStackTrace__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueStackTrace__type_name,
  (void*) (ROGUEM0) RogueString_List__init_object, // String[]
  (void*) (ROGUEM0) RogueString_List__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // String[].to_value_package()
  (void*) (ROGUEM0) RogueString_List__to_String,
  0, // String[].to_Value()
  0, // Object.type_info()
  0, // String[].unpack(Value)
  (void*) (ROGUEM0) RogueString_List__type_name,
  (void*) (ROGUEM1) RogueObject__init_object, // Array<<String>>
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueArray_String___type_name,
  0, // Real64.decimal_digit_count() // Real64
  0, // Int64.print_in_power2_base(Int32,Int32,PrintWriter) // Int64
  0, // Character.is_alphanumeric() // Character
  (void*) (ROGUEM0) RogueValue__init_object, // Value
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Value.operator==(Object)
  0, // Value.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Value.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueValue__type_name,
  0, // Value.add(Int32)
  0, // Value.add(Int64)
  0, // Value.add(Logical)
  0, // Value.add(Real64)
  0, // Value.add(Object)
  0, // Value.add(String)
  0, // Value.add(Value)
  0, // Value.add(Value[])
  0, // Value.add_all(Value)
  (void*) (ROGUEM3) RogueValue__at__Int32,
  0, // Value.clear()
  0, // Value.cloned()
  0, // Value.apply((Function(Value)->Value))
  0, // Value.contains(String)
  0, // Value.contains(Value)
  0, // Value.contains((Function(Value)->Logical))
  (void*) (ROGUEM2) RogueValue__count,
  0, // Value.count((Function(Value)->Logical))
  0, // Value.compressed()
  0, // Value.datatype()
  0, // Value.decompressed()
  0, // Value.decode_indexed(ValueIDLookupTable)
  0, // Value.encode_indexed(ValueIDTableBuilder)
  0, // Value.ensure_list(String)
  0, // Value.ensure_table(String)
  0, // Value.first()
  (void*) (ROGUEM4) RogueValue__first___Function_Value_RETURNSLogical_,
  (void*) (ROGUEM3) RogueValue__get__Int32,
  (void*) (ROGUEM4) RogueValue__get__String,
  0, // Value.get((Function(Value)->Logical))
  0, // Value.insert(Int32,Int32)
  0, // Value.insert(Int64,Int32)
  0, // Value.insert(Logical,Int32)
  0, // Value.insert(Real64,Int32)
  0, // Value.insert(Object,Int32)
  0, // Value.insert(String,Int32)
  0, // Value.insert(Value,Int32)
  0, // Value.insert(Value[],Int32)
  0, // Value.insert_all(Value,Int32)
  (void*) (ROGUEM5) RogueValue__is_collection,
  0, // Value.is_complex()
  0, // Value.is_empty()
  0, // Value.is_false()
  0, // Value.is_int32()
  0, // Value.is_int64()
  (void*) (ROGUEM5) RogueValue__is_list,
  (void*) (ROGUEM5) RogueValue__is_logical,
  0, // Value.is_null()
  (void*) (ROGUEM5) RogueValue__is_non_null,
  0, // Value.is_number()
  0, // Value.is_object()
  0, // Value.is_real64()
  0, // Value.is_string()
  0, // Value.is_table()
  0, // Value.is_undefined()
  0, // Value.keys()
  0, // Value.last()
  0, // Value.last((Function(Value)->Logical))
  0, // Value.locate(Value)
  0, // Value.locate((Function(Value)->Logical))
  0, // Value.locate_last(Value)
  0, // Value.locate_last((Function(Value)->Logical))
  0, // Value.operator==(Value)
  0, // Value.operator==(Byte)
  0, // Value.operator==(Character)
  0, // Value.operator==(Int32)
  0, // Value.operator==(Int64)
  0, // Value.operator==(Logical)
  0, // Value.operator==(Real64)
  0, // Value.operator==(Real32)
  0, // Value.operator==(String)
  0, // Value.operator<(Value)
  0, // Value.operator<(Byte)
  0, // Value.operator<(Character)
  0, // Value.operator<(Int32)
  0, // Value.operator<(Int64)
  0, // Value.operator<(Logical)
  0, // Value.operator<(Real64)
  0, // Value.operator<(Real32)
  0, // Value.operator<(Object)
  0, // Value.operator<(String)
  0, // Value.operator-()
  0, // Value.operator+(Value)
  0, // Value.operator-(Value)
  0, // Value.operator*(Value)
  0, // Value.operator/(Value)
  0, // Value.operator%(Value)
  0, // Value.operator^(Value)
  0, // Value.operator+(Real64)
  0, // Value.operator-(Real64)
  0, // Value.operator*(Real64)
  0, // Value.operator/(Real64)
  0, // Value.operator%(Real64)
  0, // Value.operator^(Real64)
  0, // Value.operator+(Int64)
  0, // Value.operator-(Int64)
  0, // Value.operator*(Int64)
  0, // Value.operator/(Int64)
  0, // Value.operator%(Int64)
  0, // Value.operator^(Int64)
  0, // Value.operator+(Int32)
  0, // Value.operator-(Int32)
  0, // Value.operator*(Int32)
  0, // Value.operator/(Int32)
  0, // Value.operator%(Int32)
  0, // Value.operator^(Int32)
  0, // Value.operator+(String)
  0, // Value.operator*(String)
  0, // Value.remove(Value)
  0, // Value.remove(String)
  0, // Value.remove((Function(Value)->Logical))
  0, // Value.remove_at(Int32)
  0, // Value.remove_first()
  0, // Value.remove_last()
  0, // Value.reserve(Int32)
  0, // Value.rest(Value)
  0, // Value.save(File,Logical,Logical)
  0, // Value.set(Int32,Int32)
  0, // Value.set(Int32,Int64)
  0, // Value.set(Int32,Logical)
  0, // Value.set(Int32,Real64)
  0, // Value.set(Int32,Object)
  0, // Value.set(Int32,String)
  0, // Value.set(Int32,Value)
  0, // Value.set(String,Int32)
  0, // Value.set(String,Int64)
  0, // Value.set(String,Logical)
  0, // Value.set(String,Real64)
  0, // Value.set(String,Object)
  0, // Value.set(String,String)
  0, // Value.set(Value,Value)
  (void*) (ROGUEM6) RogueValue__set__String_Value,
  0, // Value.sort((Function(Value,Value)->Logical))
  0, // Value.sorted((Function(Value,Value)->Logical))
  0, // Value.to_Byte()
  0, // Value.to_Character()
  (void*) (ROGUEM7) RogueValue__to_Int64,
  (void*) (ROGUEM2) RogueValue__to_Int32,
  (void*) (ROGUEM5) RogueValue__to_Logical,
  0, // Value.to_Real64()
  0, // Value.to_Real32()
  0, // Value.to_Object()
  0, // Value.to_VarArgs()
  0, // Value.to_json(Int32)
  0, // Value.to_json(Logical,Logical)
  0, // Value.to_json(StringBuilder,Logical,Logical)
  (void*) (ROGUEM8) RogueValue__to_json__StringBuilder_Int32,
  (void*) (ROGUEM0) RogueCharacter_List__init_object, // Character[]
  (void*) (ROGUEM0) RogueCharacter_List__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Character[].to_value_package()
  (void*) (ROGUEM0) RogueCharacter_List__to_String,
  0, // Character[].to_Value()
  0, // Object.type_info()
  0, // Character[].unpack(Value)
  (void*) (ROGUEM0) RogueCharacter_List__type_name,
  (void*) (ROGUEM1) RogueObject__init_object, // Array<<Character>>
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueArray_Character___type_name,
  (void*) (ROGUEM0) Rogue_Function_Value_RETURNSLogical___init_object, // (Function(Value)->Logical)
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) Rogue_Function_Value_RETURNSLogical___type_name,
  (void*) (ROGUEM9) Rogue_Function_Value_RETURNSLogical___call__Value,
  (void*) (ROGUEM0) RogueFile__init_object, // File
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueFile__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueFile__type_name,
  (void*) (ROGUEM0) RogueInt32_List__init_object, // Int32[]
  (void*) (ROGUEM0) RogueInt32_List__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Int32[].to_value_package()
  (void*) (ROGUEM0) RogueInt32_List__to_String,
  0, // Int32[].to_Value()
  0, // Object.type_info()
  0, // Int32[].unpack(Value)
  (void*) (ROGUEM0) RogueInt32_List__type_name,
  (void*) (ROGUEM1) RogueObject__init_object, // Array<<Int32>>
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueArray_Int32___type_name,
  (void*) (ROGUEM0) RogueRuntime__init_object, // Runtime
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueRuntime__type_name,
  (void*) (ROGUEM0) RogueUndefinedValue__init_object, // UndefinedValue
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Value.operator==(Object)
  0, // Value.to_value_package()
  (void*) (ROGUEM0) RogueUndefinedValue__to_String,
  0, // Value.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueUndefinedValue__type_name,
  0, // Value.add(Int32)
  0, // Value.add(Int64)
  0, // Value.add(Logical)
  0, // Value.add(Real64)
  0, // Value.add(Object)
  0, // Value.add(String)
  0, // Value.add(Value)
  0, // Value.add(Value[])
  0, // Value.add_all(Value)
  (void*) (ROGUEM3) RogueValue__at__Int32,
  0, // Value.clear()
  0, // Value.cloned()
  0, // Value.apply((Function(Value)->Value))
  0, // Value.contains(String)
  0, // Value.contains(Value)
  0, // Value.contains((Function(Value)->Logical))
  (void*) (ROGUEM2) RogueValue__count,
  0, // Value.count((Function(Value)->Logical))
  0, // Value.compressed()
  0, // Value.datatype()
  0, // Value.decompressed()
  0, // Value.decode_indexed(ValueIDLookupTable)
  0, // Value.encode_indexed(ValueIDTableBuilder)
  0, // Value.ensure_list(String)
  0, // Value.ensure_table(String)
  0, // Value.first()
  (void*) (ROGUEM4) RogueValue__first___Function_Value_RETURNSLogical_,
  (void*) (ROGUEM3) RogueValue__get__Int32,
  (void*) (ROGUEM4) RogueValue__get__String,
  0, // Value.get((Function(Value)->Logical))
  0, // Value.insert(Int32,Int32)
  0, // Value.insert(Int64,Int32)
  0, // Value.insert(Logical,Int32)
  0, // Value.insert(Real64,Int32)
  0, // Value.insert(Object,Int32)
  0, // Value.insert(String,Int32)
  0, // Value.insert(Value,Int32)
  0, // Value.insert(Value[],Int32)
  0, // Value.insert_all(Value,Int32)
  (void*) (ROGUEM5) RogueValue__is_collection,
  0, // Value.is_complex()
  0, // UndefinedValue.is_empty()
  0, // Value.is_false()
  0, // Value.is_int32()
  0, // Value.is_int64()
  (void*) (ROGUEM5) RogueValue__is_list,
  (void*) (ROGUEM5) RogueValue__is_logical,
  0, // NullValue.is_null()
  (void*) (ROGUEM5) RogueNullValue__is_non_null,
  0, // Value.is_number()
  0, // Value.is_object()
  0, // Value.is_real64()
  0, // Value.is_string()
  0, // Value.is_table()
  0, // UndefinedValue.is_undefined()
  0, // Value.keys()
  0, // Value.last()
  0, // Value.last((Function(Value)->Logical))
  0, // Value.locate(Value)
  0, // Value.locate((Function(Value)->Logical))
  0, // Value.locate_last(Value)
  0, // Value.locate_last((Function(Value)->Logical))
  0, // NullValue.operator==(Value)
  0, // Value.operator==(Byte)
  0, // Value.operator==(Character)
  0, // Value.operator==(Int32)
  0, // Value.operator==(Int64)
  0, // Value.operator==(Logical)
  0, // Value.operator==(Real64)
  0, // Value.operator==(Real32)
  0, // Value.operator==(String)
  0, // NullValue.operator<(Value)
  0, // Value.operator<(Byte)
  0, // Value.operator<(Character)
  0, // Value.operator<(Int32)
  0, // Value.operator<(Int64)
  0, // Value.operator<(Logical)
  0, // Value.operator<(Real64)
  0, // Value.operator<(Real32)
  0, // Value.operator<(Object)
  0, // Value.operator<(String)
  0, // Value.operator-()
  0, // Value.operator+(Value)
  0, // Value.operator-(Value)
  0, // Value.operator*(Value)
  0, // Value.operator/(Value)
  0, // Value.operator%(Value)
  0, // Value.operator^(Value)
  0, // Value.operator+(Real64)
  0, // Value.operator-(Real64)
  0, // Value.operator*(Real64)
  0, // Value.operator/(Real64)
  0, // Value.operator%(Real64)
  0, // Value.operator^(Real64)
  0, // Value.operator+(Int64)
  0, // Value.operator-(Int64)
  0, // Value.operator*(Int64)
  0, // Value.operator/(Int64)
  0, // Value.operator%(Int64)
  0, // Value.operator^(Int64)
  0, // Value.operator+(Int32)
  0, // Value.operator-(Int32)
  0, // Value.operator*(Int32)
  0, // Value.operator/(Int32)
  0, // Value.operator%(Int32)
  0, // Value.operator^(Int32)
  0, // Value.operator+(String)
  0, // Value.operator*(String)
  0, // Value.remove(Value)
  0, // Value.remove(String)
  0, // Value.remove((Function(Value)->Logical))
  0, // Value.remove_at(Int32)
  0, // Value.remove_first()
  0, // Value.remove_last()
  0, // Value.reserve(Int32)
  0, // Value.rest(Value)
  0, // Value.save(File,Logical,Logical)
  0, // Value.set(Int32,Int32)
  0, // Value.set(Int32,Int64)
  0, // Value.set(Int32,Logical)
  0, // Value.set(Int32,Real64)
  0, // Value.set(Int32,Object)
  0, // Value.set(Int32,String)
  0, // Value.set(Int32,Value)
  0, // Value.set(String,Int32)
  0, // Value.set(String,Int64)
  0, // Value.set(String,Logical)
  0, // Value.set(String,Real64)
  0, // Value.set(String,Object)
  0, // Value.set(String,String)
  0, // Value.set(Value,Value)
  (void*) (ROGUEM6) RogueValue__set__String_Value,
  0, // Value.sort((Function(Value,Value)->Logical))
  0, // Value.sorted((Function(Value,Value)->Logical))
  0, // Value.to_Byte()
  0, // Value.to_Character()
  (void*) (ROGUEM7) RogueNullValue__to_Int64,
  (void*) (ROGUEM2) RogueValue__to_Int32,
  (void*) (ROGUEM5) RogueValue__to_Logical,
  0, // Value.to_Real64()
  0, // Value.to_Real32()
  0, // Value.to_Object()
  0, // Value.to_VarArgs()
  0, // Value.to_json(Int32)
  0, // Value.to_json(Logical,Logical)
  0, // Value.to_json(StringBuilder,Logical,Logical)
  (void*) (ROGUEM8) RogueNullValue__to_json__StringBuilder_Int32,
  (void*) (ROGUEM0) RogueNullValue__init_object, // NullValue
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Value.operator==(Object)
  0, // Value.to_value_package()
  (void*) (ROGUEM0) RogueNullValue__to_String,
  0, // Value.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueNullValue__type_name,
  0, // Value.add(Int32)
  0, // Value.add(Int64)
  0, // Value.add(Logical)
  0, // Value.add(Real64)
  0, // Value.add(Object)
  0, // Value.add(String)
  0, // Value.add(Value)
  0, // Value.add(Value[])
  0, // Value.add_all(Value)
  (void*) (ROGUEM3) RogueValue__at__Int32,
  0, // Value.clear()
  0, // Value.cloned()
  0, // Value.apply((Function(Value)->Value))
  0, // Value.contains(String)
  0, // Value.contains(Value)
  0, // Value.contains((Function(Value)->Logical))
  (void*) (ROGUEM2) RogueValue__count,
  0, // Value.count((Function(Value)->Logical))
  0, // Value.compressed()
  0, // Value.datatype()
  0, // Value.decompressed()
  0, // Value.decode_indexed(ValueIDLookupTable)
  0, // Value.encode_indexed(ValueIDTableBuilder)
  0, // Value.ensure_list(String)
  0, // Value.ensure_table(String)
  0, // Value.first()
  (void*) (ROGUEM4) RogueValue__first___Function_Value_RETURNSLogical_,
  (void*) (ROGUEM3) RogueValue__get__Int32,
  (void*) (ROGUEM4) RogueValue__get__String,
  0, // Value.get((Function(Value)->Logical))
  0, // Value.insert(Int32,Int32)
  0, // Value.insert(Int64,Int32)
  0, // Value.insert(Logical,Int32)
  0, // Value.insert(Real64,Int32)
  0, // Value.insert(Object,Int32)
  0, // Value.insert(String,Int32)
  0, // Value.insert(Value,Int32)
  0, // Value.insert(Value[],Int32)
  0, // Value.insert_all(Value,Int32)
  (void*) (ROGUEM5) RogueValue__is_collection,
  0, // Value.is_complex()
  0, // Value.is_empty()
  0, // Value.is_false()
  0, // Value.is_int32()
  0, // Value.is_int64()
  (void*) (ROGUEM5) RogueValue__is_list,
  (void*) (ROGUEM5) RogueValue__is_logical,
  0, // NullValue.is_null()
  (void*) (ROGUEM5) RogueNullValue__is_non_null,
  0, // Value.is_number()
  0, // Value.is_object()
  0, // Value.is_real64()
  0, // Value.is_string()
  0, // Value.is_table()
  0, // Value.is_undefined()
  0, // Value.keys()
  0, // Value.last()
  0, // Value.last((Function(Value)->Logical))
  0, // Value.locate(Value)
  0, // Value.locate((Function(Value)->Logical))
  0, // Value.locate_last(Value)
  0, // Value.locate_last((Function(Value)->Logical))
  0, // NullValue.operator==(Value)
  0, // Value.operator==(Byte)
  0, // Value.operator==(Character)
  0, // Value.operator==(Int32)
  0, // Value.operator==(Int64)
  0, // Value.operator==(Logical)
  0, // Value.operator==(Real64)
  0, // Value.operator==(Real32)
  0, // Value.operator==(String)
  0, // NullValue.operator<(Value)
  0, // Value.operator<(Byte)
  0, // Value.operator<(Character)
  0, // Value.operator<(Int32)
  0, // Value.operator<(Int64)
  0, // Value.operator<(Logical)
  0, // Value.operator<(Real64)
  0, // Value.operator<(Real32)
  0, // Value.operator<(Object)
  0, // Value.operator<(String)
  0, // Value.operator-()
  0, // Value.operator+(Value)
  0, // Value.operator-(Value)
  0, // Value.operator*(Value)
  0, // Value.operator/(Value)
  0, // Value.operator%(Value)
  0, // Value.operator^(Value)
  0, // Value.operator+(Real64)
  0, // Value.operator-(Real64)
  0, // Value.operator*(Real64)
  0, // Value.operator/(Real64)
  0, // Value.operator%(Real64)
  0, // Value.operator^(Real64)
  0, // Value.operator+(Int64)
  0, // Value.operator-(Int64)
  0, // Value.operator*(Int64)
  0, // Value.operator/(Int64)
  0, // Value.operator%(Int64)
  0, // Value.operator^(Int64)
  0, // Value.operator+(Int32)
  0, // Value.operator-(Int32)
  0, // Value.operator*(Int32)
  0, // Value.operator/(Int32)
  0, // Value.operator%(Int32)
  0, // Value.operator^(Int32)
  0, // Value.operator+(String)
  0, // Value.operator*(String)
  0, // Value.remove(Value)
  0, // Value.remove(String)
  0, // Value.remove((Function(Value)->Logical))
  0, // Value.remove_at(Int32)
  0, // Value.remove_first()
  0, // Value.remove_last()
  0, // Value.reserve(Int32)
  0, // Value.rest(Value)
  0, // Value.save(File,Logical,Logical)
  0, // Value.set(Int32,Int32)
  0, // Value.set(Int32,Int64)
  0, // Value.set(Int32,Logical)
  0, // Value.set(Int32,Real64)
  0, // Value.set(Int32,Object)
  0, // Value.set(Int32,String)
  0, // Value.set(Int32,Value)
  0, // Value.set(String,Int32)
  0, // Value.set(String,Int64)
  0, // Value.set(String,Logical)
  0, // Value.set(String,Real64)
  0, // Value.set(String,Object)
  0, // Value.set(String,String)
  0, // Value.set(Value,Value)
  (void*) (ROGUEM6) RogueValue__set__String_Value,
  0, // Value.sort((Function(Value,Value)->Logical))
  0, // Value.sorted((Function(Value,Value)->Logical))
  0, // Value.to_Byte()
  0, // Value.to_Character()
  (void*) (ROGUEM7) RogueNullValue__to_Int64,
  (void*) (ROGUEM2) RogueValue__to_Int32,
  (void*) (ROGUEM5) RogueValue__to_Logical,
  0, // Value.to_Real64()
  0, // Value.to_Real32()
  0, // Value.to_Object()
  0, // Value.to_VarArgs()
  0, // Value.to_json(Int32)
  0, // Value.to_json(Logical,Logical)
  0, // Value.to_json(StringBuilder,Logical,Logical)
  (void*) (ROGUEM8) RogueNullValue__to_json__StringBuilder_Int32,
  (void*) (ROGUEM0) RogueValueTable__init_object, // ValueTable
  (void*) (ROGUEM0) RogueValueTable__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Value.operator==(Object)
  0, // Value.to_value_package()
  (void*) (ROGUEM0) RogueValueTable__to_String,
  0, // Value.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueValueTable__type_name,
  0, // Value.add(Int32)
  0, // Value.add(Int64)
  0, // Value.add(Logical)
  0, // Value.add(Real64)
  0, // Value.add(Object)
  0, // Value.add(String)
  0, // Value.add(Value)
  0, // Value.add(Value[])
  0, // ValueTable.add_all(Value)
  (void*) (ROGUEM3) RogueValueTable__at__Int32,
  0, // ValueTable.clear()
  0, // ValueTable.cloned()
  0, // ValueTable.apply((Function(Value)->Value))
  0, // ValueTable.contains(String)
  0, // ValueTable.contains(Value)
  0, // Value.contains((Function(Value)->Logical))
  (void*) (ROGUEM2) RogueValueTable__count,
  0, // ValueTable.count((Function(Value)->Logical))
  0, // Value.compressed()
  0, // ValueTable.datatype()
  0, // Value.decompressed()
  0, // ValueTable.decode_indexed(ValueIDLookupTable)
  0, // ValueTable.encode_indexed(ValueIDTableBuilder)
  0, // ValueTable.ensure_list(String)
  0, // ValueTable.ensure_table(String)
  0, // Value.first()
  (void*) (ROGUEM4) RogueValueTable__first___Function_Value_RETURNSLogical_,
  (void*) (ROGUEM3) RogueValueTable__get__Int32,
  (void*) (ROGUEM4) RogueValueTable__get__String,
  0, // ValueTable.get((Function(Value)->Logical))
  0, // Value.insert(Int32,Int32)
  0, // Value.insert(Int64,Int32)
  0, // Value.insert(Logical,Int32)
  0, // Value.insert(Real64,Int32)
  0, // Value.insert(Object,Int32)
  0, // Value.insert(String,Int32)
  0, // Value.insert(Value,Int32)
  0, // Value.insert(Value[],Int32)
  0, // Value.insert_all(Value,Int32)
  (void*) (ROGUEM5) RogueValueTable__is_collection,
  0, // Value.is_complex()
  0, // ValueTable.is_empty()
  0, // Value.is_false()
  0, // Value.is_int32()
  0, // Value.is_int64()
  (void*) (ROGUEM5) RogueValue__is_list,
  (void*) (ROGUEM5) RogueValue__is_logical,
  0, // Value.is_null()
  (void*) (ROGUEM5) RogueValue__is_non_null,
  0, // Value.is_number()
  0, // Value.is_object()
  0, // Value.is_real64()
  0, // Value.is_string()
  0, // ValueTable.is_table()
  0, // Value.is_undefined()
  (void*) (ROGUEM10) RogueValueTable__keys,
  0, // Value.last()
  0, // ValueTable.last((Function(Value)->Logical))
  0, // ValueTable.locate(Value)
  0, // ValueTable.locate((Function(Value)->Logical))
  0, // ValueTable.locate_last(Value)
  0, // ValueTable.locate_last((Function(Value)->Logical))
  0, // Value.operator==(Value)
  0, // Value.operator==(Byte)
  0, // Value.operator==(Character)
  0, // Value.operator==(Int32)
  0, // Value.operator==(Int64)
  0, // Value.operator==(Logical)
  0, // Value.operator==(Real64)
  0, // Value.operator==(Real32)
  0, // Value.operator==(String)
  0, // Value.operator<(Value)
  0, // Value.operator<(Byte)
  0, // Value.operator<(Character)
  0, // Value.operator<(Int32)
  0, // Value.operator<(Int64)
  0, // Value.operator<(Logical)
  0, // Value.operator<(Real64)
  0, // Value.operator<(Real32)
  0, // Value.operator<(Object)
  0, // Value.operator<(String)
  0, // Value.operator-()
  0, // Value.operator+(Value)
  0, // Value.operator-(Value)
  0, // Value.operator*(Value)
  0, // Value.operator/(Value)
  0, // Value.operator%(Value)
  0, // Value.operator^(Value)
  0, // Value.operator+(Real64)
  0, // Value.operator-(Real64)
  0, // Value.operator*(Real64)
  0, // Value.operator/(Real64)
  0, // Value.operator%(Real64)
  0, // Value.operator^(Real64)
  0, // Value.operator+(Int64)
  0, // Value.operator-(Int64)
  0, // Value.operator*(Int64)
  0, // Value.operator/(Int64)
  0, // Value.operator%(Int64)
  0, // Value.operator^(Int64)
  0, // Value.operator+(Int32)
  0, // Value.operator-(Int32)
  0, // Value.operator*(Int32)
  0, // Value.operator/(Int32)
  0, // Value.operator%(Int32)
  0, // Value.operator^(Int32)
  0, // Value.operator+(String)
  0, // Value.operator*(String)
  0, // ValueTable.remove(Value)
  0, // ValueTable.remove(String)
  0, // ValueTable.remove((Function(Value)->Logical))
  0, // ValueTable.remove_at(Int32)
  0, // Value.remove_first()
  0, // Value.remove_last()
  0, // Value.reserve(Int32)
  0, // Value.rest(Value)
  0, // Value.save(File,Logical,Logical)
  0, // Value.set(Int32,Int32)
  0, // Value.set(Int32,Int64)
  0, // Value.set(Int32,Logical)
  0, // Value.set(Int32,Real64)
  0, // Value.set(Int32,Object)
  0, // Value.set(Int32,String)
  0, // ValueTable.set(Int32,Value)
  0, // Value.set(String,Int32)
  0, // Value.set(String,Int64)
  0, // Value.set(String,Logical)
  0, // Value.set(String,Real64)
  0, // Value.set(String,Object)
  0, // Value.set(String,String)
  0, // Value.set(Value,Value)
  (void*) (ROGUEM6) RogueValueTable__set__String_Value,
  0, // ValueTable.sort((Function(Value,Value)->Logical))
  0, // Value.sorted((Function(Value,Value)->Logical))
  0, // Value.to_Byte()
  0, // Value.to_Character()
  (void*) (ROGUEM7) RogueValue__to_Int64,
  (void*) (ROGUEM2) RogueValue__to_Int32,
  (void*) (ROGUEM5) RogueValueTable__to_Logical,
  0, // Value.to_Real64()
  0, // Value.to_Real32()
  0, // Value.to_Object()
  0, // Value.to_VarArgs()
  0, // Value.to_json(Int32)
  0, // Value.to_json(Logical,Logical)
  0, // Value.to_json(StringBuilder,Logical,Logical)
  (void*) (ROGUEM8) RogueValueTable__to_json__StringBuilder_Int32,
  (void*) (ROGUEM0) RogueTable_String_Value___init_object, // Table<<String,Value>>
  (void*) (ROGUEM0) RogueTable_String_Value___init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Table<<String,Value>>.to_value_package()
  (void*) (ROGUEM0) RogueTable_String_Value___to_String,
  0, // Table<<String,Value>>.to_Value()
  0, // Object.type_info()
  0, // Table<<String,Value>>.unpack(Value)
  (void*) (ROGUEM0) RogueTable_String_Value___type_name,
  (void*) (ROGUEM1) RogueObject__init_object, // Array<<TableEntry<<String,Value>>>>
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueArray_TableEntry_String_Value____type_name,
  (void*) (ROGUEM0) RogueTableEntry_String_Value___init_object, // TableEntry<<String,Value>>
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueTableEntry_String_Value___to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueTableEntry_String_Value___type_name,
  (void*) (ROGUEM0) Rogue_Function_TableEntry_String_Value__TableEntry_String_Value__RETURNSLogical___init_object, // (Function(TableEntry<<String,Value>>,TableEntry<<String,Value>>)->Logical)
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) Rogue_Function_TableEntry_String_Value__TableEntry_String_Value__RETURNSLogical___type_name,
  (void*) (ROGUEM11) Rogue_Function_TableEntry_String_Value__TableEntry_String_Value__RETURNSLogical___call__TableEntry_String_Value__TableEntry_String_Value_,
  (void*) (ROGUEM0) RogueSystem__init_object, // System
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueSystem__type_name,
  (void*) (ROGUEM0) RogueSystemEnvironment__init_object, // SystemEnvironment
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueSystemEnvironment__type_name,
  (void*) (ROGUEM0) RogueStringTable_String___init_object, // StringTable<<String>>
  (void*) (ROGUEM0) RogueTable_String_String___init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Table<<String,String>>.to_value_package()
  (void*) (ROGUEM0) RogueTable_String_String___to_String,
  0, // Table<<String,String>>.to_Value()
  0, // Object.type_info()
  0, // Table<<String,String>>.unpack(Value)
  (void*) (ROGUEM0) RogueStringTable_String___type_name,
  0, // Table<<String,String>>.init(Int32)
  0, // Table<<String,String>>.init(Table<<String,String>>)
  0, // Table<<String,String>>.add(Table<<String,String>>)
  0, // Table<<String,String>>.at(Int32)
  0, // Table<<String,String>>.clear()
  0, // Table<<String,String>>.cloned()
  0, // Table<<String,String>>.contains(String)
  0, // Table<<String,String>>.contains((Function(String)->Logical))
  0, // Table<<String,String>>.count((Function(Value)->Logical))
  0, // Table<<String,String>>.discard((Function(TableEntry<<String,String>>)->Logical))
  0, // Table<<String,String>>.entries()
  0, // Table<<String,String>>.entry_at(Int32)
  0, // Table<<String,String>>.is_empty()
  0, // Table<<String,String>>.find(String)
  0, // Table<<String,String>>.first()
  0, // Table<<String,String>>.first((Function(String)->Logical))
  (void*) (ROGUEM4) RogueTable_String_String___get__String,
  (void*) (ROGUEM0) RogueTable_String_String___init_object, // Table<<String,String>>
  (void*) (ROGUEM0) RogueTable_String_String___init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Table<<String,String>>.to_value_package()
  (void*) (ROGUEM0) RogueTable_String_String___to_String,
  0, // Table<<String,String>>.to_Value()
  0, // Object.type_info()
  0, // Table<<String,String>>.unpack(Value)
  (void*) (ROGUEM0) RogueTable_String_String___type_name,
  0, // Table<<String,String>>.init(Int32)
  0, // Table<<String,String>>.init(Table<<String,String>>)
  0, // Table<<String,String>>.add(Table<<String,String>>)
  0, // Table<<String,String>>.at(Int32)
  0, // Table<<String,String>>.clear()
  0, // Table<<String,String>>.cloned()
  0, // Table<<String,String>>.contains(String)
  0, // Table<<String,String>>.contains((Function(String)->Logical))
  0, // Table<<String,String>>.count((Function(Value)->Logical))
  0, // Table<<String,String>>.discard((Function(TableEntry<<String,String>>)->Logical))
  0, // Table<<String,String>>.entries()
  0, // Table<<String,String>>.entry_at(Int32)
  0, // Table<<String,String>>.is_empty()
  0, // Table<<String,String>>.find(String)
  0, // Table<<String,String>>.first()
  0, // Table<<String,String>>.first((Function(String)->Logical))
  (void*) (ROGUEM4) RogueTable_String_String___get__String,
  (void*) (ROGUEM1) RogueObject__init_object, // Array<<TableEntry<<String,String>>>>
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueArray_TableEntry_String_String____type_name,
  (void*) (ROGUEM0) RogueTableEntry_String_String___init_object, // TableEntry<<String,String>>
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueTableEntry_String_String___to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueTableEntry_String_String___type_name,
  (void*) (ROGUEM0) Rogue_Function_TableEntry_String_String__TableEntry_String_String__RETURNSLogical___init_object, // (Function(TableEntry<<String,String>>,TableEntry<<String,String>>)->Logical)
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) Rogue_Function_TableEntry_String_String__TableEntry_String_String__RETURNSLogical___type_name,
  (void*) (ROGUEM0) Rogue_Function_String_RETURNSLogical___init_object, // (Function(String)->Logical)
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) Rogue_Function_String_RETURNSLogical___type_name,
  (void*) (ROGUEM9) Rogue_Function_String_RETURNSLogical___call__String,
  (void*) (ROGUEM0) RogueMath__init_object, // Math
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueMath__type_name,
  (void*) (ROGUEM0) RogueStringValue__init_object, // StringValue
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Value.operator==(Object)
  0, // Value.to_value_package()
  (void*) (ROGUEM0) RogueStringValue__to_String,
  0, // Value.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueStringValue__type_name,
  0, // Value.add(Int32)
  0, // Value.add(Int64)
  0, // Value.add(Logical)
  0, // Value.add(Real64)
  0, // Value.add(Object)
  0, // Value.add(String)
  0, // Value.add(Value)
  0, // Value.add(Value[])
  0, // Value.add_all(Value)
  (void*) (ROGUEM3) RogueValue__at__Int32,
  0, // Value.clear()
  0, // Value.cloned()
  0, // Value.apply((Function(Value)->Value))
  0, // Value.contains(String)
  0, // Value.contains(Value)
  0, // Value.contains((Function(Value)->Logical))
  (void*) (ROGUEM2) RogueStringValue__count,
  0, // Value.count((Function(Value)->Logical))
  0, // Value.compressed()
  0, // StringValue.datatype()
  0, // Value.decompressed()
  0, // StringValue.decode_indexed(ValueIDLookupTable)
  0, // StringValue.encode_indexed(ValueIDTableBuilder)
  0, // Value.ensure_list(String)
  0, // Value.ensure_table(String)
  0, // Value.first()
  (void*) (ROGUEM4) RogueValue__first___Function_Value_RETURNSLogical_,
  (void*) (ROGUEM3) RogueStringValue__get__Int32,
  (void*) (ROGUEM4) RogueValue__get__String,
  0, // Value.get((Function(Value)->Logical))
  0, // Value.insert(Int32,Int32)
  0, // Value.insert(Int64,Int32)
  0, // Value.insert(Logical,Int32)
  0, // Value.insert(Real64,Int32)
  0, // Value.insert(Object,Int32)
  0, // Value.insert(String,Int32)
  0, // Value.insert(Value,Int32)
  0, // Value.insert(Value[],Int32)
  0, // Value.insert_all(Value,Int32)
  (void*) (ROGUEM5) RogueValue__is_collection,
  0, // Value.is_complex()
  0, // StringValue.is_empty()
  0, // StringValue.is_false()
  0, // Value.is_int32()
  0, // Value.is_int64()
  (void*) (ROGUEM5) RogueValue__is_list,
  (void*) (ROGUEM5) RogueValue__is_logical,
  0, // Value.is_null()
  (void*) (ROGUEM5) RogueValue__is_non_null,
  0, // Value.is_number()
  0, // Value.is_object()
  0, // Value.is_real64()
  0, // StringValue.is_string()
  0, // Value.is_table()
  0, // Value.is_undefined()
  0, // Value.keys()
  0, // Value.last()
  0, // Value.last((Function(Value)->Logical))
  0, // Value.locate(Value)
  0, // Value.locate((Function(Value)->Logical))
  0, // Value.locate_last(Value)
  0, // Value.locate_last((Function(Value)->Logical))
  0, // StringValue.operator==(Value)
  0, // Value.operator==(Byte)
  0, // Value.operator==(Character)
  0, // Value.operator==(Int32)
  0, // Value.operator==(Int64)
  0, // Value.operator==(Logical)
  0, // Value.operator==(Real64)
  0, // Value.operator==(Real32)
  0, // Value.operator==(String)
  0, // StringValue.operator<(Value)
  0, // Value.operator<(Byte)
  0, // Value.operator<(Character)
  0, // Value.operator<(Int32)
  0, // Value.operator<(Int64)
  0, // Value.operator<(Logical)
  0, // Value.operator<(Real64)
  0, // Value.operator<(Real32)
  0, // Value.operator<(Object)
  0, // Value.operator<(String)
  0, // Value.operator-()
  0, // StringValue.operator+(Value)
  0, // Value.operator-(Value)
  0, // StringValue.operator*(Value)
  0, // Value.operator/(Value)
  0, // Value.operator%(Value)
  0, // Value.operator^(Value)
  0, // StringValue.operator+(Real64)
  0, // Value.operator-(Real64)
  0, // Value.operator*(Real64)
  0, // Value.operator/(Real64)
  0, // Value.operator%(Real64)
  0, // Value.operator^(Real64)
  0, // Value.operator+(Int64)
  0, // Value.operator-(Int64)
  0, // Value.operator*(Int64)
  0, // Value.operator/(Int64)
  0, // Value.operator%(Int64)
  0, // Value.operator^(Int64)
  0, // Value.operator+(Int32)
  0, // Value.operator-(Int32)
  0, // Value.operator*(Int32)
  0, // Value.operator/(Int32)
  0, // Value.operator%(Int32)
  0, // Value.operator^(Int32)
  0, // StringValue.operator+(String)
  0, // Value.operator*(String)
  0, // Value.remove(Value)
  0, // Value.remove(String)
  0, // Value.remove((Function(Value)->Logical))
  0, // Value.remove_at(Int32)
  0, // Value.remove_first()
  0, // Value.remove_last()
  0, // Value.reserve(Int32)
  0, // Value.rest(Value)
  0, // Value.save(File,Logical,Logical)
  0, // Value.set(Int32,Int32)
  0, // Value.set(Int32,Int64)
  0, // Value.set(Int32,Logical)
  0, // Value.set(Int32,Real64)
  0, // Value.set(Int32,Object)
  0, // Value.set(Int32,String)
  0, // Value.set(Int32,Value)
  0, // Value.set(String,Int32)
  0, // Value.set(String,Int64)
  0, // Value.set(String,Logical)
  0, // Value.set(String,Real64)
  0, // Value.set(String,Object)
  0, // Value.set(String,String)
  0, // Value.set(Value,Value)
  (void*) (ROGUEM6) RogueValue__set__String_Value,
  0, // Value.sort((Function(Value,Value)->Logical))
  0, // Value.sorted((Function(Value,Value)->Logical))
  0, // Value.to_Byte()
  0, // StringValue.to_Character()
  (void*) (ROGUEM7) RogueStringValue__to_Int64,
  (void*) (ROGUEM2) RogueValue__to_Int32,
  (void*) (ROGUEM5) RogueStringValue__to_Logical,
  0, // StringValue.to_Real64()
  0, // Value.to_Real32()
  0, // StringValue.to_Object()
  0, // Value.to_VarArgs()
  0, // Value.to_json(Int32)
  0, // Value.to_json(Logical,Logical)
  0, // Value.to_json(StringBuilder,Logical,Logical)
  (void*) (ROGUEM8) RogueStringValue__to_json__StringBuilder_Int32,
  (void*) (ROGUEM0) RogueReal64Value__init_object, // Real64Value
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Value.operator==(Object)
  0, // Value.to_value_package()
  (void*) (ROGUEM0) RogueReal64Value__to_String,
  0, // Value.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueReal64Value__type_name,
  0, // Value.add(Int32)
  0, // Value.add(Int64)
  0, // Value.add(Logical)
  0, // Value.add(Real64)
  0, // Value.add(Object)
  0, // Value.add(String)
  0, // Value.add(Value)
  0, // Value.add(Value[])
  0, // Value.add_all(Value)
  (void*) (ROGUEM3) RogueValue__at__Int32,
  0, // Value.clear()
  0, // Value.cloned()
  0, // Value.apply((Function(Value)->Value))
  0, // Value.contains(String)
  0, // Value.contains(Value)
  0, // Value.contains((Function(Value)->Logical))
  (void*) (ROGUEM2) RogueValue__count,
  0, // Value.count((Function(Value)->Logical))
  0, // Value.compressed()
  0, // Real64Value.datatype()
  0, // Value.decompressed()
  0, // Value.decode_indexed(ValueIDLookupTable)
  0, // Value.encode_indexed(ValueIDTableBuilder)
  0, // Value.ensure_list(String)
  0, // Value.ensure_table(String)
  0, // Value.first()
  (void*) (ROGUEM4) RogueValue__first___Function_Value_RETURNSLogical_,
  (void*) (ROGUEM3) RogueValue__get__Int32,
  (void*) (ROGUEM4) RogueValue__get__String,
  0, // Value.get((Function(Value)->Logical))
  0, // Value.insert(Int32,Int32)
  0, // Value.insert(Int64,Int32)
  0, // Value.insert(Logical,Int32)
  0, // Value.insert(Real64,Int32)
  0, // Value.insert(Object,Int32)
  0, // Value.insert(String,Int32)
  0, // Value.insert(Value,Int32)
  0, // Value.insert(Value[],Int32)
  0, // Value.insert_all(Value,Int32)
  (void*) (ROGUEM5) RogueValue__is_collection,
  0, // Value.is_complex()
  0, // Value.is_empty()
  0, // Value.is_false()
  0, // Value.is_int32()
  0, // Value.is_int64()
  (void*) (ROGUEM5) RogueValue__is_list,
  (void*) (ROGUEM5) RogueValue__is_logical,
  0, // Value.is_null()
  (void*) (ROGUEM5) RogueValue__is_non_null,
  0, // Real64Value.is_number()
  0, // Value.is_object()
  0, // Real64Value.is_real64()
  0, // Value.is_string()
  0, // Value.is_table()
  0, // Value.is_undefined()
  0, // Value.keys()
  0, // Value.last()
  0, // Value.last((Function(Value)->Logical))
  0, // Value.locate(Value)
  0, // Value.locate((Function(Value)->Logical))
  0, // Value.locate_last(Value)
  0, // Value.locate_last((Function(Value)->Logical))
  0, // Real64Value.operator==(Value)
  0, // Value.operator==(Byte)
  0, // Value.operator==(Character)
  0, // Value.operator==(Int32)
  0, // Value.operator==(Int64)
  0, // Value.operator==(Logical)
  0, // Value.operator==(Real64)
  0, // Value.operator==(Real32)
  0, // Value.operator==(String)
  0, // Real64Value.operator<(Value)
  0, // Value.operator<(Byte)
  0, // Value.operator<(Character)
  0, // Value.operator<(Int32)
  0, // Value.operator<(Int64)
  0, // Value.operator<(Logical)
  0, // Value.operator<(Real64)
  0, // Value.operator<(Real32)
  0, // Value.operator<(Object)
  0, // Value.operator<(String)
  0, // Real64Value.operator-()
  0, // Real64Value.operator+(Value)
  0, // Real64Value.operator-(Value)
  0, // Real64Value.operator*(Value)
  0, // Real64Value.operator/(Value)
  0, // Real64Value.operator%(Value)
  0, // Real64Value.operator^(Value)
  0, // Real64Value.operator+(Real64)
  0, // Real64Value.operator-(Real64)
  0, // Real64Value.operator*(Real64)
  0, // Real64Value.operator/(Real64)
  0, // Real64Value.operator%(Real64)
  0, // Real64Value.operator^(Real64)
  0, // Value.operator+(Int64)
  0, // Value.operator-(Int64)
  0, // Value.operator*(Int64)
  0, // Value.operator/(Int64)
  0, // Value.operator%(Int64)
  0, // Value.operator^(Int64)
  0, // Value.operator+(Int32)
  0, // Value.operator-(Int32)
  0, // Value.operator*(Int32)
  0, // Value.operator/(Int32)
  0, // Value.operator%(Int32)
  0, // Value.operator^(Int32)
  0, // Real64Value.operator+(String)
  0, // Real64Value.operator*(String)
  0, // Value.remove(Value)
  0, // Value.remove(String)
  0, // Value.remove((Function(Value)->Logical))
  0, // Value.remove_at(Int32)
  0, // Value.remove_first()
  0, // Value.remove_last()
  0, // Value.reserve(Int32)
  0, // Value.rest(Value)
  0, // Value.save(File,Logical,Logical)
  0, // Value.set(Int32,Int32)
  0, // Value.set(Int32,Int64)
  0, // Value.set(Int32,Logical)
  0, // Value.set(Int32,Real64)
  0, // Value.set(Int32,Object)
  0, // Value.set(Int32,String)
  0, // Value.set(Int32,Value)
  0, // Value.set(String,Int32)
  0, // Value.set(String,Int64)
  0, // Value.set(String,Logical)
  0, // Value.set(String,Real64)
  0, // Value.set(String,Object)
  0, // Value.set(String,String)
  0, // Value.set(Value,Value)
  (void*) (ROGUEM6) RogueValue__set__String_Value,
  0, // Value.sort((Function(Value,Value)->Logical))
  0, // Value.sorted((Function(Value,Value)->Logical))
  0, // Value.to_Byte()
  0, // Real64Value.to_Character()
  (void*) (ROGUEM7) RogueReal64Value__to_Int64,
  (void*) (ROGUEM2) RogueValue__to_Int32,
  (void*) (ROGUEM5) RogueValue__to_Logical,
  0, // Real64Value.to_Real64()
  0, // Value.to_Real32()
  0, // Value.to_Object()
  0, // Value.to_VarArgs()
  0, // Value.to_json(Int32)
  0, // Value.to_json(Logical,Logical)
  0, // Value.to_json(StringBuilder,Logical,Logical)
  (void*) (ROGUEM8) RogueReal64Value__to_json__StringBuilder_Int32,
  (void*) (ROGUEM0) RogueLogicalValue__init_object, // LogicalValue
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Value.operator==(Object)
  0, // Value.to_value_package()
  (void*) (ROGUEM0) RogueLogicalValue__to_String,
  0, // Value.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueLogicalValue__type_name,
  0, // Value.add(Int32)
  0, // Value.add(Int64)
  0, // Value.add(Logical)
  0, // Value.add(Real64)
  0, // Value.add(Object)
  0, // Value.add(String)
  0, // Value.add(Value)
  0, // Value.add(Value[])
  0, // Value.add_all(Value)
  (void*) (ROGUEM3) RogueValue__at__Int32,
  0, // Value.clear()
  0, // Value.cloned()
  0, // Value.apply((Function(Value)->Value))
  0, // Value.contains(String)
  0, // Value.contains(Value)
  0, // Value.contains((Function(Value)->Logical))
  (void*) (ROGUEM2) RogueValue__count,
  0, // Value.count((Function(Value)->Logical))
  0, // Value.compressed()
  0, // LogicalValue.datatype()
  0, // Value.decompressed()
  0, // Value.decode_indexed(ValueIDLookupTable)
  0, // Value.encode_indexed(ValueIDTableBuilder)
  0, // Value.ensure_list(String)
  0, // Value.ensure_table(String)
  0, // Value.first()
  (void*) (ROGUEM4) RogueValue__first___Function_Value_RETURNSLogical_,
  (void*) (ROGUEM3) RogueValue__get__Int32,
  (void*) (ROGUEM4) RogueValue__get__String,
  0, // Value.get((Function(Value)->Logical))
  0, // Value.insert(Int32,Int32)
  0, // Value.insert(Int64,Int32)
  0, // Value.insert(Logical,Int32)
  0, // Value.insert(Real64,Int32)
  0, // Value.insert(Object,Int32)
  0, // Value.insert(String,Int32)
  0, // Value.insert(Value,Int32)
  0, // Value.insert(Value[],Int32)
  0, // Value.insert_all(Value,Int32)
  (void*) (ROGUEM5) RogueValue__is_collection,
  0, // Value.is_complex()
  0, // Value.is_empty()
  0, // Value.is_false()
  0, // Value.is_int32()
  0, // Value.is_int64()
  (void*) (ROGUEM5) RogueValue__is_list,
  (void*) (ROGUEM5) RogueLogicalValue__is_logical,
  0, // Value.is_null()
  (void*) (ROGUEM5) RogueValue__is_non_null,
  0, // Value.is_number()
  0, // Value.is_object()
  0, // Value.is_real64()
  0, // Value.is_string()
  0, // Value.is_table()
  0, // Value.is_undefined()
  0, // Value.keys()
  0, // Value.last()
  0, // Value.last((Function(Value)->Logical))
  0, // Value.locate(Value)
  0, // Value.locate((Function(Value)->Logical))
  0, // Value.locate_last(Value)
  0, // Value.locate_last((Function(Value)->Logical))
  0, // Value.operator==(Value)
  0, // Value.operator==(Byte)
  0, // Value.operator==(Character)
  0, // Value.operator==(Int32)
  0, // Value.operator==(Int64)
  0, // Value.operator==(Logical)
  0, // Value.operator==(Real64)
  0, // Value.operator==(Real32)
  0, // Value.operator==(String)
  0, // Value.operator<(Value)
  0, // Value.operator<(Byte)
  0, // Value.operator<(Character)
  0, // Value.operator<(Int32)
  0, // Value.operator<(Int64)
  0, // Value.operator<(Logical)
  0, // Value.operator<(Real64)
  0, // Value.operator<(Real32)
  0, // Value.operator<(Object)
  0, // Value.operator<(String)
  0, // Value.operator-()
  0, // Value.operator+(Value)
  0, // Value.operator-(Value)
  0, // Value.operator*(Value)
  0, // Value.operator/(Value)
  0, // Value.operator%(Value)
  0, // Value.operator^(Value)
  0, // Value.operator+(Real64)
  0, // Value.operator-(Real64)
  0, // Value.operator*(Real64)
  0, // Value.operator/(Real64)
  0, // Value.operator%(Real64)
  0, // Value.operator^(Real64)
  0, // Value.operator+(Int64)
  0, // Value.operator-(Int64)
  0, // Value.operator*(Int64)
  0, // Value.operator/(Int64)
  0, // Value.operator%(Int64)
  0, // Value.operator^(Int64)
  0, // Value.operator+(Int32)
  0, // Value.operator-(Int32)
  0, // Value.operator*(Int32)
  0, // Value.operator/(Int32)
  0, // Value.operator%(Int32)
  0, // Value.operator^(Int32)
  0, // Value.operator+(String)
  0, // Value.operator*(String)
  0, // Value.remove(Value)
  0, // Value.remove(String)
  0, // Value.remove((Function(Value)->Logical))
  0, // Value.remove_at(Int32)
  0, // Value.remove_first()
  0, // Value.remove_last()
  0, // Value.reserve(Int32)
  0, // Value.rest(Value)
  0, // Value.save(File,Logical,Logical)
  0, // Value.set(Int32,Int32)
  0, // Value.set(Int32,Int64)
  0, // Value.set(Int32,Logical)
  0, // Value.set(Int32,Real64)
  0, // Value.set(Int32,Object)
  0, // Value.set(Int32,String)
  0, // Value.set(Int32,Value)
  0, // Value.set(String,Int32)
  0, // Value.set(String,Int64)
  0, // Value.set(String,Logical)
  0, // Value.set(String,Real64)
  0, // Value.set(String,Object)
  0, // Value.set(String,String)
  0, // Value.set(Value,Value)
  (void*) (ROGUEM6) RogueValue__set__String_Value,
  0, // Value.sort((Function(Value,Value)->Logical))
  0, // Value.sorted((Function(Value,Value)->Logical))
  0, // Value.to_Byte()
  0, // Value.to_Character()
  (void*) (ROGUEM7) RogueLogicalValue__to_Int64,
  (void*) (ROGUEM2) RogueValue__to_Int32,
  (void*) (ROGUEM5) RogueLogicalValue__to_Logical,
  0, // Value.to_Real64()
  0, // Value.to_Real32()
  0, // Value.to_Object()
  0, // Value.to_VarArgs()
  0, // Value.to_json(Int32)
  0, // Value.to_json(Logical,Logical)
  0, // Value.to_json(StringBuilder,Logical,Logical)
  (void*) (ROGUEM8) RogueLogicalValue__to_json__StringBuilder_Int32,
  (void*) (ROGUEM0) RogueValueList__init_object, // ValueList
  (void*) (ROGUEM0) RogueValueList__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Value.operator==(Object)
  0, // Value.to_value_package()
  (void*) (ROGUEM0) RogueValueList__to_String,
  0, // Value.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueValueList__type_name,
  0, // Value.add(Int32)
  0, // Value.add(Int64)
  0, // Value.add(Logical)
  0, // Value.add(Real64)
  0, // Value.add(Object)
  0, // Value.add(String)
  (void*) (ROGUEM4) RogueValueList__add__Value,
  0, // ValueList.add(Value[])
  0, // Value.add_all(Value)
  (void*) (ROGUEM3) RogueValue__at__Int32,
  0, // ValueList.clear()
  0, // ValueList.cloned()
  0, // ValueList.apply((Function(Value)->Value))
  0, // ValueList.contains(String)
  0, // ValueList.contains(Value)
  0, // Value.contains((Function(Value)->Logical))
  (void*) (ROGUEM2) RogueValueList__count,
  0, // ValueList.count((Function(Value)->Logical))
  0, // Value.compressed()
  0, // ValueList.datatype()
  0, // Value.decompressed()
  0, // ValueList.decode_indexed(ValueIDLookupTable)
  0, // ValueList.encode_indexed(ValueIDTableBuilder)
  0, // Value.ensure_list(String)
  0, // Value.ensure_table(String)
  0, // ValueList.first()
  (void*) (ROGUEM4) RogueValueList__first___Function_Value_RETURNSLogical_,
  (void*) (ROGUEM3) RogueValueList__get__Int32,
  (void*) (ROGUEM4) RogueValue__get__String,
  0, // ValueList.get((Function(Value)->Logical))
  0, // Value.insert(Int32,Int32)
  0, // Value.insert(Int64,Int32)
  0, // Value.insert(Logical,Int32)
  0, // Value.insert(Real64,Int32)
  0, // Value.insert(Object,Int32)
  0, // Value.insert(String,Int32)
  0, // ValueList.insert(Value,Int32)
  0, // ValueList.insert(Value[],Int32)
  0, // Value.insert_all(Value,Int32)
  (void*) (ROGUEM5) RogueValueList__is_collection,
  0, // Value.is_complex()
  (void*) (ROGUEM5) RogueValueList__is_empty,
  0, // Value.is_false()
  0, // Value.is_int32()
  0, // Value.is_int64()
  (void*) (ROGUEM5) RogueValueList__is_list,
  (void*) (ROGUEM5) RogueValue__is_logical,
  0, // Value.is_null()
  (void*) (ROGUEM5) RogueValue__is_non_null,
  0, // Value.is_number()
  0, // Value.is_object()
  0, // Value.is_real64()
  0, // Value.is_string()
  0, // Value.is_table()
  0, // Value.is_undefined()
  0, // Value.keys()
  0, // ValueList.last()
  0, // ValueList.last((Function(Value)->Logical))
  0, // ValueList.locate(Value)
  0, // ValueList.locate((Function(Value)->Logical))
  0, // ValueList.locate_last(Value)
  0, // ValueList.locate_last((Function(Value)->Logical))
  0, // Value.operator==(Value)
  0, // Value.operator==(Byte)
  0, // Value.operator==(Character)
  0, // Value.operator==(Int32)
  0, // Value.operator==(Int64)
  0, // Value.operator==(Logical)
  0, // Value.operator==(Real64)
  0, // Value.operator==(Real32)
  0, // Value.operator==(String)
  0, // Value.operator<(Value)
  0, // Value.operator<(Byte)
  0, // Value.operator<(Character)
  0, // Value.operator<(Int32)
  0, // Value.operator<(Int64)
  0, // Value.operator<(Logical)
  0, // Value.operator<(Real64)
  0, // Value.operator<(Real32)
  0, // Value.operator<(Object)
  0, // Value.operator<(String)
  0, // Value.operator-()
  0, // Value.operator+(Value)
  0, // Value.operator-(Value)
  0, // Value.operator*(Value)
  0, // Value.operator/(Value)
  0, // Value.operator%(Value)
  0, // Value.operator^(Value)
  0, // Value.operator+(Real64)
  0, // Value.operator-(Real64)
  0, // Value.operator*(Real64)
  0, // Value.operator/(Real64)
  0, // Value.operator%(Real64)
  0, // Value.operator^(Real64)
  0, // Value.operator+(Int64)
  0, // Value.operator-(Int64)
  0, // Value.operator*(Int64)
  0, // Value.operator/(Int64)
  0, // Value.operator%(Int64)
  0, // Value.operator^(Int64)
  0, // Value.operator+(Int32)
  0, // Value.operator-(Int32)
  0, // Value.operator*(Int32)
  0, // Value.operator/(Int32)
  0, // Value.operator%(Int32)
  0, // Value.operator^(Int32)
  0, // Value.operator+(String)
  0, // Value.operator*(String)
  0, // ValueList.remove(Value)
  0, // Value.remove(String)
  0, // ValueList.remove((Function(Value)->Logical))
  0, // ValueList.remove_at(Int32)
  0, // ValueList.remove_first()
  0, // ValueList.remove_last()
  0, // ValueList.reserve(Int32)
  0, // Value.rest(Value)
  0, // Value.save(File,Logical,Logical)
  0, // Value.set(Int32,Int32)
  0, // Value.set(Int32,Int64)
  0, // Value.set(Int32,Logical)
  0, // Value.set(Int32,Real64)
  0, // Value.set(Int32,Object)
  0, // Value.set(Int32,String)
  0, // ValueList.set(Int32,Value)
  0, // Value.set(String,Int32)
  0, // Value.set(String,Int64)
  0, // Value.set(String,Logical)
  0, // Value.set(String,Real64)
  0, // Value.set(String,Object)
  0, // Value.set(String,String)
  0, // Value.set(Value,Value)
  (void*) (ROGUEM6) RogueValue__set__String_Value,
  0, // ValueList.sort((Function(Value,Value)->Logical))
  0, // ValueList.sorted((Function(Value,Value)->Logical))
  0, // Value.to_Byte()
  0, // Value.to_Character()
  (void*) (ROGUEM7) RogueValue__to_Int64,
  (void*) (ROGUEM2) RogueValue__to_Int32,
  (void*) (ROGUEM5) RogueValueList__to_Logical,
  0, // Value.to_Real64()
  0, // Value.to_Real32()
  0, // Value.to_Object()
  0, // ValueList.to_VarArgs()
  0, // Value.to_json(Int32)
  0, // Value.to_json(Logical,Logical)
  0, // Value.to_json(StringBuilder,Logical,Logical)
  (void*) (ROGUEM8) RogueValueList__to_json__StringBuilder_Int32,
  (void*) (ROGUEM0) RogueValue_List__init_object, // Value[]
  (void*) (ROGUEM0) RogueValue_List__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Value[].to_value_package()
  (void*) (ROGUEM0) RogueValue_List__to_String,
  0, // Value[].to_Value()
  0, // Object.type_info()
  0, // Value[].unpack(Value)
  (void*) (ROGUEM0) RogueValue_List__type_name,
  (void*) (ROGUEM1) RogueObject__init_object, // Array<<Value>>
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueArray_Value___type_name,
  (void*) (ROGUEM0) RogueJSON__init_object, // JSON
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueJSON__type_name,
  (void*) (ROGUEM0) RogueJSONParser__init_object, // JSONParser
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueJSONParser__type_name,
  (void*) (ROGUEM0) RogueScanner__init_object, // Scanner
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueScanner__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueScanner__type_name,
  0, // Reader<<Character>>.close() // Reader<<Character>>
  (void*) (ROGUEM0) RogueStringConsolidationTable__init_object, // StringConsolidationTable
  (void*) (ROGUEM0) RogueTable_String_String___init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Table<<String,String>>.to_value_package()
  (void*) (ROGUEM0) RogueTable_String_String___to_String,
  0, // Table<<String,String>>.to_Value()
  0, // Object.type_info()
  0, // Table<<String,String>>.unpack(Value)
  (void*) (ROGUEM0) RogueStringConsolidationTable__type_name,
  0, // Table<<String,String>>.init(Int32)
  0, // Table<<String,String>>.init(Table<<String,String>>)
  0, // Table<<String,String>>.add(Table<<String,String>>)
  0, // Table<<String,String>>.at(Int32)
  0, // Table<<String,String>>.clear()
  0, // Table<<String,String>>.cloned()
  0, // Table<<String,String>>.contains(String)
  0, // Table<<String,String>>.contains((Function(String)->Logical))
  0, // Table<<String,String>>.count((Function(Value)->Logical))
  0, // Table<<String,String>>.discard((Function(TableEntry<<String,String>>)->Logical))
  0, // Table<<String,String>>.entries()
  0, // Table<<String,String>>.entry_at(Int32)
  0, // Table<<String,String>>.is_empty()
  0, // Table<<String,String>>.find(String)
  0, // Table<<String,String>>.first()
  0, // Table<<String,String>>.first((Function(String)->Logical))
  (void*) (ROGUEM4) RogueStringConsolidationTable__get__String,
  (void*) (ROGUEM0) RogueFileWriter__init_object, // FileWriter
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueFileWriter__type_name,
  0, // Writer<<Byte>>.close() // Writer<<Byte>>
  (void*) (ROGUEM0) RogueConsole__init_object, // Console
  (void*) (ROGUEM0) RogueConsole__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueConsole__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueConsole__type_name,
  0, // BufferedPrintWriter<<output_buffer>>.close() // BufferedPrintWriter<<output_buffer>>
  (void*) (ROGUEM0) RogueConsoleErrorPrinter__init_object, // ConsoleErrorPrinter
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueConsoleErrorPrinter__type_name,
  (void*) (ROGUEM0) RogueConsoleEvent_List__init_object, // ConsoleEvent[]
  (void*) (ROGUEM0) RogueConsoleEvent_List__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // ConsoleEvent[].to_value_package()
  (void*) (ROGUEM0) RogueConsoleEvent_List__to_String,
  0, // ConsoleEvent[].to_Value()
  0, // Object.type_info()
  0, // ConsoleEvent[].unpack(Value)
  (void*) (ROGUEM0) RogueConsoleEvent_List__type_name,
  (void*) (ROGUEM1) RogueObject__init_object, // Array<<ConsoleEvent>>
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueArray_ConsoleEvent___type_name,
  (void*) (ROGUEM0) RogueFileReader__init_object, // FileReader
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueFileReader__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueFileReader__type_name,
  0, // Reader<<Byte>>.close() // Reader<<Byte>>
  (void*) (ROGUEM0) RogueLineReader__init_object, // LineReader
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueLineReader__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueLineReader__type_name,
  0, // LineReader.close()
  (void*) (ROGUEM5) RogueLineReader__has_another,
  0, // Reader<<String>>.close() // Reader<<String>>
  (void*) (ROGUEM0) RogueFunction_819__init_object, // Function_819
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueFunction_819__type_name,
  (void*) (ROGUEM1) RogueFunction_819__call,
  (void*) (ROGUEM0) RogueProcess__init_object, // Process
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueProcess__type_name,
  0, // Process.close()
  0, // Process.error_bytes()
  0, // Process.error_string()
  0, // Process.exit_code()
  0, // Process.is_finished()
  0, // Process.is_running()
  (void*) (ROGUEM0) RogueProcess__finish,
  0, // Process.launch(Logical,Logical,Logical)
  (void*) (ROGUEM1) RogueProcess__on_cleanup,
  0, // Process.output_bytes()
  0, // Process.output_string()
  0, // Process.run()
  (void*) (ROGUEM12) RogueProcess__update_io__Logical,
  (void*) (ROGUEM0) RogueProcessResult__init_object, // ProcessResult
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueProcessResult__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueProcessResult__type_name,
  (void*) (ROGUEM0) RogueWindowsProcess__init_object, // WindowsProcess
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueWindowsProcess__type_name,
  0, // Process.close()
  0, // Process.error_bytes()
  0, // Process.error_string()
  0, // Process.exit_code()
  (void*) (ROGUEM5) RogueWindowsProcess__is_finished,
  0, // Process.is_running()
  (void*) (ROGUEM0) RogueWindowsProcess__finish,
  (void*) (ROGUEM13) RogueWindowsProcess__launch__Logical_Logical_Logical,
  (void*) (ROGUEM1) RogueWindowsProcess__on_cleanup,
  0, // Process.output_bytes()
  0, // Process.output_string()
  0, // Process.run()
  (void*) (ROGUEM12) RogueProcess__update_io__Logical,
  (void*) (ROGUEM0) RogueWindowsProcessReader__init_object, // WindowsProcessReader
  (void*) (ROGUEM0) RogueWindowsProcessReader__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueWindowsProcessReader__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueWindowsProcessReader__type_name,
  (void*) (ROGUEM0) RogueWindowsProcessWriter__init_object, // WindowsProcessWriter
  (void*) (ROGUEM0) RogueWindowsProcessWriter__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueWindowsProcessWriter__type_name,
  (void*) (ROGUEM0) RoguePosixProcess__init_object, // PosixProcess
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RoguePosixProcess__type_name,
  0, // Process.close()
  0, // Process.error_bytes()
  0, // Process.error_string()
  0, // Process.exit_code()
  0, // PosixProcess.is_finished()
  0, // Process.is_running()
  (void*) (ROGUEM0) RoguePosixProcess__finish,
  (void*) (ROGUEM13) RoguePosixProcess__launch__Logical_Logical_Logical,
  (void*) (ROGUEM1) RogueProcess__on_cleanup,
  0, // Process.output_bytes()
  0, // Process.output_string()
  0, // Process.run()
  (void*) (ROGUEM12) RoguePosixProcess__update_io__Logical,
  (void*) (ROGUEM0) RoguePosixProcessReader__init_object, // PosixProcessReader
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RoguePosixProcessReader__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RoguePosixProcessReader__type_name,
  (void*) (ROGUEM0) RogueFDReader__init_object, // FDReader
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueFDReader__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueFDReader__type_name,
  (void*) (ROGUEM0) RogueFDWriter__init_object, // FDWriter
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueFDWriter__type_name,
  (void*) (ROGUEM0) RogueMorlock__init_object, // Morlock
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueMorlock__type_name,
  (void*) (ROGUEM0) RogueBootstrap__init_object, // Bootstrap
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueBootstrap__type_name,
  (void*) (ROGUEM0) RogueFunction_863__init_object, // Function_863
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueFunction_863__type_name,
  (void*) (ROGUEM9) RogueFunction_863__call__String,
  (void*) (ROGUEM0) RogueFunction_868__init_object, // Function_868
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueFunction_868__type_name,
  (void*) (ROGUEM9) RogueFunction_868__call__String,
  (void*) (ROGUEM0) RoguePackage__init_object, // Package
  (void*) (ROGUEM0) RoguePackage__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RoguePackage__type_name,
  (void*) (ROGUEM0) RoguePackageInfo__init_object, // PackageInfo
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RoguePackageInfo__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RoguePackageInfo__type_name,
  (void*) (ROGUEM0) RogueFileListing__init_object, // FileListing
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueFileListing__type_name,
  (void*) (ROGUEM0) Rogue_Function_String____init_object, // (Function(String))
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) Rogue_Function_String____type_name,
  (void*) (ROGUEM14) Rogue_Function_String____call__String,
  (void*) (ROGUEM0) RogueFileListing_collect_String___init_object, // FileListing.collect(String)
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueFileListing_collect_String___type_name,
  (void*) (ROGUEM14) RogueFileListing_collect_String___call__String,
  (void*) (ROGUEM0) RogueFunction_896__init_object, // Function_896
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueFunction_896__type_name,
  (void*) (ROGUEM9) RogueFunction_896__call__String,
  (void*) (ROGUEM0) RogueListRewriter_String___init_object, // ListRewriter<<String>>
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueListRewriter_String___type_name,
  (void*) (ROGUEM0) RogueGenericListRewriter__init_object, // GenericListRewriter
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueGenericListRewriter__type_name,
  0, // Writer<<String>>.close() // Writer<<String>>
  (void*) (ROGUEM0) RogueGenericListRewriter_List__init_object, // GenericListRewriter[]
  (void*) (ROGUEM0) RogueGenericListRewriter_List__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // GenericListRewriter[].to_value_package()
  (void*) (ROGUEM0) RogueGenericListRewriter_List__to_String,
  0, // GenericListRewriter[].to_Value()
  0, // Object.type_info()
  0, // GenericListRewriter[].unpack(Value)
  (void*) (ROGUEM0) RogueGenericListRewriter_List__type_name,
  (void*) (ROGUEM1) RogueObject__init_object, // Array<<GenericListRewriter>>
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueArray_GenericListRewriter___type_name,
  (void*) (ROGUEM0) Rogue_Function_String_String_RETURNSLogical___init_object, // (Function(String,String)->Logical)
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) Rogue_Function_String_String_RETURNSLogical___type_name,
  (void*) (ROGUEM11) Rogue_Function_String_String_RETURNSLogical___call__String_String,
  (void*) (ROGUEM0) RogueFunction_948__init_object, // Function_948
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueFunction_948__type_name,
  (void*) (ROGUEM11) RogueFunction_948__call__String_String,
  (void*) (ROGUEM0) RogueQuicksort_String___init_object, // Quicksort<<String>>
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueQuicksort_String___type_name,
  (void*) (ROGUEM0) RogueFunction_950__init_object, // Function_950
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueFunction_950__type_name,
  (void*) (ROGUEM11) RogueFunction_950__call__String_String,
  (void*) (ROGUEM0) RoguePlatforms__init_object, // Platforms
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RoguePlatforms__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RoguePlatforms__type_name,
  (void*) (ROGUEM0) RogueFunction_983__init_object, // Function_983
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueFunction_983__type_name,
  (void*) (ROGUEM11) RogueFunction_983__call__String_String,
  (void*) (ROGUEM0) RogueFunction_999__init_object, // Function_999
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueFunction_999__type_name,
  (void*) (ROGUEM11) RogueFunction_999__call__String_String,
  (void*) (ROGUEM0) RogueFunction_1003__init_object, // Function_1003
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueFunction_1003__type_name,
  (void*) (ROGUEM11) RogueFunction_1003__call__String_String,
  (void*) (ROGUEM0) RogueZip__init_object, // Zip
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueZip__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueZip__type_name,
  (void*) (ROGUEM0) RogueFunction_1033__init_object, // Function_1033
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueFunction_1033__type_name,
  (void*) (ROGUEM9) RogueFunction_1033__call__String,
  (void*) (ROGUEM0) RogueFunction_1039__init_object, // Function_1039
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueFunction_1039__type_name,
  (void*) (ROGUEM9) RogueFunction_1039__call__String,
  (void*) (ROGUEM0) RogueFunction_1040__init_object, // Function_1040
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueFunction_1040__type_name,
  (void*) (ROGUEM9) RogueFunction_1040__call__String,
  (void*) (ROGUEM0) RogueFunction_1041__init_object, // Function_1041
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueFunction_1041__type_name,
  (void*) (ROGUEM9) RogueFunction_1041__call__String,
  (void*) (ROGUEM0) RogueFunction_1043__init_object, // Function_1043
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueFunction_1043__type_name,
  (void*) (ROGUEM9) RogueFunction_1043__call__String,
  (void*) (ROGUEM0) RogueExtendedASCIIReader__init_object, // ExtendedASCIIReader
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueExtendedASCIIReader__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueExtendedASCIIReader__type_name,
  (void*) (ROGUEM0) RogueUTF8Reader__init_object, // UTF8Reader
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueUTF8Reader__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueUTF8Reader__type_name,
  (void*) (ROGUEM0) RogueFunction_1088__init_object, // Function_1088
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueFunction_1088__type_name,
  (void*) (ROGUEM9) RogueFunction_1088__call__Value,
  (void*) (ROGUEM0) RogueWeakReference__init_object, // WeakReference
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueWeakReference__type_name,
  (void*) (ROGUEM0) RogueFunction_2437__init_object, // Function_2437
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueFunction_2437__type_name,
  (void*) (ROGUEM1) RogueFunction_2437__call,
  (void*) (ROGUEM1) RogueObject__init_object, // Object
  (void*) (ROGUEM0) RogueObject__init,
  (void*) (ROGUEM0) RogueObject__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueObject__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueObject__type_name,
  (void*) (ROGUEM0) RogueException__init_object, // Exception
  (void*) (ROGUEM0) RogueException__init,
  (void*) (ROGUEM0) RogueException__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueException__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueException__type_name,
  (void*) (ROGUEM4) RogueException__init__String,
  0, // Exception.display()
  0, // Exception.format()
  (void*) (ROGUEM0) RogueException___throw,
  (void*) (ROGUEM0) RogueError__init_object, // Error
  (void*) (ROGUEM0) RogueException__init,
  (void*) (ROGUEM0) RogueException__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueException__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueError__type_name,
  (void*) (ROGUEM4) RogueException__init__String,
  0, // Exception.display()
  0, // Exception.format()
  (void*) (ROGUEM0) RogueError___throw,
  (void*) (ROGUEM0) RogueOutOfBoundsError__init_object, // OutOfBoundsError
  (void*) (ROGUEM0) RogueException__init,
  (void*) (ROGUEM0) RogueException__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueException__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueOutOfBoundsError__type_name,
  (void*) (ROGUEM4) RogueOutOfBoundsError__init__String,
  0, // Exception.display()
  0, // Exception.format()
  (void*) (ROGUEM0) RogueOutOfBoundsError___throw,
  (void*) (ROGUEM0) RogueJSONParseError__init_object, // JSONParseError
  (void*) (ROGUEM0) RogueException__init,
  (void*) (ROGUEM0) RogueException__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueException__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueJSONParseError__type_name,
  (void*) (ROGUEM4) RogueJSONParseError__init__String,
  0, // Exception.display()
  0, // Exception.format()
  (void*) (ROGUEM0) RogueJSONParseError___throw,
  (void*) (ROGUEM0) RogueIOError__init_object, // IOError
  (void*) (ROGUEM0) RogueException__init,
  (void*) (ROGUEM0) RogueException__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueException__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RogueIOError__type_name,
  (void*) (ROGUEM4) RogueException__init__String,
  0, // Exception.display()
  0, // Exception.format()
  (void*) (ROGUEM0) RogueIOError___throw,
  (void*) (ROGUEM0) RoguePackageError__init_object, // PackageError
  (void*) (ROGUEM0) RogueException__init,
  (void*) (ROGUEM0) RoguePackageError__description,
  0, // Object.hash_code()
  0, // Object.introspector()
  0, // Object.object_id()
  0, // Object.operator==(Object)
  0, // Object.to_value_package()
  (void*) (ROGUEM0) RogueException__to_String,
  0, // Object.to_Value()
  0, // Object.type_info()
  0, // Object.unpack(Value)
  (void*) (ROGUEM0) RoguePackageError__type_name,
  (void*) (ROGUEM4) RogueException__init__String,
  0, // Exception.display()
  0, // Exception.format()
  (void*) (ROGUEM0) RoguePackageError___throw,
  0, // StringEncoding.description() // StringEncoding
  0, // TableKeysIterator<<String,Value>>.read_another() // TableKeysIterator<<String,Value>>
  0, // ConsoleEventType.to_Int32() // ConsoleEventType
  0, // ConsoleEvent.is_character() // ConsoleEvent
  0, // TableKeysIterator<<String,String>>.read_another() // TableKeysIterator<<String,String>>
  0, // FilePattern.to_String() // FilePattern
  0, // FileOptions.keeping_files() // FileOptions
  0, // VersionNumber.count() // VersionNumber
  0, // Best<<String>>.consider(String) // Best<<String>>
  0, // ZipEntry.extract(Byte[]) // ZipEntry

};

const int Rogue_type_info_table[] ={
  // <FOR EACH TYPE>
  // info_count (total int count after info_count)
  // allocator_index
  // dynamic_method_table_index
  // base_type_count
  // base_type_index[ base_type_count ]
  // global_property_count
  // global_property_name_indices[ global_property_count ]
  // global_property_type_indices[ global_property_count ]
  // property_count
  // property_name_indices[ property_count ]
  // property_type_indices[ property_count ]
  // dynamic_method_count
  // global_method_count
  16,0,0,3,114,1,2,0,3,477,478,479,3,2,14,13,0,// Global
  10,0,13,1,2,0,1,477,3,1,0,// BufferedPrintWriter<<global_output_buffer>>
  7,0,14,0,0,0,1,0,// PrintWriter
  25,0,15,2,114,2,2,480,481,4,10,6,482,483,484,485,486,487,4,9,9,9,9,13,13,0,// StringBuilder
  13,0,28,2,5,114,0,2,488,483,7,9,13,0,// Byte[]
  8,0,41,1,114,0,0,13,0,// GenericList
  7,0,54,0,0,0,0,0,// Byte
  9,0,54,2,8,114,0,0,13,0,// Array<<Byte>>
  8,0,67,1,114,0,0,13,0,// Array
  7,0,80,0,0,0,1,0,// Int32
  10,0,81,1,114,0,1,489,11,13,0,// StringBuilderPool
  13,0,94,2,5,114,0,2,488,483,12,9,13,0,// StringBuilder[]
  9,0,107,2,8,114,0,0,13,0,// Array<<StringBuilder>>
  7,0,120,0,0,0,1,0,// Logical
  13,0,121,2,5,114,0,2,488,483,16,9,13,0,// (Function())[]
  8,0,134,1,114,0,0,14,0,// (Function())
  9,0,148,2,8,114,0,0,13,0,// Array<<(Function())>>
  8,0,161,1,114,0,0,13,0,// String
  14,0,174,1,114,0,3,490,483,491,19,9,13,13,0,// StackTrace
  13,0,187,2,5,114,0,2,488,483,20,9,13,0,// String[]
  9,0,200,2,8,114,0,0,13,0,// Array<<String>>
  7,0,213,0,0,0,1,0,// Real64
  7,0,214,0,0,0,1,0,// Int64
  7,0,215,0,0,0,1,0,// Character
  8,0,216,1,114,0,0,160,0,// Value
  13,0,376,2,5,114,0,2,488,483,26,9,13,0,// Character[]
  9,0,389,2,8,114,0,0,13,0,// Array<<Character>>
  8,0,402,1,114,0,0,14,0,// (Function(Value)->Logical)
  10,0,416,1,114,0,1,492,17,13,0,// File
  13,0,429,2,5,114,0,2,488,483,30,9,13,0,// Int32[]
  9,0,442,2,8,114,0,0,13,0,// Array<<Int32>>
  8,0,455,1,114,0,0,13,0,// Runtime
  10,0,468,3,33,24,114,0,0,160,0,// UndefinedValue
  9,0,628,2,24,114,0,0,160,0,// NullValue
  11,0,788,2,24,114,0,1,488,35,160,0,// ValueTable
  24,0,948,1,114,0,8,483,493,494,495,496,497,498,499,9,9,9,36,37,37,37,38,13,0,// Table<<String,Value>>
  9,0,961,2,8,114,0,0,13,0,// Array<<TableEntry<<String,Value>>>>
  20,0,974,1,114,0,6,500,501,502,503,504,505,17,24,37,37,37,9,13,0,// TableEntry<<String,Value>>
  8,0,987,1,114,0,0,14,0,// (Function(TableEntry<<String,Value>>,TableEntry<<String,Value>>)->Logical)
  12,0,1001,1,114,2,506,507,19,17,0,13,0,// System
  12,0,1014,1,114,0,2,508,509,41,19,13,0,// SystemEnvironment
  25,0,1027,2,42,114,0,8,483,493,494,495,496,497,498,499,9,9,9,43,44,44,44,45,30,0,// StringTable<<String>>
  24,0,1057,1,114,0,8,483,493,494,495,496,497,498,499,9,9,9,43,44,44,44,45,30,0,// Table<<String,String>>
  9,0,1087,2,8,114,0,0,13,0,// Array<<TableEntry<<String,String>>>>
  20,0,1100,1,114,0,6,500,501,502,503,504,505,17,17,44,44,44,9,13,0,// TableEntry<<String,String>>
  8,0,1113,1,114,0,0,13,0,// (Function(TableEntry<<String,String>>,TableEntry<<String,String>>)->Logical)
  8,0,1126,1,114,0,0,14,0,// (Function(String)->Logical)
  8,0,1140,1,114,0,0,13,0,// Math
  13,0,1153,2,24,114,1,510,48,1,501,17,160,0,// StringValue
  11,0,1313,2,24,114,0,1,501,21,160,0,// Real64Value
  15,0,1473,2,24,114,2,511,512,50,50,1,501,13,160,0,// LogicalValue
  11,0,1633,2,24,114,0,1,488,52,160,0,// ValueList
  13,0,1793,2,5,114,0,2,488,483,53,9,13,0,// Value[]
  9,0,1806,2,8,114,0,0,13,0,// Array<<Value>>
  8,0,1819,1,114,0,0,13,0,// JSON
  10,0,1832,1,114,0,1,513,56,13,0,// JSONParser
  23,0,1845,2,114,57,0,7,514,488,515,483,516,517,518,9,25,17,9,9,9,9,13,0,// Scanner
  9,0,1858,0,0,1,514,9,1,0,// Reader<<Character>>
  26,0,1859,3,41,42,114,0,8,483,493,494,495,496,497,498,499,9,9,9,43,44,44,44,45,30,0,// StringConsolidationTable
  19,0,1889,2,114,60,0,5,514,492,519,520,521,9,17,13,4,22,13,0,// FileWriter
  9,0,1902,0,0,1,514,9,1,0,// Writer<<Byte>>
  39,0,1903,4,114,57,62,2,0,14,514,522,519,523,524,525,526,527,528,529,530,531,532,533,9,3,63,13,13,64,13,9,9,13,3,124,4,22,13,0,// Console
  10,0,1916,1,2,0,1,522,3,1,0,// BufferedPrintWriter<<output_buffer>>
  12,0,1917,3,114,62,2,0,1,522,3,13,0,// ConsoleErrorPrinter
  13,0,1930,2,5,114,0,2,488,483,65,9,13,0,// ConsoleEvent[]
  9,0,1943,2,8,114,0,0,13,0,// Array<<ConsoleEvent>>
  21,0,1956,2,114,67,0,6,514,492,483,534,520,521,9,17,9,9,4,22,13,0,// FileReader
  9,0,1969,0,0,1,514,9,1,0,// Reader<<Byte>>
  19,0,1970,2,114,69,0,5,514,515,535,520,536,9,57,17,3,23,15,0,// LineReader
  9,0,1985,0,0,1,514,9,1,0,// Reader<<String>>
  9,0,1986,2,15,114,0,0,14,0,// Function_819
  26,0,2000,1,114,0,9,537,538,539,540,519,541,542,543,544,19,67,67,60,13,13,9,13,72,26,0,// Process
  18,0,2026,1,114,0,5,542,545,546,547,548,9,4,4,17,17,13,0,// ProcessResult
  33,0,2039,2,71,114,0,12,537,538,539,540,519,541,542,543,544,549,550,551,19,67,67,60,13,13,9,13,72,13,17,22,26,0,// WindowsProcess
  15,0,2065,2,114,67,0,3,514,520,552,9,4,22,13,0,// WindowsProcessReader
  15,0,2078,2,114,60,0,3,514,520,553,9,4,22,13,0,// WindowsProcessWriter
  29,0,2091,2,71,114,0,10,537,538,539,540,519,541,542,543,544,554,19,67,67,60,13,13,9,13,72,22,26,0,// PosixProcess
  15,0,2117,2,114,67,0,3,514,555,556,9,71,78,13,0,// PosixProcessReader
  19,0,2130,2,114,67,0,5,514,557,534,558,520,9,9,9,13,4,13,0,// FDReader
  19,0,2143,2,114,60,0,5,514,557,558,519,520,9,9,13,13,4,13,0,// FDWriter
  10,0,2156,1,114,0,1,37,17,13,0,// Morlock
  12,0,2169,1,114,0,2,559,560,13,13,13,0,// Bootstrap
  9,0,2182,2,46,114,0,0,14,0,// Function_863
  11,0,2196,2,46,114,0,1,561,17,14,0,// Function_868
  44,0,2210,1,114,0,18,118,224,120,562,121,563,564,122,139,119,565,566,567,568,569,570,571,572,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,51,24,13,0,// Package
  32,0,2223,1,114,0,12,224,562,563,121,122,120,573,492,139,574,575,119,17,17,17,17,17,17,17,17,17,19,13,17,13,0,// PackageInfo
  26,0,2236,1,114,0,9,573,576,577,578,579,580,581,582,583,17,17,133,19,19,19,19,19,87,13,0,// FileListing
  8,0,2249,1,114,0,0,14,0,// (Function(String))
  11,0,2263,2,87,114,0,1,584,86,14,0,// FileListing.collect(String)
  9,0,2277,2,46,114,0,0,14,0,// Function_896
  20,0,2291,3,91,114,92,1,481,93,4,514,585,586,587,9,19,9,9,13,0,// ListRewriter<<String>>
  8,0,2304,1,114,0,0,13,0,// GenericListRewriter
  9,0,2317,0,0,1,514,9,1,0,// Writer<<String>>
  13,0,2318,2,5,114,0,2,488,483,94,9,13,0,// GenericListRewriter[]
  9,0,2331,2,8,114,0,0,13,0,// Array<<GenericListRewriter>>
  8,0,2344,1,114,0,0,14,0,// (Function(String,String)->Logical)
  9,0,2358,2,95,114,0,0,14,0,// Function_948
  8,0,2372,1,114,0,0,13,0,// Quicksort<<String>>
  9,0,2385,2,95,114,0,0,14,0,// Function_950
  10,0,2399,1,114,0,1,588,17,13,0,// Platforms
  9,0,2412,2,95,114,0,0,14,0,// Function_983
  9,0,2426,2,95,114,0,0,14,0,// Function_999
  9,0,2440,2,95,114,0,0,14,0,// Function_1003
  16,0,2454,1,114,0,4,492,589,590,591,17,9,9,22,13,0,// Zip
  9,0,2467,2,46,114,0,0,14,0,// Function_1033
  9,0,2481,2,46,114,0,0,14,0,// Function_1039
  9,0,2495,2,46,114,0,0,14,0,// Function_1040
  11,0,2509,2,46,114,0,1,563,17,14,0,// Function_1041
  11,0,2523,2,46,114,0,1,563,17,14,0,// Function_1043
  15,0,2537,2,114,57,0,3,514,592,535,9,67,137,13,0,// ExtendedASCIIReader
  15,0,2550,2,114,57,0,3,514,592,535,9,67,137,13,0,// UTF8Reader
  9,0,2563,2,27,114,0,0,14,0,// Function_1088
  12,0,2577,1,114,0,2,593,594,112,22,13,0,// WeakReference
  11,0,2590,2,15,114,0,1,478,61,14,0,// Function_2437
  7,0,2604,0,0,0,13,0,// Object
  12,0,2617,1,114,0,2,595,596,17,18,17,0,// Exception
  13,0,2634,2,115,114,0,2,595,596,17,18,17,0,// Error
  14,0,2651,3,116,115,114,0,2,595,596,17,18,17,0,// OutOfBoundsError
  14,0,2668,3,116,115,114,0,2,595,596,17,18,17,0,// JSONParseError
  14,0,2685,3,116,115,114,0,2,595,596,17,18,17,0,// IOError
  16,0,2702,3,116,115,114,0,3,595,596,597,17,18,17,17,0,// PackageError
  13,0,2719,0,2,509,598,19,29,1,501,9,1,0,// StringEncoding
  9,0,2720,0,0,1,599,37,1,0,// TableKeysIterator<<String,Value>>
  11,0,2721,0,0,2,501,600,17,13,0,0,// String?
  11,0,2721,0,0,2,501,600,9,13,0,0,// Int32?
  11,0,2721,0,0,2,501,600,6,13,0,0,// Byte?
  13,0,2721,0,2,509,598,19,29,1,501,9,1,0,// ConsoleEventType
  13,0,2722,0,0,3,601,602,603,126,9,9,1,0,// ConsoleEvent
  11,0,2723,0,0,2,604,483,9,9,0,0,// Span
  11,0,2723,0,0,2,501,600,128,13,0,0,// Span?
  9,0,2723,0,0,1,599,44,1,0,// TableKeysIterator<<String,String>>
  9,0,2724,0,0,1,576,17,1,0,// FilePattern
  11,0,2725,0,0,2,501,600,131,13,0,0,// FilePattern?
  9,0,2725,0,0,1,605,9,1,0,// FileOptions
  11,0,2726,0,0,2,122,606,17,13,1,0,// VersionNumber
  13,0,2727,0,0,3,607,501,600,95,17,13,1,0,// Best<<String>>
  19,0,2728,0,1,520,4,5,142,224,608,609,610,103,17,13,22,9,1,0,// ZipEntry
  11,0,2729,0,0,2,501,600,23,13,0,0,// Character?
  13,0,2729,0,2,509,598,19,29,1,501,9,0,0,// UnixConsoleMouseEventType
};

const int Rogue_object_size_table[139] =
{
  (int) sizeof(RogueClassGlobal),
  (int) sizeof(RogueClassBufferedPrintWriter_global_output_buffer_),
  (int) sizeof(RogueClassPrintWriter),
  (int) sizeof(RogueStringBuilder),
  (int) sizeof(RogueByte_List),
  (int) sizeof(RogueClassGenericList),
  (int) sizeof(RogueByte),
  (int) sizeof(RogueArray),
  (int) sizeof(RogueArray),
  (int) sizeof(RogueInt32),
  (int) sizeof(RogueClassStringBuilderPool),
  (int) sizeof(RogueStringBuilder_List),
  (int) sizeof(RogueArray),
  (int) sizeof(RogueLogical),
  (int) sizeof(Rogue_Function____List),
  (int) sizeof(RogueClass_Function___),
  (int) sizeof(RogueArray),
  (int) sizeof(RogueString),
  (int) sizeof(RogueClassStackTrace),
  (int) sizeof(RogueString_List),
  (int) sizeof(RogueArray),
  (int) sizeof(RogueReal64),
  (int) sizeof(RogueInt64),
  (int) sizeof(RogueCharacter),
  (int) sizeof(RogueClassValue),
  (int) sizeof(RogueCharacter_List),
  (int) sizeof(RogueArray),
  (int) sizeof(RogueClass_Function_Value_RETURNSLogical_),
  (int) sizeof(RogueClassFile),
  (int) sizeof(RogueInt32_List),
  (int) sizeof(RogueArray),
  (int) sizeof(RogueClassRuntime),
  (int) sizeof(RogueClassUndefinedValue),
  (int) sizeof(RogueClassNullValue),
  (int) sizeof(RogueClassValueTable),
  (int) sizeof(RogueClassTable_String_Value_),
  (int) sizeof(RogueArray),
  (int) sizeof(RogueClassTableEntry_String_Value_),
  (int) sizeof(RogueClass_Function_TableEntry_String_Value__TableEntry_String_Value__RETURNSLogical_),
  (int) sizeof(RogueClassSystem),
  (int) sizeof(RogueClassSystemEnvironment),
  (int) sizeof(RogueClassStringTable_String_),
  (int) sizeof(RogueClassTable_String_String_),
  (int) sizeof(RogueArray),
  (int) sizeof(RogueClassTableEntry_String_String_),
  (int) sizeof(RogueClass_Function_TableEntry_String_String__TableEntry_String_String__RETURNSLogical_),
  (int) sizeof(RogueClass_Function_String_RETURNSLogical_),
  (int) sizeof(RogueClassMath),
  (int) sizeof(RogueClassStringValue),
  (int) sizeof(RogueClassReal64Value),
  (int) sizeof(RogueClassLogicalValue),
  (int) sizeof(RogueClassValueList),
  (int) sizeof(RogueValue_List),
  (int) sizeof(RogueArray),
  (int) sizeof(RogueClassJSON),
  (int) sizeof(RogueClassJSONParser),
  (int) sizeof(RogueClassScanner),
  (int) sizeof(RogueClassReader_Character_),
  (int) sizeof(RogueClassStringConsolidationTable),
  (int) sizeof(RogueClassFileWriter),
  (int) sizeof(RogueClassWriter_Byte_),
  (int) sizeof(RogueClassConsole),
  (int) sizeof(RogueClassBufferedPrintWriter_output_buffer_),
  (int) sizeof(RogueClassConsoleErrorPrinter),
  (int) sizeof(RogueConsoleEvent_List),
  (int) sizeof(RogueArray),
  (int) sizeof(RogueClassFileReader),
  (int) sizeof(RogueClassReader_Byte_),
  (int) sizeof(RogueClassLineReader),
  (int) sizeof(RogueClassReader_String_),
  (int) sizeof(RogueClassFunction_819),
  (int) sizeof(RogueClassProcess),
  (int) sizeof(RogueClassProcessResult),
  (int) sizeof(RogueClassWindowsProcess),
  (int) sizeof(RogueClassWindowsProcessReader),
  (int) sizeof(RogueClassWindowsProcessWriter),
  (int) sizeof(RogueClassPosixProcess),
  (int) sizeof(RogueClassPosixProcessReader),
  (int) sizeof(RogueClassFDReader),
  (int) sizeof(RogueClassFDWriter),
  (int) sizeof(RogueClassMorlock),
  (int) sizeof(RogueClassBootstrap),
  (int) sizeof(RogueClassFunction_863),
  (int) sizeof(RogueClassFunction_868),
  (int) sizeof(RogueClassPackage),
  (int) sizeof(RogueClassPackageInfo),
  (int) sizeof(RogueClassFileListing),
  (int) sizeof(RogueClass_Function_String__),
  (int) sizeof(RogueClassFileListing_collect_String_),
  (int) sizeof(RogueClassFunction_896),
  (int) sizeof(RogueClassListRewriter_String_),
  (int) sizeof(RogueClassGenericListRewriter),
  (int) sizeof(RogueClassWriter_String_),
  (int) sizeof(RogueGenericListRewriter_List),
  (int) sizeof(RogueArray),
  (int) sizeof(RogueClass_Function_String_String_RETURNSLogical_),
  (int) sizeof(RogueClassFunction_948),
  (int) sizeof(RogueClassQuicksort_String_),
  (int) sizeof(RogueClassFunction_950),
  (int) sizeof(RogueClassPlatforms),
  (int) sizeof(RogueClassFunction_983),
  (int) sizeof(RogueClassFunction_999),
  (int) sizeof(RogueClassFunction_1003),
  (int) sizeof(RogueClassZip),
  (int) sizeof(RogueClassFunction_1033),
  (int) sizeof(RogueClassFunction_1039),
  (int) sizeof(RogueClassFunction_1040),
  (int) sizeof(RogueClassFunction_1041),
  (int) sizeof(RogueClassFunction_1043),
  (int) sizeof(RogueClassExtendedASCIIReader),
  (int) sizeof(RogueClassUTF8Reader),
  (int) sizeof(RogueClassFunction_1088),
  (int) sizeof(RogueWeakReference),
  (int) sizeof(RogueClassFunction_2437),
  (int) sizeof(RogueObject),
  (int) sizeof(RogueException),
  (int) sizeof(RogueClassError),
  (int) sizeof(RogueClassOutOfBoundsError),
  (int) sizeof(RogueClassJSONParseError),
  (int) sizeof(RogueClassIOError),
  (int) sizeof(RogueClassPackageError),
  (int) sizeof(RogueClassStringEncoding),
  (int) sizeof(RogueClassTableKeysIterator_String_Value_),
  (int) sizeof(RogueOptionalString),
  (int) sizeof(RogueOptionalInt32),
  (int) sizeof(RogueOptionalByte),
  (int) sizeof(RogueClassConsoleEventType),
  (int) sizeof(RogueClassConsoleEvent),
  (int) sizeof(RogueClassSpan),
  (int) sizeof(RogueOptionalSpan),
  (int) sizeof(RogueClassTableKeysIterator_String_String_),
  (int) sizeof(RogueClassFilePattern),
  (int) sizeof(RogueOptionalFilePattern),
  (int) sizeof(RogueClassFileOptions),
  (int) sizeof(RogueClassVersionNumber),
  (int) sizeof(RogueClassBest_String_),
  (int) sizeof(RogueClassZipEntry),
  (int) sizeof(RogueOptionalCharacter),
  (int) sizeof(RogueClassUnixConsoleMouseEventType)
};

int Rogue_allocator_count = 1;
RogueAllocator Rogue_allocators[1];

int Rogue_type_count = 139;
RogueType Rogue_types[139];

RogueType* RogueTypeGlobal;
RogueType* RogueTypeBufferedPrintWriter_global_output_buffer_;
RogueType* RogueTypePrintWriter;
RogueType* RogueTypeStringBuilder;
RogueType* RogueTypeByte_List;
RogueType* RogueTypeGenericList;
RogueType* RogueTypeByte;
RogueType* RogueTypeArray;
RogueType* RogueTypeInt32;
RogueType* RogueTypeStringBuilderPool;
RogueType* RogueTypeStringBuilder_List;
RogueType* RogueTypeLogical;
RogueType* RogueType_Function____List;
RogueType* RogueType_Function___;
RogueType* RogueTypeString;
RogueType* RogueTypeStackTrace;
RogueType* RogueTypeString_List;
RogueType* RogueTypeReal64;
RogueType* RogueTypeInt64;
RogueType* RogueTypeCharacter;
RogueType* RogueTypeValue;
RogueType* RogueTypeCharacter_List;
RogueType* RogueType_Function_Value_RETURNSLogical_;
RogueType* RogueTypeFile;
RogueType* RogueTypeInt32_List;
RogueType* RogueTypeRuntime;
RogueType* RogueTypeUndefinedValue;
RogueType* RogueTypeNullValue;
RogueType* RogueTypeValueTable;
RogueType* RogueTypeTable_String_Value_;
RogueType* RogueTypeTableEntry_String_Value_;
RogueType* RogueType_Function_TableEntry_String_Value__TableEntry_String_Value__RETURNSLogical_;
RogueType* RogueTypeSystem;
RogueType* RogueTypeSystemEnvironment;
RogueType* RogueTypeStringTable_String_;
RogueType* RogueTypeTable_String_String_;
RogueType* RogueTypeTableEntry_String_String_;
RogueType* RogueType_Function_TableEntry_String_String__TableEntry_String_String__RETURNSLogical_;
RogueType* RogueType_Function_String_RETURNSLogical_;
RogueType* RogueTypeMath;
RogueType* RogueTypeStringValue;
RogueType* RogueTypeReal64Value;
RogueType* RogueTypeLogicalValue;
RogueType* RogueTypeValueList;
RogueType* RogueTypeValue_List;
RogueType* RogueTypeJSON;
RogueType* RogueTypeJSONParser;
RogueType* RogueTypeScanner;
RogueType* RogueTypeReader_Character_;
RogueType* RogueTypeStringConsolidationTable;
RogueType* RogueTypeFileWriter;
RogueType* RogueTypeWriter_Byte_;
RogueType* RogueTypeConsole;
RogueType* RogueTypeBufferedPrintWriter_output_buffer_;
RogueType* RogueTypeConsoleErrorPrinter;
RogueType* RogueTypeConsoleEvent_List;
RogueType* RogueTypeFileReader;
RogueType* RogueTypeReader_Byte_;
RogueType* RogueTypeLineReader;
RogueType* RogueTypeReader_String_;
RogueType* RogueTypeFunction_819;
RogueType* RogueTypeProcess;
RogueType* RogueTypeProcessResult;
RogueType* RogueTypeWindowsProcess;
RogueType* RogueTypeWindowsProcessReader;
RogueType* RogueTypeWindowsProcessWriter;
RogueType* RogueTypePosixProcess;
RogueType* RogueTypePosixProcessReader;
RogueType* RogueTypeFDReader;
RogueType* RogueTypeFDWriter;
RogueType* RogueTypeMorlock;
RogueType* RogueTypeBootstrap;
RogueType* RogueTypeFunction_863;
RogueType* RogueTypeFunction_868;
RogueType* RogueTypePackage;
RogueType* RogueTypePackageInfo;
RogueType* RogueTypeFileListing;
RogueType* RogueType_Function_String__;
RogueType* RogueTypeFileListing_collect_String_;
RogueType* RogueTypeFunction_896;
RogueType* RogueTypeListRewriter_String_;
RogueType* RogueTypeGenericListRewriter;
RogueType* RogueTypeWriter_String_;
RogueType* RogueTypeGenericListRewriter_List;
RogueType* RogueType_Function_String_String_RETURNSLogical_;
RogueType* RogueTypeFunction_948;
RogueType* RogueTypeQuicksort_String_;
RogueType* RogueTypeFunction_950;
RogueType* RogueTypePlatforms;
RogueType* RogueTypeFunction_983;
RogueType* RogueTypeFunction_999;
RogueType* RogueTypeFunction_1003;
RogueType* RogueTypeZip;
RogueType* RogueTypeFunction_1033;
RogueType* RogueTypeFunction_1039;
RogueType* RogueTypeFunction_1040;
RogueType* RogueTypeFunction_1041;
RogueType* RogueTypeFunction_1043;
RogueType* RogueTypeExtendedASCIIReader;
RogueType* RogueTypeUTF8Reader;
RogueType* RogueTypeFunction_1088;
RogueType* RogueTypeWeakReference;
RogueType* RogueTypeFunction_2437;
RogueType* RogueTypeObject;
RogueType* RogueTypeException;
RogueType* RogueTypeError;
RogueType* RogueTypeOutOfBoundsError;
RogueType* RogueTypeJSONParseError;
RogueType* RogueTypeIOError;
RogueType* RogueTypePackageError;
RogueType* RogueTypeStringEncoding;
RogueType* RogueTypeTableKeysIterator_String_Value_;
RogueType* RogueTypeOptionalString;
RogueType* RogueTypeOptionalInt32;
RogueType* RogueTypeOptionalByte;
RogueType* RogueTypeConsoleEventType;
RogueType* RogueTypeConsoleEvent;
RogueType* RogueTypeSpan;
RogueType* RogueTypeOptionalSpan;
RogueType* RogueTypeTableKeysIterator_String_String_;
RogueType* RogueTypeFilePattern;
RogueType* RogueTypeOptionalFilePattern;
RogueType* RogueTypeFileOptions;
RogueType* RogueTypeVersionNumber;
RogueType* RogueTypeBest_String_;
RogueType* RogueTypeZipEntry;
RogueType* RogueTypeOptionalCharacter;
RogueType* RogueTypeUnixConsoleMouseEventType;

int Rogue_literal_string_count = 611;
RogueString* Rogue_literal_strings[611];

const char* Rogue_literal_c_strings[611] =
{
  "",
  "%HOMEDRIVE%%HOMEPATH%/AppData/Local/Morlock",
  "/opt/morlock",
  "bootstrap",
  "install",
  "reinstall",
  "null",
  "Unsupported string encoding: ",
  "UTF8",
  "ASCII",
  "ASCII_256",
  "AUTODETECT",
  "(undefined)",
  "Exception",
  "=",
  ",",
  "\n",
  "No stack trace",
  "Unknown",
  "'morlock ",
  "' requires a single package name as argument.",
  "Index ",
  "-9223372036854775808",
  " is out of bounds (data has zero elements).",
  " is out of bounds (data has one element at index 0).",
  " is out of bounds (data has ",
  "# element",
  "#",
  "es",
  "s",
  " indices ",
  "..",
  ").",
  "~",
  "~/",
  "HOMEDRIVE",
  "HOMEPATH",
  "HOME",
  "\"",
  "cmd.exe",
  "/c",
  "Process reader could not be initialized.",
  " ",
  "sh",
  "-c",
  "cl",
  "This command must be run from a Visual Studio Developer Command Prompt with a command line C++ compiler.",
  "-",
  "Installing the Morlock Package Management System",
  "Creating home folder...",
  "//",
  "/",
  ".",
  "Could not get absolute path",
  "Unable to create folder: ",
  "sudo mkdir -p ",
  "\"*:<>?/\\|#$.-@_",
  "/#%+-._~",
  "> ",
  "Error executing '",
  "'; retrying with 'sudo'.",
  "sudo ",
  "Error executing:\n",
  "chown ",
  "USER",
  ":admin ",
  "Unable to chown Morlock home folder: ",
  "chmod 755 ",
  "bin",
  "build",
  "packages",
  "Add the following folder to your system Path:",
  "1. Start > Search for \"env\" > run \"Edit the system environment variables\".",
  "2. Click \"Environment Variables...\" near the bottom.",
  "3. Add or edit \"Path\" in the top \"User variables\" section.",
  "4. Add the Morlock \"bin\" folder to the path:",
  "  ",
  "5. Open a new Developer Command Prompt and re-run 'install.bat'.",
  "Add the following folder to your system PATH and re-run Morlock install command:",
  "SHELL",
  "~/.",
  "rc",
  "You can execute this command to make the change:",
  "echo export PATH=\"",
  "\":\\",
  "PATH >> ",
  " && source ",
  "Make the change by adding this line to your ~/.bashrc (etc.) and reopening your terminal:",
  "export PATH=\"",
  "\":$PATH",
  ".bat",
  "/bin/roguec",
  "://",
  "$://$/$",
  "https://github.com/account/repo or https://raw.githubusercontent.com/account/repo/main/morlock/account/app_name.rogue",
  "Invalid URL \"",
  "\" - expected e.g. \"",
  "\".",
  ".rogue",
  "github",
  "https://",
  "/packages/",
  "url.txt",
  "Unable to open ",
  " for reading.",
  "github.com",
  "**",
  "**/",
  "**/*",
  "*",
  "/*",
  "./",
  "script_filepath",
  "(",
  "Object",
  " 0x",
  ")",
  "Package name must be specified as a property. For example:\n\n  PROPERTIES\n    name = \"provider/",
  "action",
  "morlock_home",
  "host",
  "repo",
  "version",
  "https://api.github.com/repos/",
  "/releases",
  "curl -fsSL -H \"Accept: application/vnd.github.v3+json\" ",
  "Download failed: ",
  "Identifier expected.",
  "'",
  "' expected.",
  "true",
  "false",
  "tag_name",
  "v",
  "v$(I)",
  "$(I).(I)",
  "$(I)?(I)",
  "$(I)",
  "Cannot determine version number from release URL \"",
  "url",
  "platforms",
  "filename",
  "zip",
  "tar",
  "gz",
  "tarball",
  ".tar.gz",
  "zipball",
  ".zip",
  "Unrecognized archive format: ",
  "tarball_url",
  "ml",
  "zipball_url",
  "w",
  "https://github.com/abepralle/rogue",
  "No releases are available.",
  "No release is compatible with requested version '",
  "'. Available versions:",
  " v",
  " is already installed.",
  "build/abepralle/rogue",
  "Downloading ",
  "curl -LfsS ",
  " -o ",
  "Error downloading ",
  "[INTERNAL] Must call download() before unpack().",
  "Error opening \"",
  "Zip.extract() folder does not exist: ",
  "Extracting ",
  " for writing.",
  "tar -C ",
  " -xvf ",
  "Cannot unpack() file type '.$'; write custom install() code to handle it.",
  "make.bat",
  "Makefile",
  "Failed to find extracted source folder in: ",
  "Compiling roguec - this may take a while...",
  "cd ",
  " && make build",
  "xcopy /I /S /Q /Y ",
  "Source/Libraries",
  "Libraries",
  "(cd ",
  " && make build LIBRARIES_FOLDER=",
  "roguec.exe",
  "roguec",
  "Build",
  "Build/*",
  "bin/*",
  "No filepath or pattern given for ",
  "Android",
  "Cygwin",
  "iOS",
  "macOS",
  "Web",
  "Windows",
  "Linux",
  "$(OS)",
  "Cannot locate executable build product.",
  ".exe",
  "Error creating folder \"",
  "Copying ",
  " -> ",
  "Source file for copy() does not exist: ",
  "First argument must be a file; cannot copy() from folder: ",
  "Cannot copy() to nonexistent folder: ",
  "chmod u+x ",
  "@",
  " %*",
  "ln -s ",
  "/bin/rogo",
  "https://github.com/abepralle/rogo",
  "build/abepralle/rogo",
  "Compiling rogo...",
  " && make build)",
  "/bin/morlock",
  "https://github.com/abepralle/morlock",
  "build/abepralle/morlock",
  "Build.rogue",
  "Compiling morlock...",
  " && rogo build)",
  "help",
  "USAGE\n  morlock <command>\n\nCOMMANDS\n  help\n    Show this help text.\n\n  install   <package>\n\n  uninstall <package>\n\nPACKAGE FORMAT\n  provider/repo/app-name\n  provider/repo\n  repo\n  https://github.com/provider/repo/morlock/app-name.rogue",
  "A local .rogue script can only be used with 'morlock install'.",
  "name",
  "name*=*\"*\"",
  "*name*=*\"$\"*",
  "Failed to parse package name out of script ('name = \"provider/app-name\"').",
  "packages/*/",
  "Ambiguous app name '",
  "' matches mulitple installed packages:",
  " version ",
  "Updating",
  "Fetching",
  ".rogue install script",
  "curl -H \"Accept: application/vnd.github.v3+json\" ",
  "/contents",
  "Unable to list default branch of 'github.com/",
  "'.",
  "Repo does not exist: github.com/",
  "No morlock/ install script folder exists on repo.",
  "ref=",
  "main",
  "https://raw.githubusercontent.com/",
  "Morlock does not know how to construct ",
  " URLs.",
  "Creating folder ",
  "Creating ",
  "\\\\",
  "\\0",
  "\\b",
  "\\e",
  "\\f",
  "\\n",
  "\\t",
  "\\v",
  "\\",
  "\\[",
  "\\x",
  "/opt/morlock/packages/morlock/Source/ScriptLauncher.rogue",
  "/opt/morlock/packages/morlock/Source/Package.rogue",
  "source_crc32.txt",
  "-1",
  "0",
  "1",
  "2",
  "3",
  "4",
  "5",
  "6",
  "7",
  "8",
  "9",
  "roguec ",
  " --essential --api --compile --output=",
  " && ",
  " \"",
  "uninstall",
  " is not installed.",
  "Unrecognized morlock command '",
  "ROGUE_GC_THRESHOLD",
  "MB",
  "KB",
  "name:",
  "host:",
  "provider:",
  "repo:",
  "app_name:",
  "version:",
  "url:",
  "folder:",
  "filepath:",
  "using_local_script:",
  "[",
  "]",
  ":",
  "0.0",
  "-infinity",
  "infinity",
  "NaN",
  "End of Input.",
  "[Console._fill_event_queue_unix_process_next() Console.rogue:316]   _input_bytes:",
  "[Console._fill_event_queue_unix_process_next() Console.rogue:320]",
  "[Console._fill_event_queue_unix_process_next() Console.rogue:322]",
  "[Console._fill_event_queue_unix_process_next() Console.rogue:326]",
  "[Console._fill_event_queue_unix_process_next() Console.rogue:328]",
  "[Console._fill_event_queue_unix_process_next() Console.rogue:333]",
  "[Console._fill_event_queue_unix_process_next() Console.rogue:335]",
  "BACKSPACE",
  "TAB",
  "NEWLINE",
  "ESCAPE",
  "UP",
  "DOWN",
  "RIGHT",
  "LEFT",
  "DELETE",
  "CHARACTER",
  "POINTER_PRESS_LEFT",
  "POINTER_PRESS_RIGHT",
  "POINTER_RELEASE",
  "POINTER_MOVE",
  "SCROLL_UP",
  "SCROLL_DOWN",
  "Global",
  "Value",
  "String",
  "StringBuilder",
  "GenericList",
  "Array",
  "(Function(Value)->Logical)",
  "File",
  "Runtime",
  "Table<<String,Value>>",
  "TableEntry<<String,Value>>",
  "System",
  "SystemEnvironment",
  "Table<<String,String>>",
  "TableEntry<<String,String>>",
  "(Function(String)->Logical)",
  "(Function(TableEntry<<String,String>>,TableEntry<<String,String>>)->Logical)",
  "StringBuilderPool",
  "Math",
  "JSON",
  "JSONParser",
  "Scanner",
  "(Function(TableEntry<<String,Value>>,TableEntry<<String,Value>>)->Logical)",
  "FileWriter",
  "Console",
  "FileReader",
  "LineReader",
  "StackTrace",
  "(Function())",
  "Process",
  "WindowsProcessReader",
  "WindowsProcessWriter",
  "PosixProcessReader",
  "FDReader",
  "FDWriter",
  "ProcessResult",
  "Morlock",
  "Bootstrap",
  "Package",
  "PackageInfo",
  "FileListing",
  "(Function(String))",
  "GenericListRewriter",
  "(Function(String,String)->Logical)",
  "Quicksort<<String>>",
  "Platforms",
  "Zip",
  "ExtendedASCIIReader",
  "UTF8Reader",
  "ConsoleErrorPrinter",
  "WeakReference",
  "Character[]",
  "Byte[]",
  "String[]",
  "Value[]",
  "StringBuilder[]",
  "(Function())[]",
  "GenericListRewriter[]",
  "Int32[]",
  "ConsoleEvent[]",
  "Array<<Byte>>",
  "Array<<TableEntry<<String,String>>>>",
  "Array<<String>>",
  "Array<<TableEntry<<String,Value>>>>",
  "Array<<Value>>",
  "Array<<Character>>",
  "Array<<StringBuilder>>",
  "Array<<(Function())>>",
  "Array<<GenericListRewriter>>",
  "Array<<Int32>>",
  "Array<<ConsoleEvent>>",
  "Function_819",
  "Function_2437",
  """\x1B""[?1003h",
  """\x1B""[?1003l",
  """\x1B""[?25",
  "ERROR [",
  "]\n",
  "Error",
  "NullValue",
  "ValueTable",
  "StringValue",
  "Real64Value",
  "LogicalValue",
  "ValueList",
  "f",
  "no",
  "n",
  "off",
  "disable",
  "disabled",
  "\\\"",
  "\\r",
  "\\u",
  "Function_1088",
  "morlock",
  "OutOfBoundsError",
  "JSONParseError",
  "IOError",
  "PackageError",
  "UndefinedValue",
  "StringTable<<String>>",
  "Function_863",
  "Function_868",
  "Function_896",
  "Function_1033",
  "Function_1039",
  "Function_1040",
  "Function_1041",
  "Function_1043",
  "path",
  "WindowsProcess",
  "PosixProcess",
  "FileListing.collect(String)",
  "ListRewriter<<String>>",
  "Function_948",
  "Function_950",
  "Function_983",
  "Function_999",
  "Function_1003",
  "StringConsolidationTable",
  "BufferedPrintWriter<<global_output_buffer>>",
  "PrintWriter",
  "Byte",
  "Int32",
  "Logical",
  "Real64",
  "Int64",
  "Character",
  "Reader<<Character>>",
  "Writer<<Byte>>",
  "BufferedPrintWriter<<output_buffer>>",
  "Reader<<Byte>>",
  "Reader<<String>>",
  "Writer<<String>>",
  "StringEncoding",
  "TableKeysIterator<<String,Value>>",
  "String?",
  "Int32?",
  "Byte?",
  "ConsoleEventType",
  "ConsoleEvent",
  "Span",
  "Span?",
  "TableKeysIterator<<String,String>>",
  "FilePattern",
  "FilePattern?",
  "FileOptions",
  "VersionNumber",
  "Best<<String>>",
  "ZipEntry",
  "Character?",
  "UnixConsoleMouseEventType",
  "global_output_buffer",
  "console",
  "exit_functions",
  "work_bytes",
  "pool",
  "utf8",
  "count",
  "indent",
  "cursor_offset",
  "cursor_index",
  "at_newline",
  "data",
  "available",
  "entries",
  "is_formatted",
  "filepath",
  "bin_mask",
  "cur_entry_index",
  "bins",
  "first_entry",
  "last_entry",
  "cur_entry",
  "sort_function",
  "key",
  "value",
  "adjacent_entry",
  "next_entry",
  "previous_entry",
  "hash",
  "command_line_arguments",
  "executable_filepath",
  "definitions",
  "names",
  "empty_string",
  "true_value",
  "false_value",
  "reader",
  "position",
  "source",
  "line",
  "column",
  "spaces_per_tab",
  "error",
  "buffer",
  "FILE* fp;",
  "output_buffer",
  "immediate_mode",
  "show_cursor",
  "events",
  "windows_in_quick_edit_mode",
  "windows_button_state",
  "windows_last_press_type",
  "decode_utf8",
  "input_buffer",
  "next_input_character",
  "_input_bytes",
  "#if !defined(ROGUE_PLATFORM_WINDOWS)\n  termios original_terminal_settings;\n  int     original_stdin_flags;\n#endif",
  "buffer_position",
  "next",
  "prev",
  "args",
  "output_reader",
  "error_reader",
  "input_writer",
  "is_finished",
  "exit_code",
  "is_blocking",
  "result",
  "output_bytes",
  "error_bytes",
  "output_string",
  "error_string",
  "process_created",
  "arg_string",
  "#ifdef ROGUE_PLATFORM_WINDOWS\nSTARTUPINFO         startup_info;\nPROCESS_INFORMATION process_info;\n#endif",
  "#ifdef ROGUE_PLATFORM_WINDOWS\nHANDLE output_writer;\nHANDLE output_reader;\n#endif",
  "#ifdef ROGUE_PLATFORM_WINDOWS\nHANDLE input_writer;\nHANDLE input_reader;\n#endif",
  "#ifndef ROGUE_PLATFORM_WINDOWS\npid_t pid;\nint cin_pipe[2];\nint cout_pipe[2];\nint cerr_pipe[2];\nposix_spawn_file_actions_t actions;\npollfd poll_list[2];\n#endif",
  "process",
  "fd_reader",
  "fd",
  "auto_close",
  "printed_installing_header",
  "bootstrapping",
  "binpath",
  "provider",
  "app_name",
  "specified_version",
  "launcher_folder",
  "install_folder",
  "bin_folder",
  "archive_filename",
  "archive_folder",
  "is_unpacked",
  "releases",
  "properties",
  "folder",
  "installed_versions",
  "using_local_script",
  "pattern",
  "options",
  "path_segments",
  "pattern_segments",
  "filepath_segments",
  "empty_segments",
  "results",
  "callback",
  "context",
  "list",
  "read_index",
  "write_index",
  "combined",
  "compression",
  "mode",
  "zip_t* zip;",
  "byte_reader",
  "next_weak_reference",
  "RogueObject* value;",
  "message",
  "stack_trace",
  "package_name",
  "values",
  "cur",
  "exists",
  "type",
  "x",
  "y",
  "index",
  "flags",
  "kludge",
  "better_fn",
  "is_folder",
  "size",
  "crc32"
};

const int Rogue_accessible_type_count = 128;

RogueType** Rogue_accessible_type_pointers[128] =
{
  &RogueTypeGlobal,
  &RogueTypeBufferedPrintWriter_global_output_buffer_,
  &RogueTypePrintWriter,
  &RogueTypeStringBuilder,
  &RogueTypeByte_List,
  &RogueTypeGenericList,
  &RogueTypeByte,
  &RogueTypeArray,
  &RogueTypeInt32,
  &RogueTypeStringBuilderPool,
  &RogueTypeStringBuilder_List,
  &RogueTypeLogical,
  &RogueType_Function____List,
  &RogueType_Function___,
  &RogueTypeString,
  &RogueTypeStackTrace,
  &RogueTypeString_List,
  &RogueTypeReal64,
  &RogueTypeInt64,
  &RogueTypeCharacter,
  &RogueTypeValue,
  &RogueTypeCharacter_List,
  &RogueType_Function_Value_RETURNSLogical_,
  &RogueTypeFile,
  &RogueTypeInt32_List,
  &RogueTypeRuntime,
  &RogueTypeUndefinedValue,
  &RogueTypeNullValue,
  &RogueTypeValueTable,
  &RogueTypeTable_String_Value_,
  &RogueTypeTableEntry_String_Value_,
  &RogueType_Function_TableEntry_String_Value__TableEntry_String_Value__RETURNSLogical_,
  &RogueTypeSystem,
  &RogueTypeSystemEnvironment,
  &RogueTypeStringTable_String_,
  &RogueTypeTable_String_String_,
  &RogueTypeTableEntry_String_String_,
  &RogueType_Function_TableEntry_String_String__TableEntry_String_String__RETURNSLogical_,
  &RogueType_Function_String_RETURNSLogical_,
  &RogueTypeMath,
  &RogueTypeStringValue,
  &RogueTypeReal64Value,
  &RogueTypeLogicalValue,
  &RogueTypeValueList,
  &RogueTypeValue_List,
  &RogueTypeJSON,
  &RogueTypeJSONParser,
  &RogueTypeScanner,
  &RogueTypeReader_Character_,
  &RogueTypeStringConsolidationTable,
  &RogueTypeFileWriter,
  &RogueTypeWriter_Byte_,
  &RogueTypeConsole,
  &RogueTypeBufferedPrintWriter_output_buffer_,
  &RogueTypeConsoleErrorPrinter,
  &RogueTypeConsoleEvent_List,
  &RogueTypeFileReader,
  &RogueTypeReader_Byte_,
  &RogueTypeLineReader,
  &RogueTypeReader_String_,
  &RogueTypeFunction_819,
  &RogueTypeProcess,
  &RogueTypeProcessResult,
  &RogueTypeWindowsProcess,
  &RogueTypeWindowsProcessReader,
  &RogueTypeWindowsProcessWriter,
  &RogueTypePosixProcess,
  &RogueTypePosixProcessReader,
  &RogueTypeFDReader,
  &RogueTypeFDWriter,
  &RogueTypeMorlock,
  &RogueTypeBootstrap,
  &RogueTypeFunction_863,
  &RogueTypeFunction_868,
  &RogueTypePackage,
  &RogueTypePackageInfo,
  &RogueTypeFileListing,
  &RogueType_Function_String__,
  &RogueTypeFileListing_collect_String_,
  &RogueTypeFunction_896,
  &RogueTypeListRewriter_String_,
  &RogueTypeGenericListRewriter,
  &RogueTypeWriter_String_,
  &RogueTypeGenericListRewriter_List,
  &RogueType_Function_String_String_RETURNSLogical_,
  &RogueTypeFunction_948,
  &RogueTypeQuicksort_String_,
  &RogueTypeFunction_950,
  &RogueTypePlatforms,
  &RogueTypeFunction_983,
  &RogueTypeFunction_999,
  &RogueTypeFunction_1003,
  &RogueTypeZip,
  &RogueTypeFunction_1033,
  &RogueTypeFunction_1039,
  &RogueTypeFunction_1040,
  &RogueTypeFunction_1041,
  &RogueTypeFunction_1043,
  &RogueTypeExtendedASCIIReader,
  &RogueTypeUTF8Reader,
  &RogueTypeFunction_1088,
  &RogueTypeWeakReference,
  &RogueTypeFunction_2437,
  &RogueTypeObject,
  &RogueTypeException,
  &RogueTypeError,
  &RogueTypeOutOfBoundsError,
  &RogueTypeJSONParseError,
  &RogueTypeIOError,
  &RogueTypePackageError,
  &RogueTypeStringEncoding,
  &RogueTypeTableKeysIterator_String_Value_,
  &RogueTypeOptionalString,
  &RogueTypeOptionalInt32,
  &RogueTypeOptionalByte,
  &RogueTypeConsoleEventType,
  &RogueTypeConsoleEvent,
  &RogueTypeSpan,
  &RogueTypeOptionalSpan,
  &RogueTypeTableKeysIterator_String_String_,
  &RogueTypeFilePattern,
  &RogueTypeOptionalFilePattern,
  &RogueTypeFileOptions,
  &RogueTypeVersionNumber,
  &RogueTypeBest_String_,
  &RogueTypeZipEntry,
  &RogueTypeOptionalCharacter,
  &RogueTypeUnixConsoleMouseEventType
};

const int Rogue_accessible_type_indices[128] =
{
  0,
  1,
  2,
  3,
  4,
  5,
  6,
  8,
  9,
  10,
  11,
  13,
  14,
  15,
  17,
  18,
  19,
  21,
  22,
  23,
  24,
  25,
  27,
  28,
  29,
  31,
  32,
  33,
  34,
  35,
  37,
  38,
  39,
  40,
  41,
  42,
  44,
  45,
  46,
  47,
  48,
  49,
  50,
  51,
  52,
  54,
  55,
  56,
  57,
  58,
  59,
  60,
  61,
  62,
  63,
  64,
  66,
  67,
  68,
  69,
  70,
  71,
  72,
  73,
  74,
  75,
  76,
  77,
  78,
  79,
  80,
  81,
  82,
  83,
  84,
  85,
  86,
  87,
  88,
  89,
  90,
  91,
  92,
  93,
  95,
  96,
  97,
  98,
  99,
  100,
  101,
  102,
  103,
  104,
  105,
  106,
  107,
  108,
  109,
  110,
  111,
  112,
  113,
  114,
  115,
  116,
  117,
  118,
  119,
  120,
  121,
  122,
  123,
  124,
  125,
  126,
  127,
  128,
  129,
  130,
  131,
  132,
  133,
  134,
  135,
  136,
  137,
  138
};

RogueLogical RogueGlobal__execute__String_Logical_Logical_Logical( RogueString* cmd_0, RogueLogical suppress_error_1, RogueLogical allow_sudo_2, RogueLogical bg_3 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(bg_3)))
  {
    RogueGlobal__println( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[58], cmd_0 ))) )))) );
  }
  if (ROGUE_COND(((0) == ((RogueSystem__run__String( cmd_0 ))))))
  {
    return (RogueLogical)(true);
  }
  if (ROGUE_COND(allow_sudo_2))
  {
    RogueGlobal__println( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[59] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], cmd_0 ))) )))), Rogue_literal_strings[60] )))) )))) )))) );
    return (RogueLogical)((RogueGlobal__execute__String_Logical_Logical_Logical( ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[61], cmd_0 ))), suppress_error_1, false, false )));
  }
  if (ROGUE_COND(suppress_error_1))
  {
    return (RogueLogical)(false);
  }
  throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(((RogueException*)Rogue_call_ROGUEM4( 13, ROGUE_ARG(((RogueException*)ROGUE_CREATE_REF(RogueClassError*,ROGUE_CREATE_OBJECT(Error)))), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[62] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], cmd_0 ))) )))) )))) ))))) ));
}

void RogueGlobal__on_control_c__Int32( RogueInt32 signum_0 )
{
  ROGUE_GC_CHECK;
  RogueGlobal__call_exit_functions( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)) );
  RogueSystem__exit__Int32( 1 );
}

void RogueStringBuilder__init_class_thread_local()
{
  ROGUE_GC_CHECK;
  RogueStringBuilder_work_bytes = ((RogueByte_List*)(RogueByte_List__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueByte_List*,ROGUE_CREATE_OBJECT(Byte_List))) )));
  RogueStringBuilder_pool = ((RogueClassStringBuilderPool*)(((RogueObject*)Rogue_call_ROGUEM0( 1, ROGUE_ARG(((RogueObject*)ROGUE_CREATE_REF(RogueClassStringBuilderPool*,ROGUE_CREATE_OBJECT(StringBuilderPool)))) ))));
}

RogueInt32 RogueInt32__create__Int32( RogueInt32 value_0 )
{
  ROGUE_GC_CHECK;
  return (RogueInt32)(value_0);
}

RogueLogical RogueLogical__create__Int32( RogueInt32 value_0 )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(((value_0) != (0)));
}

RogueString* RogueString__create__Byte_List_StringEncoding( RogueByte_List* bytes_0, RogueClassStringEncoding encoding_1 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((RogueStringEncoding__operatorEQUALSEQUALS__StringEncoding( encoding_1, RogueClassStringEncoding( 3 ) )))))
  {
    encoding_1 = ((RogueClassStringEncoding)(((((((RogueByte_List__is_valid_utf8( bytes_0 )))))) ? (RogueClassStringEncoding( 0 )) : RogueClassStringEncoding( 2 ))));
  }
  switch (encoding_1.value)
  {
    case 0:
    case 1:
    {
      return (RogueString*)(((RogueString*)(RogueString_create_from_utf8( (char*)(bytes_0->data->as_bytes), bytes_0->count ))));
    }
    case 2:
    {
      RogueInt32 len_2 = (0);
      unsigned char* src = bytes_0->data->as_bytes - 1;
      for (int i=bytes_0->count; --i>=0; )
      {
        if (*(++src) & 0x80) ++len_2;
      }

      RogueInt32 new_count_3 = (((bytes_0->count) + (len_2)));
      ROGUE_DEF_LOCAL_REF(RogueByte_List*,utf8_bytes_4,(((RogueByte_List*)(RogueByte_List__init__Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueByte_List*,ROGUE_CREATE_OBJECT(Byte_List))), ROGUE_ARG(((bytes_0->count) + (len_2))) )))));
      utf8_bytes_4->count = new_count_3;
      src = bytes_0->data->as_bytes + bytes_0->count;
      unsigned char* dest = utf8_bytes_4->data->as_bytes + new_count_3;
      for (int i=bytes_0->count; --i>=0; )
      {
        unsigned char ch = *(--src);
        if (ch & 0x80)
        {
          *(--dest) = 0x80 | (ch & 0x3F);
          *(--dest) = 0xC0 | ((ch>>6) & 0x03);
        }
        else
        {
          *(--dest) = ch;
        }
      }
      return RogueString_create_from_utf8( (char*)(utf8_bytes_4->data->as_bytes), utf8_bytes_4->count );

      break;
    }
    default:
    {
      throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(((RogueException*)Rogue_call_ROGUEM4( 13, ROGUE_ARG(((RogueException*)ROGUE_CREATE_REF(RogueClassError*,ROGUE_CREATE_OBJECT(Error)))), ROGUE_ARG(((RogueString*)(RogueString__operatorPLUS__String( ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], Rogue_literal_strings[7] ))), ROGUE_ARG(((RogueString*)(RogueStringEncoding__to_String( encoding_1 )))) )))) ))))) ));
    }
  }
}

RogueString* RogueString__create__Character_List( RogueCharacter_List* characters_0 )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(RogueString_create_from_characters( characters_0 ))));
}

RogueString* RogueString__create__File_StringEncoding( RogueClassFile* file_0, RogueClassStringEncoding encoding_1 )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(RogueFile__load_as_string__StringEncoding( file_0, encoding_1 ))));
}

RogueLogical RogueString__exists__String( RogueString* string_0 )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(((!!(string_0)) && (!!(((RogueString__count( string_0 )))))));
}

RogueLogical RogueString__operatorEQUALSEQUALS__String_String( RogueString* a_0, RogueString* b_1 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((void*)a_0) == ((void*)NULL)))
  {
    return (RogueLogical)(((void*)b_1) == ((void*)NULL));
  }
  else
  {
    return (RogueLogical)(((RogueString__operatorEQUALSEQUALS__String( a_0, b_1 ))));
  }
}

RogueInt32 RogueString__operatorLTGT__String_String( RogueString* a_0, RogueString* b_1 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((void*)a_0) == ((void*)NULL)))
  {
    if (ROGUE_COND(((void*)b_1) == ((void*)NULL)))
    {
      return (RogueInt32)(0);
    }
    else
    {
      return (RogueInt32)(-1);
    }
  }
  else
  {
    return (RogueInt32)(((RogueString__operatorLTGT__String( a_0, b_1 ))));
  }
}

RogueString* RogueString__operatorPLUS__String_String( RogueString* st_0, RogueString* value_1 )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueString*,_auto_118_2,(st_0));
  return (RogueString*)(((RogueString*)(RogueString__operatorPLUS__String( ROGUE_ARG(((((_auto_118_2))) ? (ROGUE_ARG((RogueString*)_auto_118_2)) : ROGUE_ARG(Rogue_literal_strings[6]))), value_1 ))));
}

RogueString* RogueString__operatorTIMES__String_Int32( RogueString* st_0, RogueInt32 value_1 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((void*)st_0) == ((void*)NULL)))
  {
    return (RogueString*)(((RogueString*)(NULL)));
  }
  return (RogueString*)(((RogueString*)(RogueString__operatorTIMES__Int32( st_0, value_1 ))));
}

RogueString* RogueString__operatorSLASH__String_String( RogueString* prefix_0, RogueString* suffix_1 )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(((RogueString*)(RogueString__without_trailing__Character( prefix_0, (RogueCharacter)'/' )))) ))) )))), Rogue_literal_strings[51] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], suffix_1 ))) )))) ))));
}

RogueCharacter RogueCharacter__create__Int32( RogueInt32 value_0 )
{
  ROGUE_GC_CHECK;
  return (RogueCharacter)(((RogueCharacter)(value_0)));
}

RogueCharacter RogueCharacter__create__Character( RogueCharacter value_0 )
{
  ROGUE_GC_CHECK;
  return (RogueCharacter)(value_0);
}

RogueCharacter RogueCharacter__create__Byte( RogueByte value_0 )
{
  ROGUE_GC_CHECK;
  return (RogueCharacter)(((RogueCharacter)(value_0)));
}

RogueClassValue* RogueValue__create__Logical( RogueLogical value_0 )
{
  ROGUE_GC_CHECK;
  return (RogueClassValue*)(((RogueClassValue*)(((((value_0))) ? (ROGUE_ARG((RogueClassLogicalValue*)RogueLogicalValue_true_value)) : ROGUE_ARG(RogueLogicalValue_false_value)))));
}

RogueClassValue* RogueValue__create__String( RogueString* value_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((void*)value_0) == ((void*)NULL)))
  {
    return (RogueClassValue*)(((RogueClassValue*)(((RogueClassNullValue*)ROGUE_SINGLETON(NullValue)))));
  }
  return (RogueClassValue*)(((RogueClassValue*)(((RogueClassStringValue*)(RogueStringValue__init__String( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassStringValue*,ROGUE_CREATE_OBJECT(StringValue))), value_0 ))))));
}

RogueLogical RogueOptionalValue__operator__Value( RogueClassValue* value_0 )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(((((((void*)value_0) != ((void*)NULL)) && ((Rogue_call_ROGUEM5( 61, value_0 ))))) && (((!((Rogue_call_ROGUEM5( 59, value_0 )))) || ((Rogue_call_ROGUEM5( 151, value_0 )))))));
}

RogueString* RogueFile__absolute_filepath__String( RogueString* _auto_3994 )
{
  ROGUE_DEF_LOCAL_REF(RogueString*,filepath_0,_auto_3994);
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(!!(filepath_0))))
  {
    return (RogueString*)(((RogueString*)(NULL)));
  }
  filepath_0 = ((RogueString*)((RogueFile__expand_path__String( filepath_0 ))));
  if (ROGUE_COND(!((RogueFile__exists__String( filepath_0 )))))
  {
    ROGUE_DEF_LOCAL_REF(RogueString*,parent_1,((RogueFile__folder__String( filepath_0 ))));
    if (ROGUE_COND(((((RogueString__count( parent_1 )))) == (0))))
    {
      parent_1 = ((RogueString*)(Rogue_literal_strings[52]));
    }
    return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG((RogueFile__absolute_filepath__String( parent_1 ))) ))) )))), Rogue_literal_strings[51] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG((RogueFile__filename__String( filepath_0 ))) ))) )))) ))));
  }
  ROGUE_DEF_LOCAL_REF(RogueString*,return_value_2,0);
#if defined(_WIN32)
  {
    char long_name[PATH_MAX+4];
    char full_name[PATH_MAX+4];

    strcpy_s( long_name, PATH_MAX+4, (char*)filepath_0->utf8 );

    if (GetFullPathName(long_name, PATH_MAX+4, full_name, 0) != 0)
    {
      return_value_2 = RogueString_create_from_utf8( full_name, -1 );
    }
  }
#else
  {
    int original_dir_fd;
    int new_dir_fd;
    char filename[PATH_MAX];
    char c_filepath[ PATH_MAX ];
    bool is_folder;

    is_folder = RogueFile__is_folder__String( filepath_0 );

    int len = filepath_0->byte_count;
    if (len >= PATH_MAX) len = PATH_MAX - 1;
    memcpy( c_filepath, (char*)filepath_0->utf8, len );
    c_filepath[len] = 0;

    // A way to get back to the starting folder when finished.
    original_dir_fd = open( ".", O_RDONLY );

    if (is_folder)
    {
      filename[0] = 0;
    }
    else
    {
      // fchdir only works with a path, not a path+filename (c_filepath).
      // Copy out the filename and null terminate the filepath to be just a path.
      int i = (int) strlen( c_filepath ) - 1;
      while (i >= 0 && c_filepath[i] != '/') --i;
      strcpy( filename, c_filepath+i+1 );
      if (i == -1) strcpy( c_filepath, "." );
      else         c_filepath[i] = 0;
    }
    new_dir_fd = open( c_filepath, O_RDONLY );

    do
    {
      if (original_dir_fd >= 0 && new_dir_fd >= 0)
      {
        int r = fchdir( new_dir_fd );
        if ( r != 0 ) break;
        char * r2 = getcwd( c_filepath, PATH_MAX );
        if ( r2 == 0 ) break;
        if ( !is_folder )
        {
          strcat( c_filepath, "/" );
          strcat( c_filepath, filename );
        }
        r = fchdir( original_dir_fd );
        if ( r != 0 ) break;
      }

      return_value_2 = RogueString_create_from_utf8( c_filepath, -1 );
    } while (false);

    if (original_dir_fd >= 0) close( original_dir_fd );
    if (new_dir_fd >= 0) close( new_dir_fd );
  }
#endif

  if (ROGUE_COND(((void*)return_value_2) == ((void*)NULL)))
  {
    throw ((RogueException*)(RogueIOError___throw( ROGUE_ARG(((RogueClassIOError*)(((RogueException*)Rogue_call_ROGUEM4( 13, ROGUE_ARG(((RogueException*)ROGUE_CREATE_REF(RogueClassIOError*,ROGUE_CREATE_OBJECT(IOError)))), Rogue_literal_strings[53] ))))) )));
  }
  return (RogueString*)((RogueFile__fix_slashes__String( return_value_2 )));
}

RogueLogical RogueFile__copy__String_String_Logical_Logical_Logical_Logical( RogueString* _auto_3995, RogueString* _auto_3996, RogueLogical if_newer_2, RogueLogical if_different_3, RogueLogical dry_run_4, RogueLogical verbose_5 )
{
  ROGUE_DEF_LOCAL_REF(RogueString*,from_filepath_0,_auto_3995);
  ROGUE_DEF_LOCAL_REF(RogueString*,to_filepath_1,_auto_3996);
  ROGUE_GC_CHECK;
  from_filepath_0 = ((RogueString*)((RogueFile__expand_path__String( from_filepath_0 ))));
  to_filepath_1 = ((RogueString*)((RogueFile__expand_path__String( to_filepath_1 ))));
  if (ROGUE_COND(!((RogueFile__exists__String( from_filepath_0 )))))
  {
    throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(((RogueException*)Rogue_call_ROGUEM4( 13, ROGUE_ARG(((RogueException*)ROGUE_CREATE_REF(RogueClassError*,ROGUE_CREATE_OBJECT(Error)))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[203], from_filepath_0 ))) ))))) ));
  }
  if (ROGUE_COND((RogueFile__is_folder__String( from_filepath_0 ))))
  {
    throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(((RogueException*)Rogue_call_ROGUEM4( 13, ROGUE_ARG(((RogueException*)ROGUE_CREATE_REF(RogueClassError*,ROGUE_CREATE_OBJECT(Error)))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[204], from_filepath_0 ))) ))))) ));
  }
  if (ROGUE_COND((RogueFile__is_folder__String( to_filepath_1 ))))
  {
    to_filepath_1 = ((RogueString*)((RogueString__operatorSLASH__String_String( to_filepath_1, ROGUE_ARG((RogueFile__filename__String( from_filepath_0 ))) ))));
  }
  else
  {
    ROGUE_DEF_LOCAL_REF(RogueString*,containing_folder_6,((RogueFile__folder__String( to_filepath_1 ))));
    if (ROGUE_COND(!((RogueFile__is_folder__String( containing_folder_6 )))))
    {
      throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(((RogueException*)Rogue_call_ROGUEM4( 13, ROGUE_ARG(((RogueException*)ROGUE_CREATE_REF(RogueClassError*,ROGUE_CREATE_OBJECT(Error)))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[205], containing_folder_6 ))) ))))) ));
    }
  }
  {
    {
      {
        if (ROGUE_COND(((if_newer_2) && ((RogueFile__is_newer_than__String_String( from_filepath_0, to_filepath_1 )))))) goto _auto_1049;
        if (ROGUE_COND(((if_different_3) && ((RogueFile__is_different_than__String_String( from_filepath_0, to_filepath_1 )))))) goto _auto_1049;
        if ( !(((!(if_newer_2)) && (!(if_different_3)))) ) goto _auto_1050;
        }
      _auto_1049:;
      {
        if (ROGUE_COND(dry_run_4))
        {
          return (RogueLogical)(true);
        }
        if (ROGUE_COND(verbose_5))
        {
          RogueGlobal__println( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[201] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], from_filepath_0 ))) )))), Rogue_literal_strings[202] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], to_filepath_1 ))) )))) )))) )))) );
        }
        ROGUE_DEF_LOCAL_REF(RogueClassFileReader*,reader_7,((RogueFile__reader__String( from_filepath_0 ))));
        ROGUE_DEF_LOCAL_REF(RogueClassFileWriter*,writer_8,((RogueFile__writer__String( to_filepath_1 ))));
        ROGUE_DEF_LOCAL_REF(RogueByte_List*,file_buffer_9,(((RogueByte_List*)(RogueByte_List__init__Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueByte_List*,ROGUE_CREATE_OBJECT(Byte_List))), 1024 )))));
        while (ROGUE_COND(((RogueFileReader__has_another( reader_7 )))))
        {
          ROGUE_GC_CHECK;
          RogueFileReader__read__Byte_List_Int32( reader_7, ROGUE_ARG(([&]()->RogueByte_List*{
            ROGUE_DEF_LOCAL_REF(RogueByte_List*,_auto_310_0,(file_buffer_9));
            RogueByte_List__clear( _auto_310_0 );
             return _auto_310_0;
          }())
          ), 1024 );
          RogueFileWriter__write__Byte_List( writer_8, file_buffer_9 );
        }
        RogueFileWriter__close( writer_8 );
        return (RogueLogical)(true);
        }
    }
    _auto_1050:;
    {
      return (RogueLogical)(false);
      }
  }
}

RogueString* RogueFile__conventional_filepath__String( RogueString* filepath_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND((RogueSystem__is_windows())))
  {
    return (RogueString*)(((RogueString*)(RogueString__replacing__Character_Character_Logical( filepath_0, (RogueCharacter)'/', (RogueCharacter)'\\', false ))));
  }
  else
  {
    return (RogueString*)(filepath_0);
  }
}

RogueInt32 RogueFile__crc32__String( RogueString* _auto_3997 )
{
  ROGUE_DEF_LOCAL_REF(RogueString*,filepath_0,_auto_3997);
  ROGUE_GC_CHECK;
  filepath_0 = ((RogueString*)((RogueFile__expand_path__String( filepath_0 ))));
  if (ROGUE_COND(!((RogueFile__exists__String( filepath_0 )))))
  {
    return (RogueInt32)(0);
  }
  RogueUInt32 result = ~0U;

  ROGUE_DEF_LOCAL_REF(RogueClassFileReader*,reader_1,((RogueFile__reader__String( filepath_0 ))));
  ROGUE_DEF_LOCAL_REF(RogueByte_List*,file_buffer_2,(((RogueByte_List*)(RogueByte_List__init__Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueByte_List*,ROGUE_CREATE_OBJECT(Byte_List))), 1024 )))));
  while (ROGUE_COND(((RogueFileReader__has_another( reader_1 )))))
  {
    ROGUE_GC_CHECK;
    RogueFileReader__read__Byte_List_Int32( reader_1, ROGUE_ARG(([&]()->RogueByte_List*{
      ROGUE_DEF_LOCAL_REF(RogueByte_List*,_auto_311_0,(file_buffer_2));
      RogueByte_List__clear( _auto_311_0 );
       return _auto_311_0;
    }())
    ), 1024 );
    RogueByte* byte_ptr = file_buffer_2->data->as_bytes - 1;
    for (int i=file_buffer_2->count+1; --i; )
    {
      result = Rogue_crc32_table[ (result ^ *(++byte_ptr)) & 0xFF ] ^ (result >> 8);
    }

  }
  return (RogueInt32)(((RogueInt32)((RogueInt32)~result)));
}

RogueLogical RogueFile__create_folder__String( RogueString* _auto_3998 )
{
  ROGUE_DEF_LOCAL_REF(RogueString*,filepath_0,_auto_3998);
  ROGUE_GC_CHECK;
  filepath_0 = ((RogueString*)((RogueFile__expand_path__String( filepath_0 ))));
  while (ROGUE_COND(((((((RogueString__count( filepath_0 )))) > (1))) && (((RogueString__ends_with__Character_Logical( filepath_0, (RogueCharacter)'/', false )))))))
  {
    ROGUE_GC_CHECK;
    filepath_0 = ((RogueString*)(((RogueString*)(RogueString__leftmost__Int32( filepath_0, -1 )))));
  }
  filepath_0 = ((RogueString*)((RogueFile__absolute_filepath__String( filepath_0 ))));
  if (ROGUE_COND((RogueFile__exists__String( filepath_0 ))))
  {
    return (RogueLogical)((RogueFile__is_folder__String( filepath_0 )));
  }
  ROGUE_DEF_LOCAL_REF(RogueString*,parent_1,((RogueFile__folder__String( filepath_0 ))));
  if (ROGUE_COND(!((RogueFile__create_folder__String( parent_1 )))))
  {
    return (RogueLogical)(false);
  }
#if defined(ROGUE_PLATFORM_WINDOWS)
    return (0 == mkdir((char*)filepath_0->utf8));
#else
    return (0 == mkdir((char*)filepath_0->utf8, 0777));
#endif

}

RogueLogical RogueFile__delete__String( RogueString* _auto_3999 )
{
  ROGUE_DEF_LOCAL_REF(RogueString*,filepath_0,_auto_3999);
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(!!(filepath_0))))
  {
    return (RogueLogical)(false);
  }
  filepath_0 = ((RogueString*)((RogueFile__expand_path__String( filepath_0 ))));
  if (ROGUE_COND((RogueFile__is_folder__String( filepath_0 ))))
  {
    {
      ROGUE_DEF_LOCAL_REF(RogueString_List*,_auto_1011_0,((RogueFile__listing__String_OptionalFilePattern_Logical_Logical_Logical_Logical_Logical_Logical( filepath_0, RogueOptionalFilePattern( RogueClassFilePattern( Rogue_literal_strings[109] ), true ), false, false, false, false, false, false ))));
      RogueInt32 _auto_1012_0 = (0);
      RogueInt32 _auto_1013_0 = (((_auto_1011_0->count) - (1)));
      for (;ROGUE_COND(((_auto_1012_0) <= (_auto_1013_0)));++_auto_1012_0)
      {
        ROGUE_GC_CHECK;
        ROGUE_DEF_LOCAL_REF(RogueString*,_auto_312_0,(((RogueString*)(_auto_1011_0->data->as_objects[_auto_1012_0]))));
        RogueFile__delete__String( _auto_312_0 );
      }
    }
    return (RogueLogical)(((0) == (((RogueInt32)(rmdir( (const char*) filepath_0->utf8 ))))));
  }
  return (RogueLogical)(((0) == (((RogueInt32)(unlink( (const char*) filepath_0->utf8 ))))));
}

RogueLogical RogueFile__ends_with_separator__String( RogueString* filepath_0 )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(((((RogueString__ends_with__Character_Logical( filepath_0, (RogueCharacter)'/', false )))) || ((((RogueSystem__is_windows())) && (((RogueString__ends_with__Character_Logical( filepath_0, (RogueCharacter)'\\', false ))))))));
}

RogueString* RogueFile__ensure_ends_with_separator__String( RogueString* filepath_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND((RogueFile__ends_with_separator__String( filepath_0 ))))
  {
    return (RogueString*)(filepath_0);
  }
  return (RogueString*)(((RogueString*)(RogueString__operatorPLUS__Character( ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], filepath_0 ))), (RogueCharacter)'/' ))));
}

RogueString* RogueFile__esc__String( RogueString* filepath_0 )
{
  ROGUE_GC_CHECK;
  return (RogueString*)((RogueFile__shell_escaped__String( ROGUE_ARG((RogueFile__conventional_filepath__String( filepath_0 ))) )));
}

RogueLogical RogueFile__exists__String( RogueString* _auto_4000 )
{
  ROGUE_DEF_LOCAL_REF(RogueString*,filepath_0,_auto_4000);
  ROGUE_GC_CHECK;
  filepath_0 = ((RogueString*)((RogueFile__expand_path__String( filepath_0 ))));
  if ( !filepath_0 ) return false;

  struct stat s;
  return (stat((char*)filepath_0->utf8, &s) == 0);

}

RogueString* RogueFile__expand_path__String( RogueString* _auto_4001 )
{
  ROGUE_DEF_LOCAL_REF(RogueString*,filepath_0,_auto_4001);
  ROGUE_GC_CHECK;
  if (ROGUE_COND((RogueSystem__is_windows())))
  {
    RogueInt32 n_1 = (((RogueString__count__Character( filepath_0, (RogueCharacter)'%' ))));
    while (ROGUE_COND(((n_1) >= (2))))
    {
      ROGUE_GC_CHECK;
      RogueInt32 i1_2 = (((RogueString__locate__Character_OptionalInt32_Logical( filepath_0, (RogueCharacter)'%', (RogueOptionalInt32__create()), false ))).value);
      RogueInt32 i2_3 = (((RogueString__locate__Character_OptionalInt32_Logical( filepath_0, (RogueCharacter)'%', RogueOptionalInt32( ((i1_2) + (1)), true ), false ))).value);
      ROGUE_DEF_LOCAL_REF(RogueString*,name_4,(((RogueString*)(RogueString__from__Int32_Int32( filepath_0, ROGUE_ARG(((i1_2) + (1))), ROGUE_ARG(((i2_3) - (1))) )))));
      ROGUE_DEF_LOCAL_REF(RogueString*,_auto_313_5,(((RogueString*)(RogueSystemEnvironment__get__String( ROGUE_ARG((RogueSystem__env())), name_4 )))));
      ROGUE_DEF_LOCAL_REF(RogueString*,value_6,(((((_auto_313_5))) ? (ROGUE_ARG((RogueString*)_auto_313_5)) : ROGUE_ARG(Rogue_literal_strings[0]))));
      filepath_0 = ((RogueString*)(((RogueString*)(RogueString__replacing_at__Int32_Int32_String( filepath_0, i1_2, ROGUE_ARG(((((i2_3) - (i1_2))) + (1))), value_6 )))));
      n_1 -= 2;
    }
  }
  filepath_0 = ((RogueString*)((RogueFile__fix_slashes__String( filepath_0 ))));
  if (ROGUE_COND((((RogueString__operatorEQUALSEQUALS__String_String( filepath_0, Rogue_literal_strings[33] ))) || (((RogueString__begins_with__String_Logical( filepath_0, Rogue_literal_strings[34], false )))))))
  {
    ROGUE_DEF_LOCAL_REF(RogueString*,home_folder_7,0);
    if (ROGUE_COND((RogueSystem__is_windows())))
    {
      home_folder_7 = ((RogueString*)((RogueString__operatorPLUS__String_String( ROGUE_ARG(((RogueString*)(RogueSystemEnvironment__get__String( ROGUE_ARG((RogueSystem__environment())), Rogue_literal_strings[35] )))), ROGUE_ARG(((RogueString*)(RogueSystemEnvironment__get__String( ROGUE_ARG((RogueSystem__environment())), Rogue_literal_strings[36] )))) ))));
    }
    else
    {
      home_folder_7 = ((RogueString*)(((RogueString*)(RogueSystemEnvironment__get__String( ROGUE_ARG((RogueSystem__environment())), Rogue_literal_strings[37] )))));
    }
    if (ROGUE_COND(((!!(home_folder_7)) && (!!(((RogueString__count( home_folder_7 ))))))))
    {
      filepath_0 = ((RogueString*)((RogueString__operatorPLUS__String_String( home_folder_7, ROGUE_ARG(((RogueString*)(RogueString__rightmost__Int32( filepath_0, -1 )))) ))));
      return (RogueString*)(filepath_0);
    }
  }
  return (RogueString*)(filepath_0);
}

RogueString* RogueFile__extension__String( RogueString* filepath_0 )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(RogueString__after_last__Character_Logical( ROGUE_ARG((RogueFile__filename__String( filepath_0 ))), (RogueCharacter)'.', false ))));
}

RogueString* RogueFile__filename__String( RogueString* _auto_4002 )
{
  ROGUE_DEF_LOCAL_REF(RogueString*,filepath_0,_auto_4002);
  ROGUE_GC_CHECK;
  filepath_0 = ((RogueString*)((RogueFile__expand_path__String( filepath_0 ))));
  RogueOptionalInt32 i_1 = (((RogueString__locate_last__Character_OptionalInt32_Logical( filepath_0, (RogueCharacter)'/', (RogueOptionalInt32__create()), false ))));
  if (ROGUE_COND(!(i_1.exists)))
  {
    return (RogueString*)(filepath_0);
  }
  return (RogueString*)(((RogueString*)(RogueString__from__Int32( filepath_0, ROGUE_ARG(((i_1.value) + (1))) ))));
}

RogueString* RogueFile__fix_slashes__String( RogueString* filepath_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND((RogueSystem__is_windows())))
  {
    return (RogueString*)(((RogueString*)(RogueString__replacing__Character_Character_Logical( filepath_0, (RogueCharacter)'\\', (RogueCharacter)'/', false ))));
  }
  else
  {
    return (RogueString*)(filepath_0);
  }
}

RogueString* RogueFile__folder__String( RogueString* _auto_4003 )
{
  ROGUE_DEF_LOCAL_REF(RogueString*,filepath_0,_auto_4003);
  ROGUE_GC_CHECK;
  filepath_0 = ((RogueString*)((RogueFile__expand_path__String( filepath_0 ))));
  while (ROGUE_COND(((RogueString__contains__String_Logical( filepath_0, Rogue_literal_strings[50], false )))))
  {
    ROGUE_GC_CHECK;
    filepath_0 = ((RogueString*)(((RogueString*)(RogueString__replacing__String_String_Logical( filepath_0, Rogue_literal_strings[50], Rogue_literal_strings[51], false )))));
  }
  if (ROGUE_COND(((RogueString__ends_with__Character_Logical( filepath_0, (RogueCharacter)'/', false )))))
  {
    filepath_0 = ((RogueString*)(((RogueString*)(RogueString__leftmost__Int32( filepath_0, -1 )))));
  }
  if (ROGUE_COND(((((RogueString__count( filepath_0 )))) == (0))))
  {
    return (RogueString*)(Rogue_literal_strings[0]);
  }
  RogueOptionalInt32 i1_1 = (((RogueString__locate_last__Character_OptionalInt32_Logical( filepath_0, (RogueCharacter)'/', (RogueOptionalInt32__create()), false ))));
  if (ROGUE_COND(!(i1_1.exists)))
  {
    return (RogueString*)(Rogue_literal_strings[0]);
  }
  if (ROGUE_COND(((i1_1.value) == (0))))
  {
    return (RogueString*)(((RogueString*)(RogueString__leftmost__Int32( filepath_0, 1 ))));
  }
  return (RogueString*)(((RogueString*)(RogueString__from__Int32_Int32( filepath_0, 0, ROGUE_ARG(((i1_1.value) - (1))) ))));
}

RogueLogical RogueFile__is_different_than__String_String( RogueString* _auto_4004, RogueString* _auto_4005 )
{
  ROGUE_DEF_LOCAL_REF(RogueString*,filepath_0,_auto_4004);
  ROGUE_DEF_LOCAL_REF(RogueString*,other_filepath_1,_auto_4005);
  ROGUE_GC_CHECK;
  filepath_0 = ((RogueString*)((RogueFile__expand_path__String( filepath_0 ))));
  other_filepath_1 = ((RogueString*)((RogueFile__expand_path__String( other_filepath_1 ))));
  if (ROGUE_COND((((RogueFile__exists__String( filepath_0 ))) ^ ((RogueFile__exists__String( other_filepath_1 ))))))
  {
    return (RogueLogical)(true);
  }
  if (ROGUE_COND((((RogueFile__size__String( filepath_0 ))) != ((RogueFile__size__String( other_filepath_1 ))))))
  {
    return (RogueLogical)(true);
  }
  if (ROGUE_COND((((RogueFile__crc32__String( filepath_0 ))) != ((RogueFile__crc32__String( other_filepath_1 ))))))
  {
    return (RogueLogical)(true);
  }
  return (RogueLogical)(false);
}

RogueLogical RogueFile__is_folder__String( RogueString* _auto_4006 )
{
  ROGUE_DEF_LOCAL_REF(RogueString*,filepath_0,_auto_4006);
  ROGUE_GC_CHECK;
  filepath_0 = ((RogueString*)((RogueFile__expand_path__String( filepath_0 ))));
  if ( !filepath_0 ) return false;

#if defined(ROGUE_PLATFORM_WINDOWS)
    struct stat s;
    if (stat((char*)filepath_0->utf8, &s) != 0) return false;
    return (s.st_mode & S_IFMT) == S_IFDIR;
#else
    DIR* dir = opendir( (char*)filepath_0->utf8 );
    if ( !dir ) return 0;

    closedir( dir );
    return 1;
#endif

}

RogueLogical RogueFile__is_newer_than__String_String( RogueString* _auto_4007, RogueString* _auto_4008 )
{
  ROGUE_DEF_LOCAL_REF(RogueString*,filepath_0,_auto_4007);
  ROGUE_DEF_LOCAL_REF(RogueString*,other_filepath_1,_auto_4008);
  ROGUE_GC_CHECK;
  filepath_0 = ((RogueString*)((RogueFile__expand_path__String( filepath_0 ))));
  other_filepath_1 = ((RogueString*)((RogueFile__expand_path__String( other_filepath_1 ))));
  if (ROGUE_COND(!((RogueFile__exists__String( filepath_0 )))))
  {
    return (RogueLogical)(false);
  }
  if (ROGUE_COND(!((RogueFile__exists__String( other_filepath_1 )))))
  {
    return (RogueLogical)(true);
  }
  return (RogueLogical)(((floor((double)(RogueFile__timestamp__String( filepath_0 )))) > (floor((double)(RogueFile__timestamp__String( other_filepath_1 ))))));
}

RogueString_List* RogueFile__listing__String_OptionalFilePattern_Logical_Logical_Logical_Logical_Logical_Logical( RogueString* folder_0, RogueOptionalFilePattern filepattern_1, RogueLogical ignore_hidden_2, RogueLogical absolute_3, RogueLogical omit_path_4, RogueLogical files_5, RogueLogical folders_6, RogueLogical unsorted_7 )
{
  ROGUE_GC_CHECK;
  RogueClassFileOptions options_8 = (RogueClassFileOptions( 0 ));
  if (ROGUE_COND(ignore_hidden_2))
  {
    options_8 = ((RogueClassFileOptions)(RogueClassFileOptions( ((options_8.flags) | (32)) )));
  }
  if (ROGUE_COND(absolute_3))
  {
    options_8 = ((RogueClassFileOptions)(RogueClassFileOptions( ((options_8.flags) | (4)) )));
  }
  if (ROGUE_COND(omit_path_4))
  {
    options_8 = ((RogueClassFileOptions)(RogueClassFileOptions( ((options_8.flags) | (2)) )));
  }
  if (ROGUE_COND(files_5))
  {
    options_8 = ((RogueClassFileOptions)(RogueClassFileOptions( ((options_8.flags) | (8)) )));
  }
  if (ROGUE_COND(folders_6))
  {
    options_8 = ((RogueClassFileOptions)(RogueClassFileOptions( ((options_8.flags) | (16)) )));
  }
  if (ROGUE_COND(unsorted_7))
  {
    options_8 = ((RogueClassFileOptions)(RogueClassFileOptions( ((options_8.flags) | (64)) )));
  }
  return (RogueString_List*)((RogueFile__listing__String_OptionalFilePattern_FileOptions( folder_0, filepattern_1, options_8 )));
}

RogueString_List* RogueFile__listing__String_OptionalFilePattern_FileOptions( RogueString* folder_0, RogueOptionalFilePattern filepattern_1, RogueClassFileOptions options_2 )
{
  ROGUE_GC_CHECK;
  return (RogueString_List*)(((RogueClassFileListing*)(RogueFileListing__init__String_String_FileOptions( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassFileListing*,ROGUE_CREATE_OBJECT(FileListing))), folder_0, ROGUE_ARG(((((filepattern_1.exists))) ? (ROGUE_ARG((RogueString*)((RogueString*)(RogueFilePattern__to_String( filepattern_1.value ))))) : ROGUE_ARG(((RogueString*)(NULL))))), options_2 )))->results);
}

void RogueFile___listing__String__Function_String__( RogueString* folder_0, RogueClass_Function_String__* collector_1 )
{
  ROGUE_GC_CHECK;
#ifdef ROGUE_PLATFORM_WINDOWS

  ROGUE_DEF_LOCAL_REF(RogueString*,search_filepath_2,((RogueString__operatorPLUS__String_String( ROGUE_ARG((RogueFile__ensure_ends_with_separator__String( folder_0 ))), Rogue_literal_strings[109] ))));
  {
    WIN32_FIND_DATA entry;
    HANDLE dir;

    dir = FindFirstFile( search_filepath_2->utf8, &entry );

    if (dir != INVALID_HANDLE_VALUE)
    {
      do
      {
        int keep = 1;
        if (entry.cFileName[0] == '.')
        {
          switch (entry.cFileName[1])
          {
            case 0:   // '.' / this folder
              keep = 0;
              break;
            case '.':
              keep = entry.cFileName[2] != 0;  // ".." / Parent Folder
              break;
          }
        }
        if (keep)
        {

  {
    ROGUE_DEF_LOCAL_REF(RogueString*,entry_3,(((RogueString*)(RogueString_create_from_utf8(entry.cFileName,-1)))));
    Rogue_call_ROGUEM14( 13, collector_1, entry_3 );
  }
        }
      }
      while (FindNextFile(dir,&entry));

      FindClose( dir );
    }
  }
#else
  // Mac/Linux
  {
    DIR* dir;
    struct dirent* entry;

    dir = opendir( (const char*) folder_0->utf8 );
    if (dir)
    {
      entry = readdir( dir );
      while (entry)
      {
        int keep = 1;
        if (entry->d_name[0] == '.')
        {
          switch (entry->d_name[1])
          {
            case 0:   // '.' / this folder
              keep = 0;
              break;
            case '.':
              keep = entry->d_name[2] != 0;  // ".." / Parent Folder
              break;
          }
        }
        if (keep)
        {

  {
    ROGUE_DEF_LOCAL_REF(RogueString*,entry_4,(((RogueString*)(RogueString_create_from_utf8(entry->d_name,-1)))));
    Rogue_call_ROGUEM14( 13, collector_1, entry_4 );
  }

        }
        entry = readdir( dir );
      }
      closedir( dir );
    }
  }
#endif // Windows vs Mac/Linux

}

RogueByte_List* RogueFile__load_as_bytes__String( RogueString* _auto_4009 )
{
  ROGUE_DEF_LOCAL_REF(RogueString*,filepath_0,_auto_4009);
  ROGUE_GC_CHECK;
  filepath_0 = ((RogueString*)((RogueFile__expand_path__String( filepath_0 ))));
  RogueInt64 count_1 = ((RogueFile__size__String( filepath_0 )));
  ROGUE_DEF_LOCAL_REF(RogueByte_List*,bytes_2,(((RogueByte_List*)(RogueByte_List__init__Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueByte_List*,ROGUE_CREATE_OBJECT(Byte_List))), ROGUE_ARG(((RogueInt32)(count_1))) )))));
  ROGUE_DEF_LOCAL_REF(RogueClassFileReader*,infile_3,((RogueFile__reader__String( filepath_0 ))));
  {
    RogueInt32 _auto_318_4 = (1);
    RogueInt64 _auto_319_5 = (count_1);
    for (;ROGUE_COND(((((RogueInt64)(_auto_318_4))) <= (_auto_319_5)));++_auto_318_4)
    {
      ROGUE_GC_CHECK;
      RogueByte_List__add__Byte( bytes_2, ROGUE_ARG(((RogueFileReader__read( infile_3 )))) );
    }
  }
  RogueFileReader__close( infile_3 );
  return (RogueByte_List*)(bytes_2);
}

RogueString* RogueFile__load_as_string__String_StringEncoding( RogueString* filepath_0, RogueClassStringEncoding encoding_1 )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueByte_List*,bytes_2,((RogueFile__load_as_bytes__String( filepath_0 ))));
  if (ROGUE_COND(((((((((bytes_2->count) >= (3))) && (((((RogueInt32)(bytes_2->data->as_bytes[0]))) == (239))))) && (((((RogueInt32)(bytes_2->data->as_bytes[1]))) == (187))))) && (((((RogueInt32)(bytes_2->data->as_bytes[2]))) == (191))))))
  {
    RogueByte_List__discard__Int32_Int32( bytes_2, 0, 3 );
  }
  return (RogueString*)((RogueString__create__Byte_List_StringEncoding( bytes_2, encoding_1 )));
}

RogueLogical RogueFile__matches_wildcard_pattern__String_String_Logical( RogueString* _auto_4010, RogueString* pattern_1, RogueLogical ignore_case_2 )
{
  ROGUE_DEF_LOCAL_REF(RogueString*,filepath_0,_auto_4010);
  ROGUE_GC_CHECK;
  filepath_0 = ((RogueString*)((RogueFile__expand_path__String( filepath_0 ))));
  RogueInt32 last_wildcard_3 = (-1);
  {
    ROGUE_DEF_LOCAL_REF(RogueString*,_auto_942_0,(pattern_1));
    RogueInt32 i_0 = (((((RogueString__count( _auto_942_0 )))) - (1)));
    for (;ROGUE_COND(((i_0) >= (0)));--i_0)
    {
      ROGUE_GC_CHECK;
      RogueCharacter ch_0 = (((RogueString__get__Int32( _auto_942_0, i_0 ))));
      if (ROGUE_COND(((ch_0) == ((RogueCharacter)'*'))))
      {
        last_wildcard_3 = ((RogueInt32)(i_0));
        if (ROGUE_COND(((((((((i_0) > (0))) && (((((i_0) + (1))) < (((RogueString__count( pattern_1 )))))))) && (((((RogueString__get__Int32( pattern_1, ROGUE_ARG(((i_0) - (1))) )))) == ((RogueCharacter)'*'))))) && (((((RogueString__get__Int32( pattern_1, ROGUE_ARG(((i_0) + (1))) )))) == ((RogueCharacter)'/'))))))
        {
          ++last_wildcard_3;
        }
        goto _auto_943;
      }
      else if (ROGUE_COND(((ch_0) == ((RogueCharacter)'?'))))
      {
        last_wildcard_3 = ((RogueInt32)(i_0));
        goto _auto_943;
      }
    }
  }
  _auto_943:;
  if (ROGUE_COND(((last_wildcard_3) != (-1))))
  {
    RogueInt32 end_count_4 = (((((RogueString__count( pattern_1 )))) - (((last_wildcard_3) + (1)))));
    if (ROGUE_COND(((end_count_4) > (0))))
    {
      if (ROGUE_COND(((end_count_4) > (((RogueString__count( filepath_0 )))))))
      {
        return (RogueLogical)(false);
      }
      RogueInt32 i_5 = (((last_wildcard_3) + (1)));
      {
        ROGUE_DEF_LOCAL_REF(RogueString*,_auto_944_0,(filepath_0));
        RogueInt32 _auto_945_0 = (((((RogueString__count( filepath_0 )))) - (end_count_4)));
        RogueInt32 _auto_946_0 = (((((RogueString__count( _auto_944_0 )))) - (1)));
        for (;ROGUE_COND(((_auto_945_0) <= (_auto_946_0)));++_auto_945_0)
        {
          ROGUE_GC_CHECK;
          RogueCharacter ch_0 = (((RogueString__get__Int32( _auto_944_0, _auto_945_0 ))));
          RogueCharacter pattern_ch_6 = (((RogueString__get__Int32( pattern_1, i_5 ))));
          if (ROGUE_COND(ignore_case_2))
          {
            ch_0 = ((RogueCharacter)(((RogueCharacter__to_lowercase( ch_0 )))));
            pattern_ch_6 = ((RogueCharacter)(((RogueCharacter__to_lowercase( pattern_ch_6 )))));
          }
          if (ROGUE_COND(((ch_0) != (pattern_ch_6))))
          {
            return (RogueLogical)(false);
          }
          ++i_5;
        }
      }
    }
  }
  if (ROGUE_COND((RogueFile___matches_wildcard_pattern__String_Int32_Int32_String_Int32_Int32_Logical( filepath_0, 0, ROGUE_ARG(((RogueString__count( filepath_0 )))), pattern_1, 0, ROGUE_ARG(((RogueString__count( pattern_1 )))), ignore_case_2 ))))
  {
    return (RogueLogical)(true);
  }
  if (ROGUE_COND(((((((RogueString__count( filepath_0 )))) <= (2))) || (!(((RogueString__begins_with__String_Logical( filepath_0, Rogue_literal_strings[111], false ))))))))
  {
    return (RogueLogical)(false);
  }
  return (RogueLogical)((RogueFile___matches_wildcard_pattern__String_Int32_Int32_String_Int32_Int32_Logical( filepath_0, 2, ROGUE_ARG(((((RogueString__count( filepath_0 )))) - (2))), pattern_1, 0, ROGUE_ARG(((RogueString__count( pattern_1 )))), ignore_case_2 )));
}

RogueLogical RogueFile___matches_wildcard_pattern__String_Int32_Int32_String_Int32_Int32_Logical( RogueString* filepath_0, RogueInt32 f0_1, RogueInt32 fcount_2, RogueString* pattern_3, RogueInt32 p0_4, RogueInt32 pcount_5, RogueLogical ignore_case_6 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((pcount_5) == (0))))
  {
    return (RogueLogical)(((fcount_2) == (0)));
  }
  ROGUE_DEF_LOCAL_REF(RogueString*,remaining_pattern_7,(pattern_3));
  RogueInt32 r0_8 = (((p0_4) + (1)));
  RogueInt32 rcount_9 = (((pcount_5) - (1)));
  RogueCharacter ch_10 = (((RogueString__get__Int32( pattern_3, p0_4 ))));
  switch (ch_10)
  {
    case (RogueCharacter)'*':
    {
      if (ROGUE_COND(((((((rcount_9) >= (2))) && (((((RogueString__get__Int32( remaining_pattern_7, r0_8 )))) == ((RogueCharacter)'*'))))) && (((((RogueString__get__Int32( remaining_pattern_7, ROGUE_ARG(((r0_8) + (1))) )))) == ((RogueCharacter)'/'))))))
      {
        if (ROGUE_COND((RogueFile___matches_wildcard_pattern__String_Int32_Int32_String_Int32_Int32_Logical( filepath_0, f0_1, fcount_2, remaining_pattern_7, ROGUE_ARG(((r0_8) + (2))), ROGUE_ARG(((rcount_9) - (2))), ignore_case_6 ))))
        {
          return (RogueLogical)(true);
        }
        ++r0_8;
        --rcount_9;
        {
          RogueInt32 n_11 = (0);
          RogueInt32 _auto_320_12 = (fcount_2);
          for (;ROGUE_COND(((n_11) <= (_auto_320_12)));++n_11)
          {
            ROGUE_GC_CHECK;
            if (ROGUE_COND((RogueFile___matches_wildcard_pattern__String_Int32_Int32_String_Int32_Int32_Logical( filepath_0, ROGUE_ARG(((f0_1) + (n_11))), ROGUE_ARG(((fcount_2) - (n_11))), remaining_pattern_7, r0_8, rcount_9, ignore_case_6 ))))
            {
              return (RogueLogical)(true);
            }
          }
        }
      }
      else if (ROGUE_COND(((!!(rcount_9)) && (((((RogueString__get__Int32( remaining_pattern_7, r0_8 )))) == ((RogueCharacter)'*'))))))
      {
        ++r0_8;
        --rcount_9;
        {
          RogueInt32 n_13 = (0);
          RogueInt32 _auto_321_14 = (fcount_2);
          for (;ROGUE_COND(((n_13) <= (_auto_321_14)));++n_13)
          {
            ROGUE_GC_CHECK;
            if (ROGUE_COND((RogueFile___matches_wildcard_pattern__String_Int32_Int32_String_Int32_Int32_Logical( filepath_0, ROGUE_ARG(((f0_1) + (n_13))), ROGUE_ARG(((fcount_2) - (n_13))), remaining_pattern_7, r0_8, rcount_9, ignore_case_6 ))))
            {
              return (RogueLogical)(true);
            }
          }
        }
      }
      else
      {
        {
          RogueInt32 n_15 = (0);
          RogueInt32 _auto_322_16 = (fcount_2);
          for (;ROGUE_COND(((n_15) < (_auto_322_16)));++n_15)
          {
            ROGUE_GC_CHECK;
            ch_10 = ((RogueCharacter)(((RogueString__get__Int32( filepath_0, ROGUE_ARG(((f0_1) + (n_15))) )))));
            if (ROGUE_COND((RogueFile___matches_wildcard_pattern__String_Int32_Int32_String_Int32_Int32_Logical( filepath_0, ROGUE_ARG(((f0_1) + (n_15))), ROGUE_ARG(((fcount_2) - (n_15))), remaining_pattern_7, r0_8, rcount_9, ignore_case_6 ))))
            {
              return (RogueLogical)(true);
            }
            if (ROGUE_COND(((ch_10) == ((RogueCharacter)'/'))))
            {
              return (RogueLogical)(false);
            }
          }
        }
        return (RogueLogical)((RogueFile___matches_wildcard_pattern__String_Int32_Int32_String_Int32_Int32_Logical( Rogue_literal_strings[0], 0, 0, remaining_pattern_7, r0_8, rcount_9, ignore_case_6 )));
      }
      break;
    }
    case (RogueCharacter)'?':
    {
      if (ROGUE_COND(((fcount_2) == (0))))
      {
        return (RogueLogical)(false);
      }
      ch_10 = ((RogueCharacter)(((RogueString__get__Int32( filepath_0, f0_1 )))));
      if (ROGUE_COND(((ch_10) == ((RogueCharacter)'/'))))
      {
        return (RogueLogical)(false);
      }
      return (RogueLogical)((RogueFile___matches_wildcard_pattern__String_Int32_Int32_String_Int32_Int32_Logical( filepath_0, ROGUE_ARG(((f0_1) + (1))), ROGUE_ARG(((fcount_2) - (1))), remaining_pattern_7, r0_8, rcount_9, ignore_case_6 )));
    }
    default:
    {
      if (ROGUE_COND(((fcount_2) == (0))))
      {
        return (RogueLogical)(false);
      }
      RogueCharacter filepath_ch_17 = (((RogueString__get__Int32( filepath_0, f0_1 ))));
      if (ROGUE_COND(ignore_case_6))
      {
        ch_10 = ((RogueCharacter)(((RogueCharacter__to_lowercase( ch_10 )))));
        filepath_ch_17 = ((RogueCharacter)(((RogueCharacter__to_lowercase( filepath_ch_17 )))));
      }
      if (ROGUE_COND(((ch_10) == (filepath_ch_17))))
      {
        return (RogueLogical)((RogueFile___matches_wildcard_pattern__String_Int32_Int32_String_Int32_Int32_Logical( filepath_0, ROGUE_ARG(((f0_1) + (1))), ROGUE_ARG(((fcount_2) - (1))), remaining_pattern_7, r0_8, rcount_9, ignore_case_6 )));
      }
    }
  }
  return (RogueLogical)(false);
}

RogueString* RogueFile__path__String( RogueString* _auto_4011 )
{
  ROGUE_DEF_LOCAL_REF(RogueString*,filepath_0,_auto_4011);
  ROGUE_GC_CHECK;
  filepath_0 = ((RogueString*)((RogueFile__expand_path__String( filepath_0 ))));
  return (RogueString*)((RogueFile__folder__String( filepath_0 )));
}

RogueClassFileReader* RogueFile__reader__String( RogueString* _auto_4012 )
{
  ROGUE_DEF_LOCAL_REF(RogueString*,filepath_0,_auto_4012);
  ROGUE_GC_CHECK;
  filepath_0 = ((RogueString*)((RogueFile__expand_path__String( filepath_0 ))));
  return (RogueClassFileReader*)(((RogueClassFileReader*)(RogueFileReader__init__String( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassFileReader*,ROGUE_CREATE_OBJECT(FileReader))), filepath_0 ))));
}

RogueLogical RogueFile__save__String_Byte_List( RogueString* _auto_4013, RogueByte_List* data_1 )
{
  ROGUE_DEF_LOCAL_REF(RogueString*,filepath_0,_auto_4013);
  ROGUE_GC_CHECK;
  filepath_0 = ((RogueString*)((RogueFile__expand_path__String( filepath_0 ))));
  ROGUE_DEF_LOCAL_REF(RogueClassFileWriter*,outfile_2,((RogueFile__writer__String( filepath_0 ))));
  RogueFileWriter__write__Byte_List( outfile_2, data_1 );
  RogueFileWriter__close( outfile_2 );
  return (RogueLogical)(!(outfile_2->error));
}

RogueLogical RogueFile__save__String_String_Logical( RogueString* _auto_4014, RogueString* data_1, RogueLogical bom_2 )
{
  ROGUE_DEF_LOCAL_REF(RogueString*,filepath_0,_auto_4014);
  ROGUE_GC_CHECK;
  filepath_0 = ((RogueString*)((RogueFile__expand_path__String( filepath_0 ))));
  ROGUE_DEF_LOCAL_REF(RogueClassFileWriter*,outfile_3,((RogueFile__writer__String( filepath_0 ))));
  if (ROGUE_COND(bom_2))
  {
    ([&]()->RogueClassFileWriter*{
      ROGUE_DEF_LOCAL_REF(RogueClassFileWriter*,_auto_323_0,(outfile_3));
      RogueFileWriter__write__Byte( _auto_323_0, 239 );
      RogueFileWriter__write__Byte( _auto_323_0, 187 );
      RogueFileWriter__write__Byte( _auto_323_0, 191 );
       return _auto_323_0;
    }());
  }
  RogueFileWriter__write__String( outfile_3, data_1 );
  RogueFileWriter__close( outfile_3 );
  return (RogueLogical)(!(outfile_3->error));
}

RogueString* RogueFile__shell_escaped__String( RogueString* filepath_0 )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueString*,acceptable_1,0);
  if (ROGUE_COND((RogueSystem__is_windows())))
  {
    acceptable_1 = ((RogueString*)(Rogue_literal_strings[56]));
  }
  else
  {
    acceptable_1 = ((RogueString*)(Rogue_literal_strings[57]));
  }
  {
    {
      {
        {
          ROGUE_DEF_LOCAL_REF(RogueString*,_auto_845_0,(filepath_0));
          RogueInt32 _auto_846_0 = (0);
          RogueInt32 _auto_847_0 = (((((RogueString__count( _auto_845_0 )))) - (1)));
          for (;ROGUE_COND(((_auto_846_0) <= (_auto_847_0)));++_auto_846_0)
          {
            ROGUE_GC_CHECK;
            RogueCharacter ch_0 = (((RogueString__get__Int32( _auto_845_0, _auto_846_0 ))));
            if ( !(((((RogueCharacter__is_alphanumeric( ch_0 )))) || (((RogueString__contains__Character_Logical( acceptable_1, ch_0, false )))))) ) goto _auto_848;
          }
        }
        return (RogueString*)(filepath_0);
        }
    }
    _auto_848:;
    {
      if (ROGUE_COND((RogueSystem__is_windows())))
      {
        return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[38] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], filepath_0 ))) )))), Rogue_literal_strings[38] )))) ))));
      }
      else
      {
        {
          ROGUE_DEF_LOCAL_REF(RogueException*,_auto_850_0,0);
          ROGUE_DEF_LOCAL_REF(RogueClassStringBuilderPool*,_auto_849_0,(RogueStringBuilder_pool));
          ROGUE_DEF_LOCAL_REF(RogueString*,_auto_851_0,0);
          ROGUE_DEF_LOCAL_REF(RogueStringBuilder*,builder_0,(((RogueStringBuilder*)(RogueStringBuilderPool__on_use( _auto_849_0 )))));
          Rogue_ignore_unused(builder_0);

          ROGUE_TRY
          {
            {
              ROGUE_DEF_LOCAL_REF(RogueString*,_auto_853_0,(filepath_0));
              RogueInt32 _auto_854_0 = (0);
              RogueInt32 _auto_855_0 = (((((RogueString__count( _auto_853_0 )))) - (1)));
              for (;ROGUE_COND(((_auto_854_0) <= (_auto_855_0)));++_auto_854_0)
              {
                ROGUE_GC_CHECK;
                RogueCharacter ch_0 = (((RogueString__get__Int32( _auto_853_0, _auto_854_0 ))));
                if (ROGUE_COND(((((RogueCharacter__is_alphanumeric( ch_0 )))) || (((RogueString__contains__Character_Logical( acceptable_1, ch_0, false )))))))
                {
                  RogueStringBuilder__print__Character( builder_0, ch_0 );
                }
                else
                {
                  RogueStringBuilder__print__Character( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__Character( builder_0, (RogueCharacter)'\\' )))), ch_0 );
                }
              }
            }
            {
              _auto_851_0 = ((RogueString*)(((RogueString*)(RogueStringBuilder__to_String( builder_0 )))));
              goto _auto_856;
            }
          }
          ROGUE_CATCH( RogueException,_auto_852_0 )
          {
            _auto_850_0 = ((RogueException*)(_auto_852_0));
          }
          ROGUE_END_TRY
          _auto_856:;
          RogueStringBuilderPool__on_end_use__StringBuilder( _auto_849_0, builder_0 );
          if (ROGUE_COND(!!(_auto_850_0)))
          {
            throw ((RogueException*)Rogue_call_ROGUEM0( 16, _auto_850_0 ));
          }
          return (RogueString*)(_auto_851_0);
        }
      }
      }
  }
}

RogueInt64 RogueFile__size__String( RogueString* _auto_4015 )
{
  ROGUE_DEF_LOCAL_REF(RogueString*,filepath_0,_auto_4015);
  ROGUE_GC_CHECK;
  filepath_0 = ((RogueString*)((RogueFile__expand_path__String( filepath_0 ))));
  if ( !filepath_0 ) return 0;

  FILE* fp = fopen( (char*)filepath_0->utf8, "rb" );
  if ( !fp ) return 0;

  fseek( fp, 0, SEEK_END );
  RogueInt64 size = (RogueInt64) ftell( fp );
  fclose( fp );

  return size;

}

RogueReal64 RogueFile__timestamp__String( RogueString* _auto_4016 )
{
  ROGUE_DEF_LOCAL_REF(RogueString*,filepath_0,_auto_4016);
  ROGUE_GC_CHECK;
  filepath_0 = ((RogueString*)((RogueFile__expand_path__String( filepath_0 ))));
#if defined(_WIN32)
    HANDLE handle = CreateFile( (const char*)filepath_0->utf8, 0, 0, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL );
    if (handle != INVALID_HANDLE_VALUE)
    {
      BY_HANDLE_FILE_INFORMATION info;
      if (GetFileInformationByHandle( handle, &info ))
      {
        RogueInt64 result = info.ftLastWriteTime.dwHighDateTime;
        result <<= 32;
        result |= info.ftLastWriteTime.dwLowDateTime;
        result /= 10000; // convert from Crazyseconds to Milliseconds
        result -= 11644473600000;  // base on Jan 1, 1970 instead of Jan 1, 1601 (?!)
        CloseHandle(handle);
        return result / 1000.0;
      }
      CloseHandle(handle);
    }

#elif defined(ROGUE_PLATFORM_UNIX_COMPATIBLE)
    int file_id = open( (const char*)filepath_0->utf8, O_RDONLY );
    if (file_id >= 0)
    {
      struct stat info;
      if (0 == fstat(file_id, &info))
      {
#if defined(__APPLE__)
        RogueInt64 result = info.st_mtimespec.tv_sec;
        result *= 1000000000;
        result += info.st_mtimespec.tv_nsec;
        result /= 1000000;  // convert to milliseconds
#else
        RogueInt64 result = (RogueInt64) info.st_mtime;
        result *= 1000;  // convert to ms
#endif
        close(file_id);
        return result / 1000.0;
      }
      close(file_id);
    }

#else
# error Must define File.timestamp() for this OS.
#endif
  return 0.0;

}

RogueString* RogueFile__without_trailing_separator__String( RogueString* _auto_4017 )
{
  ROGUE_DEF_LOCAL_REF(RogueString*,filepath_0,_auto_4017);
  ROGUE_GC_CHECK;
  while (ROGUE_COND(((((((RogueString__count( filepath_0 )))) > (1))) && ((RogueFile__ends_with_separator__String( filepath_0 ))))))
  {
    ROGUE_GC_CHECK;
    filepath_0 = ((RogueString*)(((RogueString*)(RogueString__leftmost__Int32( filepath_0, -1 )))));
  }
  return (RogueString*)(filepath_0);
}

RogueClassFileWriter* RogueFile__writer__String( RogueString* _auto_4018 )
{
  ROGUE_DEF_LOCAL_REF(RogueString*,filepath_0,_auto_4018);
  ROGUE_GC_CHECK;
  filepath_0 = ((RogueString*)((RogueFile__expand_path__String( filepath_0 ))));
  return (RogueClassFileWriter*)(((RogueClassFileWriter*)(RogueFileWriter__init__String_Logical( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassFileWriter*,ROGUE_CREATE_OBJECT(FileWriter))), filepath_0, false ))));
}

void RogueRuntime__init_class()
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueString*,value_0,(((RogueString*)(RogueSystemEnvironment__get__String( ROGUE_ARG((RogueSystem__environment())), Rogue_literal_strings[280] )))));
  if (ROGUE_COND(((void*)value_0) != ((void*)NULL)))
  {
    RogueReal64 n_1 = (((RogueString__to_Real64( value_0 ))));
    if (ROGUE_COND(((((RogueString__ends_with__Character_Logical( value_0, (RogueCharacter)'M', false )))) || (((RogueString__ends_with__String_Logical( value_0, Rogue_literal_strings[281], false )))))))
    {
      n_1 *= 1048576;
    }
    else if (ROGUE_COND(((((RogueString__ends_with__Character_Logical( value_0, (RogueCharacter)'K', false )))) || (((RogueString__ends_with__String_Logical( value_0, Rogue_literal_strings[282], false )))))))
    {
      n_1 *= 1024;
    }
    RogueRuntime__set_gc_threshold__Int32( ROGUE_ARG(((RogueInt32)(n_1))) );
  }
}

void RogueRuntime__set_gc_threshold__Int32( RogueInt32 value_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((value_0) <= (0))))
  {
    value_0 = ((RogueInt32)(2147483647));
  }
  Rogue_gc_threshold = value_0;

}

RogueClassSystemEnvironment* RogueSystem__env()
{
  ROGUE_GC_CHECK;
  return (RogueClassSystemEnvironment*)(((RogueClassSystemEnvironment*)ROGUE_SINGLETON(SystemEnvironment)));
}

RogueClassSystemEnvironment* RogueSystem__environment()
{
  ROGUE_GC_CHECK;
  return (RogueClassSystemEnvironment*)(((RogueClassSystemEnvironment*)ROGUE_SINGLETON(SystemEnvironment)));
}

void RogueSystem__exit__Int32( RogueInt32 result_code_0 )
{
  ROGUE_GC_CHECK;
  Rogue_quit();
  exit( result_code_0 );

}

RogueLogical RogueSystem__is_linux()
{
  ROGUE_GC_CHECK;
  RogueLogical result_0 = (false);
#if defined(ROGUE_OS_LINUX)

  result_0 = ((RogueLogical)(true));
#endif

  return (RogueLogical)(result_0);
}

RogueLogical RogueSystem__is_macos()
{
  ROGUE_GC_CHECK;
  RogueLogical result_0 = (false);
#if defined(ROGUE_OS_MACOS)

  result_0 = ((RogueLogical)(true));
#endif

  return (RogueLogical)(result_0);
}

RogueLogical RogueSystem__is_windows()
{
  ROGUE_GC_CHECK;
  RogueLogical result_0 = (false);
#if defined(ROGUE_OS_WINDOWS)

  result_0 = ((RogueLogical)(true));
#endif

  return (RogueLogical)(result_0);
}

RogueString* RogueSystem__os()
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueString*,result_0,0);
#if defined(ROGUE_OS_ANDROID)

  result_0 = ((RogueString*)(Rogue_literal_strings[190]));
#elif defined(ROGUE_OS_CYGWIN)

  result_0 = ((RogueString*)(Rogue_literal_strings[191]));
#elif defined(ROGUE_OS_IOS)

  result_0 = ((RogueString*)(Rogue_literal_strings[192]));
#elif defined(ROGUE_OS_MACOS)

  result_0 = ((RogueString*)(Rogue_literal_strings[193]));
#elif defined(ROGUE_OS_WEB)

  result_0 = ((RogueString*)(Rogue_literal_strings[194]));
#elif defined(ROGUE_OS_WINDOWS)

  result_0 = ((RogueString*)(Rogue_literal_strings[195]));
#else

  result_0 = ((RogueString*)(Rogue_literal_strings[196]));
#endif

  return (RogueString*)(result_0);
}

RogueInt32 RogueSystem__run__String( RogueString* command_0 )
{
  ROGUE_GC_CHECK;
  RogueInt32 return_val_1 = (0);
  return_val_1 = system( (char*)command_0->utf8 );

  if (ROGUE_COND(((return_val_1) == (-1))))
  {
    return (RogueInt32)(-1);
  }
#if !defined(ROGUE_PLATFORM_WINDOWS)
  return_val_1 = (RogueInt32) WEXITSTATUS(return_val_1);
#endif

  return (RogueInt32)(return_val_1);
}

void RogueSystem__sync_storage()
{
  ROGUE_GC_CHECK;
}

void RogueSystem__init_class()
{
  RogueSystem_command_line_arguments = ((RogueString_List*)(RogueString_List__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueString_List*,ROGUE_CREATE_OBJECT(String_List))) )));
}

RogueReal64 RogueMath__ceiling__Real64( RogueReal64 n_0 )
{
  return (RogueReal64)(((RogueReal64)(ceil((double)n_0))));
}

RogueReal64 RogueMath__floor__Real64( RogueReal64 n_0 )
{
  return (RogueReal64)(((RogueReal64)(floor((double)n_0))));
}

RogueInt32 RogueMath__max__Int32_Int32( RogueInt32 a_0, RogueInt32 b_1 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((a_0) >= (b_1))))
  {
    return (RogueInt32)(a_0);
  }
  else
  {
    return (RogueInt32)(b_1);
  }
}

RogueInt64 RogueMath__mod__Int64_Int64( RogueInt64 a_0, RogueInt64 b_1 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((((!(!!(a_0))) && (!(!!(b_1))))) || (((b_1) == (1LL))))))
  {
    return (RogueInt64)(0LL);
  }
  RogueInt64 r_2 = (((RogueInt64)(a_0 % b_1
  )));
  if (ROGUE_COND(((((a_0) ^ (b_1))) < (0LL))))
  {
    if (ROGUE_COND(!!(r_2)))
    {
      return (RogueInt64)(((r_2) + (b_1)));
    }
    else
    {
      return (RogueInt64)(0LL);
    }
  }
  else
  {
    return (RogueInt64)(r_2);
  }
}

RogueReal64 RogueMath__mod__Real64_Real64( RogueReal64 a_0, RogueReal64 b_1 )
{
  ROGUE_GC_CHECK;
  RogueReal64 q_2 = (((a_0) / (b_1)));
  return (RogueReal64)(((a_0) - (((floor((double)q_2)) * (b_1)))));
}

RogueInt32 RogueMath__shift_right__Int32_Int32( RogueInt32 value_0, RogueInt32 bits_1 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((bits_1) <= (0))))
  {
    return (RogueInt32)(value_0);
  }
  --bits_1;
  if (ROGUE_COND(!!(bits_1)))
  {
    return (RogueInt32)(((((((value_0) >> (1))) & (2147483647))) >> (bits_1)));
  }
  else
  {
    return (RogueInt32)(((((value_0) >> (1))) & (2147483647)));
  }
}

RogueInt64 RogueMath__shift_right__Int64_Int64( RogueInt64 value_0, RogueInt64 bits_1 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((bits_1) <= (0LL))))
  {
    return (RogueInt64)(value_0);
  }
  --bits_1;
  if (ROGUE_COND(!!(bits_1)))
  {
    return (RogueInt64)(((((((value_0) >> (1LL))) & (9223372036854775807LL))) >> (bits_1)));
  }
  else
  {
    return (RogueInt64)(((((value_0) >> (1LL))) & (9223372036854775807LL)));
  }
}

RogueStringBuilder* RogueStringValue__to_json__String_StringBuilder_Int32( RogueString* value_0, RogueStringBuilder* buffer_1, RogueInt32 flags_2 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!!(value_0)))
  {
    RogueStringBuilder__print__Character( buffer_1, (RogueCharacter)'"' );
    {
      ROGUE_DEF_LOCAL_REF(RogueString*,_auto_484_0,(value_0));
      RogueInt32 _auto_485_0 = (0);
      RogueInt32 _auto_486_0 = (((((RogueString__count( _auto_484_0 )))) - (1)));
      for (;ROGUE_COND(((_auto_485_0) <= (_auto_486_0)));++_auto_485_0)
      {
        ROGUE_GC_CHECK;
        RogueCharacter ch_0 = (((RogueString__get__Int32( _auto_484_0, _auto_485_0 ))));
        switch (ch_0)
        {
          case (RogueCharacter)'"':
          {
            RogueStringBuilder__print__String( buffer_1, Rogue_literal_strings[415] );
            break;
          }
          case (RogueCharacter)'\\':
          {
            RogueStringBuilder__print__String( buffer_1, Rogue_literal_strings[248] );
            break;
          }
          case (RogueCharacter)8:
          {
            RogueStringBuilder__print__String( buffer_1, Rogue_literal_strings[250] );
            break;
          }
          case (RogueCharacter)12:
          {
            RogueStringBuilder__print__String( buffer_1, Rogue_literal_strings[252] );
            break;
          }
          case (RogueCharacter)10:
          {
            RogueStringBuilder__print__String( buffer_1, Rogue_literal_strings[253] );
            break;
          }
          case (RogueCharacter)13:
          {
            RogueStringBuilder__print__String( buffer_1, Rogue_literal_strings[416] );
            break;
          }
          case (RogueCharacter)9:
          {
            RogueStringBuilder__print__String( buffer_1, Rogue_literal_strings[254] );
            break;
          }
          default:
          {
            if (ROGUE_COND(((((((RogueInt32)(ch_0))) >= (32))) && (((((RogueInt32)(ch_0))) <= (126))))))
            {
              RogueStringBuilder__print__Character( buffer_1, ch_0 );
            }
            else if (ROGUE_COND(((((((((((RogueInt32)(ch_0))) < (32))) || (((((RogueInt32)(ch_0))) == (127))))) || (((((RogueInt32)(ch_0))) == (8232))))) || (((((RogueInt32)(ch_0))) == (8233))))))
            {
              RogueStringBuilder__print__String( buffer_1, Rogue_literal_strings[417] );
              RogueInt32 n_3 = (((RogueInt32)(ch_0)));
              {
                RogueInt32 nibble_5 = (0);
                RogueInt32 _auto_411_6 = (3);
                for (;ROGUE_COND(((nibble_5) <= (_auto_411_6)));++nibble_5)
                {
                  ROGUE_GC_CHECK;
                  RogueInt32 digit_4 = (((((n_3) >> (12))) & (15)));
                  n_3 = ((RogueInt32)(((n_3) << (4))));
                  if (ROGUE_COND(((digit_4) <= (9))))
                  {
                    RogueStringBuilder__print__Int32( buffer_1, digit_4 );
                  }
                  else
                  {
                    RogueStringBuilder__print__Character( buffer_1, ROGUE_ARG(((RogueCharacter)(((97) + (((digit_4) - (10))))))) );
                  }
                }
              }
            }
            else
            {
              RogueStringBuilder__print__Character( buffer_1, ch_0 );
            }
          }
        }
      }
    }
    RogueStringBuilder__print__Character( buffer_1, (RogueCharacter)'"' );
  }
  else
  {
    RogueStringBuilder__print__String( buffer_1, Rogue_literal_strings[6] );
  }
  return (RogueStringBuilder*)(buffer_1);
}

void RogueStringValue__init_class()
{
  RogueStringValue_empty_string = ((RogueClassStringValue*)(RogueStringValue__init__String( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassStringValue*,ROGUE_CREATE_OBJECT(StringValue))), Rogue_literal_strings[0] )));
}

void RogueLogicalValue__init_class()
{
  ROGUE_GC_CHECK;
  RogueLogicalValue_true_value = ((RogueClassLogicalValue*)(RogueLogicalValue__init__Logical( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassLogicalValue*,ROGUE_CREATE_OBJECT(LogicalValue))), true )));
  RogueLogicalValue_false_value = ((RogueClassLogicalValue*)(RogueLogicalValue__init__Logical( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassLogicalValue*,ROGUE_CREATE_OBJECT(LogicalValue))), false )));
}

RogueClassValue* RogueJSON__parse__String_Logical( RogueString* json_0, RogueLogical suppress_error_1 )
{
  ROGUE_GC_CHECK;
  ROGUE_TRY
  {
    return (RogueClassValue*)(((RogueClassValue*)(RogueJSONParser__parse_value( ROGUE_ARG(((RogueClassJSONParser*)(RogueJSONParser__init__String( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassJSONParser*,ROGUE_CREATE_OBJECT(JSONParser))), json_0 )))) ))));
  }
  ROGUE_CATCH( RogueClassJSONParseError,err_2 )
  {
    if (ROGUE_COND(!(suppress_error_1)))
    {
      throw ((RogueException*)(RogueJSONParseError___throw( err_2 )));
    }
    return (RogueClassValue*)(((RogueClassValue*)(((RogueClassUndefinedValue*)ROGUE_SINGLETON(UndefinedValue)))));
  }
  ROGUE_END_TRY
}

RogueClassProcess* RogueProcess__create__String_Logical_Logical_Logical_Logical_Logical( RogueString* cmd_0, RogueLogical readable_1, RogueLogical writable_2, RogueLogical is_blocking_3, RogueLogical inherit_environment_4, RogueLogical env_5 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND((RogueSystem__is_windows())))
  {
    return (RogueClassProcess*)(((RogueClassProcess*)(((RogueClassWindowsProcess*)(RogueWindowsProcess__init__String_Logical_Logical_Logical_Logical_Logical( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassWindowsProcess*,ROGUE_CREATE_OBJECT(WindowsProcess))), cmd_0, readable_1, writable_2, is_blocking_3, inherit_environment_4, env_5 ))))));
  }
  else
  {
    ROGUE_DEF_LOCAL_REF(RogueString_List*,_auto_823_0,(((RogueString_List*)(RogueString_List__init__Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueString_List*,ROGUE_CREATE_OBJECT(String_List))), 3 )))));
    {
      RogueString_List__add__String( _auto_823_0, Rogue_literal_strings[43] );
      RogueString_List__add__String( _auto_823_0, Rogue_literal_strings[44] );
      RogueString_List__add__String( _auto_823_0, cmd_0 );
    }
    return (RogueClassProcess*)(((RogueClassProcess*)(((RogueClassPosixProcess*)(RoguePosixProcess__init__String_List_Logical_Logical_Logical_Logical_Logical( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassPosixProcess*,ROGUE_CREATE_OBJECT(PosixProcess))), _auto_823_0, readable_1, writable_2, is_blocking_3, inherit_environment_4, env_5 ))))));
  }
}

RogueClassProcessResult* RogueProcess__run__String_Logical_Logical_Logical( RogueString* cmd_0, RogueLogical inherit_environment_1, RogueLogical env_2, RogueLogical writable_3 )
{
  ROGUE_GC_CHECK;
  return (RogueClassProcessResult*)((RogueProcess__run__Process( ROGUE_ARG((RogueProcess__create__String_Logical_Logical_Logical_Logical_Logical( cmd_0, true, writable_3, true, inherit_environment_1, env_2 ))) )));
}

RogueClassProcessResult* RogueProcess__run__Process( RogueClassProcess* process_0 )
{
  ROGUE_GC_CHECK;
  return (RogueClassProcessResult*)(((RogueClassProcessResult*)Rogue_call_ROGUEM0( 19, process_0 )));
}

RogueClassListRewriter_String_* RogueListRewriter_String___acquire__String_List( RogueString_List* list_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((!!(RogueListRewriter_String__pool)) && (!!(RogueListRewriter_String__pool->count)))))
  {
    return (RogueClassListRewriter_String_*)(([&]()->RogueClassListRewriter_String_*{
      ROGUE_DEF_LOCAL_REF(RogueClassListRewriter_String_*,_auto_905_0,(((RogueClassListRewriter_String_*)(RogueObject_as(((RogueClassGenericListRewriter*)(RogueGenericListRewriter_List__remove_last( ROGUE_ARG(RogueListRewriter_String__pool) ))),RogueTypeListRewriter_String_)))));
      RogueListRewriter_String___reset__String_List( _auto_905_0, list_0 );
       return _auto_905_0;
    }())
    );
  }
  else
  {
    return (RogueClassListRewriter_String_*)(((RogueClassListRewriter_String_*)(RogueListRewriter_String___init__String_List( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassListRewriter_String_*,ROGUE_CREATE_OBJECT(ListRewriter_String_))), list_0 ))));
  }
}

RogueString_List* RogueQuicksort_String___sort__String_List__Function_String_String_RETURNSLogical_( RogueString_List* list_0, RogueClass_Function_String_String_RETURNSLogical_* compare_fn_1 )
{
  ROGUE_GC_CHECK;
  RogueQuicksort_String___sort__Array__Function_String_String_RETURNSLogical__Int32_Int32( ROGUE_ARG(list_0->data), compare_fn_1, 0, ROGUE_ARG(((list_0->count) - (1))) );
  return (RogueString_List*)(list_0);
}

void RogueQuicksort_String___sort__Array__Function_String_String_RETURNSLogical__Int32_Int32( RogueArray* data_0, RogueClass_Function_String_String_RETURNSLogical_* compare_fn_1, RogueInt32 i1_2, RogueInt32 i2_3 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((i1_2) >= (i2_3))))
  {
    return;
  }
  else if (ROGUE_COND(((((i1_2) + (1))) == (i2_3))))
  {
    ROGUE_DEF_LOCAL_REF(RogueString*,a_4,(((RogueString*)(data_0->as_objects[i1_2]))));
    ROGUE_DEF_LOCAL_REF(RogueString*,b_5,(((RogueString*)(data_0->as_objects[i2_3]))));
    if (ROGUE_COND(!((Rogue_call_ROGUEM11( 13, compare_fn_1, a_4, b_5 )))))
    {
      data_0->as_objects[i2_3] = a_4;
      data_0->as_objects[i1_2] = b_5;
    }
    return;
  }
  else if (ROGUE_COND(((((i1_2) + (2))) == (i2_3))))
  {
    RogueInt32 ib_6 = (((i1_2) + (1)));
    ROGUE_DEF_LOCAL_REF(RogueString*,a_7,(((RogueString*)(data_0->as_objects[i1_2]))));
    ROGUE_DEF_LOCAL_REF(RogueString*,b_8,(((RogueString*)(data_0->as_objects[ib_6]))));
    ROGUE_DEF_LOCAL_REF(RogueString*,c_9,(((RogueString*)(data_0->as_objects[i2_3]))));
    if (ROGUE_COND((Rogue_call_ROGUEM11( 13, compare_fn_1, a_7, b_8 ))))
    {
      if (ROGUE_COND((Rogue_call_ROGUEM11( 13, compare_fn_1, b_8, c_9 ))))
      {
        return;
      }
      else if (ROGUE_COND((Rogue_call_ROGUEM11( 13, compare_fn_1, a_7, c_9 ))))
      {
        data_0->as_objects[ib_6] = c_9;
        data_0->as_objects[i2_3] = b_8;
        return;
      }
      else
      {
        data_0->as_objects[i1_2] = c_9;
        data_0->as_objects[ib_6] = a_7;
        data_0->as_objects[i2_3] = b_8;
        return;
      }
    }
    else if (ROGUE_COND((Rogue_call_ROGUEM11( 13, compare_fn_1, a_7, c_9 ))))
    {
      data_0->as_objects[i1_2] = b_8;
      data_0->as_objects[ib_6] = a_7;
      return;
    }
    else if (ROGUE_COND((Rogue_call_ROGUEM11( 13, compare_fn_1, b_8, c_9 ))))
    {
      data_0->as_objects[i1_2] = b_8;
      data_0->as_objects[ib_6] = c_9;
      data_0->as_objects[i2_3] = a_7;
      return;
    }
    else
    {
      data_0->as_objects[i1_2] = c_9;
      data_0->as_objects[ib_6] = b_8;
      data_0->as_objects[i2_3] = a_7;
      return;
    }
  }
  RogueInt32 pivot_index_10 = ((RogueMath__shift_right__Int32_Int32( ROGUE_ARG(((i1_2) + (i2_3))), 1 )));
  ROGUE_DEF_LOCAL_REF(RogueString*,pivot_11,(((RogueString*)(data_0->as_objects[pivot_index_10]))));
  ROGUE_DEF_LOCAL_REF(RogueString*,first_12,(((RogueString*)(data_0->as_objects[i1_2]))));
  if (ROGUE_COND((Rogue_call_ROGUEM11( 13, compare_fn_1, pivot_11, first_12 ))))
  {
    pivot_11 = ((RogueString*)(first_12));
    pivot_index_10 = ((RogueInt32)(i1_2));
  }
  ROGUE_DEF_LOCAL_REF(RogueString*,last_13,(((RogueString*)(data_0->as_objects[i2_3]))));
  if (ROGUE_COND((Rogue_call_ROGUEM11( 13, compare_fn_1, last_13, pivot_11 ))))
  {
    pivot_11 = ((RogueString*)(last_13));
    pivot_index_10 = ((RogueInt32)(i2_3));
  }
  ROGUE_DEF_LOCAL_REF(RogueString*,temp_14,(((RogueString*)(data_0->as_objects[pivot_index_10]))));
  data_0->as_objects[pivot_index_10] = ((RogueString*)(data_0->as_objects[i2_3]));
  data_0->as_objects[i2_3] = temp_14;
  pivot_index_10 = ((RogueInt32)(i1_2));
  {
    RogueInt32 i_15 = (i1_2);
    RogueInt32 _auto_949_16 = (((i2_3) - (1)));
    for (;ROGUE_COND(((i_15) <= (_auto_949_16)));++i_15)
    {
      ROGUE_GC_CHECK;
      if (ROGUE_COND((Rogue_call_ROGUEM11( 13, compare_fn_1, ROGUE_ARG(((RogueString*)(data_0->as_objects[i_15]))), pivot_11 ))))
      {
        temp_14 = ((RogueString*)(((RogueString*)(data_0->as_objects[i_15]))));
        data_0->as_objects[i_15] = ((RogueString*)(data_0->as_objects[pivot_index_10]));
        data_0->as_objects[pivot_index_10] = temp_14;
        ++pivot_index_10;
      }
    }
  }
  temp_14 = ((RogueString*)(((RogueString*)(data_0->as_objects[pivot_index_10]))));
  data_0->as_objects[pivot_index_10] = ((RogueString*)(data_0->as_objects[i2_3]));
  data_0->as_objects[i2_3] = temp_14;
  RogueQuicksort_String___sort__Array__Function_String_String_RETURNSLogical__Int32_Int32( data_0, compare_fn_1, i1_2, ROGUE_ARG(((pivot_index_10) - (1))) );
  RogueQuicksort_String___sort__Array__Function_String_String_RETURNSLogical__Int32_Int32( data_0, compare_fn_1, ROGUE_ARG(((pivot_index_10) + (1))), i2_3 );
}

RogueClassPlatforms* RoguePlatforms__unix()
{
  ROGUE_GC_CHECK;
  return (RogueClassPlatforms*)(((RogueClassPlatforms*)(RoguePlatforms__init__String_Logical_Logical_Logical( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassPlatforms*,ROGUE_CREATE_OBJECT(Platforms))), Rogue_literal_strings[151], false, false, false ))));
}

RogueClassPlatforms* RoguePlatforms__windows()
{
  ROGUE_GC_CHECK;
  return (RogueClassPlatforms*)(((RogueClassPlatforms*)(RoguePlatforms__init__String_Logical_Logical_Logical( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassPlatforms*,ROGUE_CREATE_OBJECT(Platforms))), Rogue_literal_strings[153], false, false, false ))));
}

void RogueStringEncoding__init_class()
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueInt32_List*,_auto_1990_0,(((RogueInt32_List*)(RogueInt32_List__init__Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueInt32_List*,ROGUE_CREATE_OBJECT(Int32_List))), 5 )))));
  {
    RogueInt32_List__add__Int32( _auto_1990_0, 0 );
    RogueInt32_List__add__Int32( _auto_1990_0, 1 );
    RogueInt32_List__add__Int32( _auto_1990_0, 2 );
    RogueInt32_List__add__Int32( _auto_1990_0, 2 );
    RogueInt32_List__add__Int32( _auto_1990_0, 3 );
  }
  RogueStringEncoding_values = _auto_1990_0;
}

RogueOptionalString RogueOptionalString__create()
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueString*,default_value_0,0);
  return (RogueOptionalString)(RogueOptionalString( default_value_0, false ));
}

RogueOptionalInt32 RogueOptionalInt32__create()
{
  ROGUE_GC_CHECK;
  RogueInt32 default_value_0 = 0;
  return (RogueOptionalInt32)(RogueOptionalInt32( default_value_0, false ));
}

RogueOptionalByte RogueOptionalByte__create()
{
  ROGUE_GC_CHECK;
  RogueByte default_value_0 = 0;
  return (RogueOptionalByte)(RogueOptionalByte( default_value_0, false ));
}

void RogueConsoleEventType__init_class()
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueInt32_List*,_auto_2555_0,(((RogueInt32_List*)(RogueInt32_List__init__Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueInt32_List*,ROGUE_CREATE_OBJECT(Int32_List))), 7 )))));
  {
    RogueInt32_List__add__Int32( _auto_2555_0, 0 );
    RogueInt32_List__add__Int32( _auto_2555_0, 1 );
    RogueInt32_List__add__Int32( _auto_2555_0, 2 );
    RogueInt32_List__add__Int32( _auto_2555_0, 3 );
    RogueInt32_List__add__Int32( _auto_2555_0, 4 );
    RogueInt32_List__add__Int32( _auto_2555_0, 5 );
    RogueInt32_List__add__Int32( _auto_2555_0, 6 );
  }
  RogueConsoleEventType_values = _auto_2555_0;
}

RogueOptionalSpan RogueOptionalSpan__create()
{
  ROGUE_GC_CHECK;
  RogueClassSpan default_value_0 = RogueClassSpan();
  return (RogueOptionalSpan)(RogueOptionalSpan( default_value_0, false ));
}

RogueOptionalFilePattern RogueOptionalFilePattern__create()
{
  ROGUE_GC_CHECK;
  RogueClassFilePattern default_value_0 = RogueClassFilePattern();
  return (RogueOptionalFilePattern)(RogueOptionalFilePattern( default_value_0, false ));
}

RogueOptionalCharacter RogueOptionalCharacter__create()
{
  ROGUE_GC_CHECK;
  RogueCharacter default_value_0 = 0;
  return (RogueOptionalCharacter)(RogueOptionalCharacter( default_value_0, false ));
}

void RogueUnixConsoleMouseEventType__init_class()
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueInt32_List*,_auto_3512_0,(((RogueInt32_List*)(RogueInt32_List__init__Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueInt32_List*,ROGUE_CREATE_OBJECT(Int32_List))), 8 )))));
  {
    RogueInt32_List__add__Int32( _auto_3512_0, 32 );
    RogueInt32_List__add__Int32( _auto_3512_0, 34 );
    RogueInt32_List__add__Int32( _auto_3512_0, 35 );
    RogueInt32_List__add__Int32( _auto_3512_0, 64 );
    RogueInt32_List__add__Int32( _auto_3512_0, 66 );
    RogueInt32_List__add__Int32( _auto_3512_0, 67 );
    RogueInt32_List__add__Int32( _auto_3512_0, 96 );
    RogueInt32_List__add__Int32( _auto_3512_0, 97 );
  }
  RogueUnixConsoleMouseEventType_values = _auto_3512_0;
}


RogueClassGlobal* RogueGlobal__init_object( RogueClassGlobal* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  THIS->global_output_buffer = ((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )));
  THIS->console = ((RogueClassPrintWriter*)(((RogueClassConsole*)ROGUE_SINGLETON(Console))));
  return (RogueClassGlobal*)(THIS);
}

RogueClassGlobal* RogueGlobal__init( RogueClassGlobal* THIS )
{
  ROGUE_GC_CHECK;
  signal( SIGINT, RogueGlobal__on_control_c__Int32 );

  RogueGlobal__on_exit___Function___( ROGUE_ARG(THIS), ROGUE_ARG(((RogueClass_Function___*)(((RogueClassFunction_819*)ROGUE_SINGLETON(Function_819))))) );
  return (RogueClassGlobal*)(THIS);
}

RogueString* RogueGlobal__type_name( RogueClassGlobal* THIS )
{
  return (RogueString*)(Rogue_literal_strings[324]);
}

void RogueGlobal__flush( RogueClassGlobal* THIS )
{
  ROGUE_GC_CHECK;
  RogueGlobal__flush__StringBuilder( ROGUE_ARG(THIS), ROGUE_ARG(THIS->global_output_buffer) );
}

RogueClassGlobal* RogueGlobal__print__Character( RogueClassGlobal* THIS, RogueCharacter value_0 )
{
  ROGUE_GC_CHECK;
  RogueStringBuilder__print__Character( ROGUE_ARG(THIS->global_output_buffer), value_0 );
  if (ROGUE_COND(((value_0) == ((RogueCharacter)10))))
  {
    RogueGlobal__flush( ROGUE_ARG(THIS) );
  }
  return (RogueClassGlobal*)(THIS);
}

RogueClassGlobal* RogueGlobal__print__String( RogueClassGlobal* THIS, RogueString* value_0 )
{
  ROGUE_GC_CHECK;
  RogueStringBuilder__print__String( ROGUE_ARG(THIS->global_output_buffer), value_0 );
  if (ROGUE_COND(((THIS->global_output_buffer->count) > (1024))))
  {
    RogueGlobal__flush( ROGUE_ARG(THIS) );
  }
  return (RogueClassGlobal*)(THIS);
}

RogueClassGlobal* RogueGlobal__print__StringBuilder( RogueClassGlobal* THIS, RogueStringBuilder* value_0 )
{
  ROGUE_GC_CHECK;
  RogueStringBuilder__print__StringBuilder( ROGUE_ARG(THIS->global_output_buffer), value_0 );
  return (RogueClassGlobal*)(THIS);
}

RogueClassGlobal* RogueGlobal__println( RogueClassGlobal* THIS )
{
  ROGUE_GC_CHECK;
  RogueStringBuilder__print__Character( ROGUE_ARG(THIS->global_output_buffer), (RogueCharacter)10 );
  RogueGlobal__flush( ROGUE_ARG(THIS) );
  return (RogueClassGlobal*)(THIS);
}

RogueClassGlobal* RogueGlobal__println__String( RogueClassGlobal* THIS, RogueString* value_0 )
{
  ROGUE_GC_CHECK;
  return (RogueClassGlobal*)(((RogueClassGlobal*)(RogueGlobal__println( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ROGUE_ARG(THIS), value_0 )))) ))));
}

void RogueGlobal__flush__StringBuilder( RogueClassGlobal* THIS, RogueStringBuilder* buffer_0 )
{
  ROGUE_GC_CHECK;
  RoguePrintWriter__flush( ROGUE_ARG(((((RogueObject*)(RoguePrintWriter__print__StringBuilder( ROGUE_ARG(((((RogueObject*)THIS->console)))), buffer_0 )))))) );
  RogueStringBuilder__clear( buffer_0 );
}

void RogueGlobal__on_launch( RogueClassGlobal* THIS )
{
  ROGUE_GC_CHECK;
  ROGUE_TRY
  {
    RogueMorlock__init__String_List( ((RogueClassMorlock*)ROGUE_SINGLETON(Morlock)), ROGUE_ARG(RogueSystem_command_line_arguments) );
  }
  ROGUE_CATCH( RogueClassError,error_0 )
  {
    ROGUE_DEF_LOCAL_REF(RogueString*,mesg_1,(((RogueString*)(RogueException__to_String( ((RogueException*)error_0) )))));
    RogueConsoleErrorPrinter__println__String( ROGUE_ARG(((RogueClassConsole*)ROGUE_SINGLETON(Console))->error), mesg_1 );
    RogueSystem__exit__Int32( 1 );
  }
  ROGUE_END_TRY
}

void RogueGlobal__run_tests( RogueClassGlobal* THIS )
{
}

void RogueGlobal__call_exit_functions( RogueClassGlobal* THIS )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(Rogue_Function____List*,functions_0,(THIS->exit_functions));
  THIS->exit_functions = ((Rogue_Function____List*)(NULL));
  if (ROGUE_COND(!!(functions_0)))
  {
    {
      ROGUE_DEF_LOCAL_REF(Rogue_Function____List*,_auto_816_0,(functions_0));
      RogueInt32 _auto_817_0 = (0);
      RogueInt32 _auto_818_0 = (((_auto_816_0->count) - (1)));
      for (;ROGUE_COND(((_auto_817_0) <= (_auto_818_0)));++_auto_817_0)
      {
        ROGUE_GC_CHECK;
        ROGUE_DEF_LOCAL_REF(RogueClass_Function___*,fn_0,(((RogueClass_Function___*)(_auto_816_0->data->as_objects[_auto_817_0]))));
        Rogue_call_ROGUEM1( 13, fn_0 );
      }
    }
  }
}

void RogueGlobal__on_exit___Function___( RogueClassGlobal* THIS, RogueClass_Function___* fn_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(!!(THIS->exit_functions))))
  {
    THIS->exit_functions = ((Rogue_Function____List*)(Rogue_Function____List__init( ROGUE_ARG(ROGUE_CREATE_REF(Rogue_Function____List*,ROGUE_CREATE_OBJECT(_Function____List))) )));
  }
  Rogue_Function____List__add___Function___( ROGUE_ARG(THIS->exit_functions), fn_0 );
}

void RogueBufferedPrintWriter_global_output_buffer___flush( RogueObject* THIS )
{
  switch (THIS->type->index)
  {
    case 0:
      RogueGlobal__flush( (RogueClassGlobal*)THIS );
    return;
  }
}

RogueClassBufferedPrintWriter_global_output_buffer_* RogueBufferedPrintWriter_global_output_buffer___print__Character( RogueObject* THIS, RogueCharacter value_0 )
{
  switch (THIS->type->index)
  {
    case 0:
      return (RogueClassBufferedPrintWriter_global_output_buffer_*)RogueGlobal__print__Character( (RogueClassGlobal*)THIS, value_0 );
    default:
      return 0;
  }
}

RogueClassBufferedPrintWriter_global_output_buffer_* RogueBufferedPrintWriter_global_output_buffer___print__String( RogueObject* THIS, RogueString* value_0 )
{
  switch (THIS->type->index)
  {
    case 0:
      return (RogueClassBufferedPrintWriter_global_output_buffer_*)RogueGlobal__print__String( (RogueClassGlobal*)THIS, value_0 );
    default:
      return 0;
  }
}

RogueClassBufferedPrintWriter_global_output_buffer_* RogueBufferedPrintWriter_global_output_buffer___print__StringBuilder( RogueObject* THIS, RogueStringBuilder* value_0 )
{
  switch (THIS->type->index)
  {
    case 0:
      return (RogueClassBufferedPrintWriter_global_output_buffer_*)RogueGlobal__print__StringBuilder( (RogueClassGlobal*)THIS, value_0 );
    default:
      return 0;
  }
}

void RoguePrintWriter__flush( RogueObject* THIS )
{
  switch (THIS->type->index)
  {
    case 0:
      RogueGlobal__flush( (RogueClassGlobal*)THIS );
    return;
    case 3:
      RogueStringBuilder__flush( (RogueStringBuilder*)THIS );
    return;
    case 61:
      RogueConsole__flush( (RogueClassConsole*)THIS );
    return;
    case 63:
      RogueConsoleErrorPrinter__flush( (RogueClassConsoleErrorPrinter*)THIS );
    return;
  }
}

RogueClassPrintWriter* RoguePrintWriter__print__Character( RogueObject* THIS, RogueCharacter value_0 )
{
  switch (THIS->type->index)
  {
    case 0:
      return (RogueClassPrintWriter*)RogueGlobal__print__Character( (RogueClassGlobal*)THIS, value_0 );
    case 3:
      return (RogueClassPrintWriter*)RogueStringBuilder__print__Character( (RogueStringBuilder*)THIS, value_0 );
    case 61:
      return (RogueClassPrintWriter*)RogueConsole__print__Character( (RogueClassConsole*)THIS, value_0 );
    case 63:
      return (RogueClassPrintWriter*)RogueConsoleErrorPrinter__print__Character( (RogueClassConsoleErrorPrinter*)THIS, value_0 );
    default:
      return 0;
  }
}

RogueClassPrintWriter* RoguePrintWriter__print__String( RogueObject* THIS, RogueString* value_0 )
{
  switch (THIS->type->index)
  {
    case 0:
      return (RogueClassPrintWriter*)RogueGlobal__print__String( (RogueClassGlobal*)THIS, value_0 );
    case 3:
      return (RogueClassPrintWriter*)RogueStringBuilder__print__String( (RogueStringBuilder*)THIS, value_0 );
    case 61:
      return (RogueClassPrintWriter*)RogueConsole__print__String( (RogueClassConsole*)THIS, value_0 );
    case 63:
      return (RogueClassPrintWriter*)RogueConsoleErrorPrinter__print__String( (RogueClassConsoleErrorPrinter*)THIS, value_0 );
    default:
      return 0;
  }
}

RogueClassPrintWriter* RoguePrintWriter__print__StringBuilder( RogueObject* THIS, RogueStringBuilder* buffer_0 )
{
  switch (THIS->type->index)
  {
    case 0:
      return (RogueClassPrintWriter*)RogueGlobal__print__StringBuilder( (RogueClassGlobal*)THIS, buffer_0 );
    case 3:
      return (RogueClassPrintWriter*)RogueStringBuilder__print__StringBuilder( (RogueStringBuilder*)THIS, buffer_0 );
    case 61:
      return (RogueClassPrintWriter*)RogueConsole__print__StringBuilder( (RogueClassConsole*)THIS, buffer_0 );
    case 63:
      return (RogueClassPrintWriter*)RogueConsoleErrorPrinter__print__StringBuilder( (RogueClassConsoleErrorPrinter*)THIS, buffer_0 );
    default:
      return 0;
  }
}

RogueStringBuilder* RogueStringBuilder__init_object( RogueStringBuilder* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  THIS->at_newline = true;
  return (RogueStringBuilder*)(THIS);
}

RogueStringBuilder* RogueStringBuilder__init( RogueStringBuilder* THIS )
{
  ROGUE_GC_CHECK;
  RogueStringBuilder__init__Int32( ROGUE_ARG(THIS), 40 );
  return (RogueStringBuilder*)(THIS);
}

RogueString* RogueStringBuilder__to_String( RogueStringBuilder* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)((RogueString__create__Byte_List_StringEncoding( ROGUE_ARG(THIS->utf8), RogueClassStringEncoding( 3 ) )));
}

RogueString* RogueStringBuilder__type_name( RogueStringBuilder* THIS )
{
  return (RogueString*)(Rogue_literal_strings[327]);
}

void RogueStringBuilder__flush( RogueStringBuilder* THIS )
{
  ROGUE_GC_CHECK;
}

RogueStringBuilder* RogueStringBuilder__print__Byte( RogueStringBuilder* THIS, RogueByte value_0 )
{
  ROGUE_GC_CHECK;
  return (RogueStringBuilder*)(((RogueStringBuilder*)(RogueStringBuilder__print__Int32( ROGUE_ARG(THIS), ROGUE_ARG(((RogueInt32)(value_0))) ))));
}

RogueStringBuilder* RogueStringBuilder__print__Character( RogueStringBuilder* THIS, RogueCharacter value_0 )
{
  ROGUE_GC_CHECK;
  return (RogueStringBuilder*)(((RogueStringBuilder*)(RogueStringBuilder__print__Character_Logical( ROGUE_ARG(THIS), value_0, true ))));
}

RogueStringBuilder* RogueStringBuilder__print__Int32( RogueStringBuilder* THIS, RogueInt32 value_0 )
{
  ROGUE_GC_CHECK;
  return (RogueStringBuilder*)(((RogueStringBuilder*)(RogueStringBuilder__print__Int64( ROGUE_ARG(THIS), ROGUE_ARG(((RogueInt64)(value_0))) ))));
}

RogueStringBuilder* RogueStringBuilder__print__Logical( RogueStringBuilder* THIS, RogueLogical value_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(value_0))
  {
    return (RogueStringBuilder*)(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(THIS), Rogue_literal_strings[130] ))));
  }
  else
  {
    return (RogueStringBuilder*)(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(THIS), Rogue_literal_strings[131] ))));
  }
}

RogueStringBuilder* RogueStringBuilder__print__Int64( RogueStringBuilder* THIS, RogueInt64 value_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((value_0) == (((1LL) << (63LL))))))
  {
    return (RogueStringBuilder*)(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(THIS), Rogue_literal_strings[22] ))));
  }
  else if (ROGUE_COND(((value_0) < (0LL))))
  {
    RogueStringBuilder__print__Character( ROGUE_ARG(THIS), (RogueCharacter)'-' );
    return (RogueStringBuilder*)(((RogueStringBuilder*)(RogueStringBuilder__print__Int64( ROGUE_ARG(THIS), ROGUE_ARG((-(value_0))) ))));
  }
  else if (ROGUE_COND(((value_0) >= (10LL))))
  {
    RogueStringBuilder__print__Int64( ROGUE_ARG(THIS), ROGUE_ARG(((value_0) / (10LL))) );
    return (RogueStringBuilder*)(((RogueStringBuilder*)(RogueStringBuilder__print__Character( ROGUE_ARG(THIS), ROGUE_ARG(((RogueCharacter)(((48LL) + ((RogueMath__mod__Int64_Int64( value_0, 10LL ))))))) ))));
  }
  else
  {
    return (RogueStringBuilder*)(((RogueStringBuilder*)(RogueStringBuilder__print__Character( ROGUE_ARG(THIS), ROGUE_ARG(((RogueCharacter)(((48LL) + (value_0))))) ))));
  }
}

RogueStringBuilder* RogueStringBuilder__print__Object( RogueStringBuilder* THIS, RogueObject* value_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!!(value_0)))
  {
    return (RogueStringBuilder*)(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(THIS), ROGUE_ARG(((RogueString*)Rogue_call_ROGUEM0( 8, value_0 ))) ))));
  }
  return (RogueStringBuilder*)(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(THIS), Rogue_literal_strings[6] ))));
}

RogueStringBuilder* RogueStringBuilder__print__Real64( RogueStringBuilder* THIS, RogueReal64 value_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((value_0) == (0.0))))
  {
    RogueStringBuilder__print__String( ROGUE_ARG(THIS), Rogue_literal_strings[296] );
    return (RogueStringBuilder*)(THIS);
  }
  else if (ROGUE_COND(((RogueReal64__is_infinite( value_0 )))))
  {
    if (ROGUE_COND(((value_0) < (0.0))))
    {
      RogueStringBuilder__print__String( ROGUE_ARG(THIS), Rogue_literal_strings[297] );
    }
    else
    {
      RogueStringBuilder__print__String( ROGUE_ARG(THIS), Rogue_literal_strings[298] );
    }
    return (RogueStringBuilder*)(THIS);
  }
  else if (ROGUE_COND(((RogueReal64__is_not_a_number( value_0 )))))
  {
    RogueStringBuilder__print__String( ROGUE_ARG(THIS), Rogue_literal_strings[299] );
    return (RogueStringBuilder*)(THIS);
  }
  if (ROGUE_COND(((value_0) < (0.0))))
  {
    RogueStringBuilder__print__Character( ROGUE_ARG(THIS), (RogueCharacter)'-' );
    value_0 = ((RogueReal64)((-(value_0))));
  }
  if (ROGUE_COND(((value_0) >= (1.0e15))))
  {
    RogueInt32 pow10_1 = (0);
    while (ROGUE_COND(((value_0) >= (10.0))))
    {
      ROGUE_GC_CHECK;
      value_0 /= 10.0;
      ++pow10_1;
    }
    return (RogueStringBuilder*)(((RogueStringBuilder*)(RogueStringBuilder__print__Int32( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__Character( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__Real64( ROGUE_ARG(THIS), value_0 )))), (RogueCharacter)'e' )))), pow10_1 ))));
  }
  if (ROGUE_COND(((value_0) < (0.00001))))
  {
    RogueInt32 pow10_2 = (0);
    while (ROGUE_COND(((value_0) < (0.1))))
    {
      ROGUE_GC_CHECK;
      value_0 *= 10.0;
      --pow10_2;
    }
    return (RogueStringBuilder*)(((RogueStringBuilder*)(RogueStringBuilder__print__Int32( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__Character( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__Real64( ROGUE_ARG(THIS), value_0 )))), (RogueCharacter)'e' )))), pow10_2 ))));
  }
  {
    RogueInt32 decimal_count_3 = (1);
    RogueInt32 _auto_6_4 = (18);
    for (;ROGUE_COND(((decimal_count_3) <= (_auto_6_4)));++decimal_count_3)
    {
      ROGUE_GC_CHECK;
      RogueStringBuilder__print_to_work_bytes__Real64_Int32( ROGUE_ARG(THIS), value_0, decimal_count_3 );
      if (ROGUE_COND(((((RogueStringBuilder__scan_work_bytes( ROGUE_ARG(THIS) )))) == (value_0))))
      {
        goto _auto_515;
      }
    }
  }
  _auto_515:;
  RogueStringBuilder__print_work_bytes( ROGUE_ARG(THIS) );
  return (RogueStringBuilder*)(THIS);
}

RogueStringBuilder* RogueStringBuilder__print__Real64_Int32( RogueStringBuilder* THIS, RogueReal64 value_0, RogueInt32 decimal_places_1 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((RogueReal64__is_infinite( value_0 )))))
  {
    if (ROGUE_COND(((value_0) < (0.0))))
    {
      RogueStringBuilder__print__String( ROGUE_ARG(THIS), Rogue_literal_strings[297] );
    }
    else
    {
      RogueStringBuilder__print__String( ROGUE_ARG(THIS), Rogue_literal_strings[298] );
    }
    return (RogueStringBuilder*)(THIS);
  }
  else if (ROGUE_COND(((RogueReal64__is_not_a_number( value_0 )))))
  {
    RogueStringBuilder__print__String( ROGUE_ARG(THIS), Rogue_literal_strings[299] );
    return (RogueStringBuilder*)(THIS);
  }
  if (ROGUE_COND(((value_0) < (0.0))))
  {
    RogueStringBuilder__print__Character( ROGUE_ARG(THIS), (RogueCharacter)'-' );
    value_0 = ((RogueReal64)((-(value_0))));
  }
  RogueStringBuilder__print_to_work_bytes__Real64_Int32( ROGUE_ARG(THIS), value_0, decimal_places_1 );
  RogueStringBuilder__print_work_bytes( ROGUE_ARG(THIS) );
  return (RogueStringBuilder*)(THIS);
}

RogueStringBuilder* RogueStringBuilder__print__String( RogueStringBuilder* THIS, RogueString* value_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!!(value_0)))
  {
    if (ROGUE_COND(!!(THIS->indent)))
    {
      {
        ROGUE_DEF_LOCAL_REF(RogueString*,_auto_326_0,(value_0));
        RogueInt32 _auto_327_0 = (0);
        RogueInt32 _auto_328_0 = (((((RogueString__count( _auto_326_0 )))) - (1)));
        for (;ROGUE_COND(((_auto_327_0) <= (_auto_328_0)));++_auto_327_0)
        {
          ROGUE_GC_CHECK;
          RogueCharacter ch_0 = (((RogueString__get__Int32( _auto_326_0, _auto_327_0 ))));
          RogueStringBuilder__print__Character( ROGUE_ARG(THIS), ch_0 );
        }
      }
    }
    else
    {
      {
        RogueInt32 i_1 = (0);
        RogueInt32 _auto_7_2 = (((RogueString__byte_count( value_0 ))));
        for (;ROGUE_COND(((i_1) < (_auto_7_2)));++i_1)
        {
          ROGUE_GC_CHECK;
          RogueByte_List__add__Byte( ROGUE_ARG(THIS->utf8), ROGUE_ARG(((RogueString__byte__Int32( value_0, i_1 )))) );
        }
      }
      THIS->count += ((RogueString__count( value_0 )));
      if (ROGUE_COND(((!!(((RogueString__count( value_0 ))))) && (((((RogueString__last( value_0 )))) == ((RogueCharacter)10))))))
      {
        THIS->at_newline = true;
      }
    }
    return (RogueStringBuilder*)(THIS);
  }
  else
  {
    return (RogueStringBuilder*)(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(THIS), Rogue_literal_strings[6] ))));
  }
}

RogueStringBuilder* RogueStringBuilder__print__StringBuilder( RogueStringBuilder* THIS, RogueStringBuilder* value_0 )
{
  ROGUE_GC_CHECK;
  RogueStringBuilder__reserve__Int32( ROGUE_ARG(THIS), ROGUE_ARG(value_0->utf8->count) );
  {
    ROGUE_DEF_LOCAL_REF(RogueByte_List*,_auto_563_0,(value_0->utf8));
    RogueInt32 _auto_564_0 = (0);
    RogueInt32 _auto_565_0 = (((_auto_563_0->count) - (1)));
    for (;ROGUE_COND(((_auto_564_0) <= (_auto_565_0)));++_auto_564_0)
    {
      ROGUE_GC_CHECK;
      RogueByte _auto_8_0 = (_auto_563_0->data->as_bytes[_auto_564_0]);
      RogueStringBuilder__write__Byte( ROGUE_ARG(THIS), _auto_8_0 );
    }
  }
  return (RogueStringBuilder*)(THIS);
}

RogueStringBuilder* RogueStringBuilder__println( RogueStringBuilder* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueStringBuilder*)(((RogueStringBuilder*)(RogueStringBuilder__print__Character( ROGUE_ARG(THIS), (RogueCharacter)10 ))));
}

RogueStringBuilder* RogueStringBuilder__println__String( RogueStringBuilder* THIS, RogueString* value_0 )
{
  ROGUE_GC_CHECK;
  return (RogueStringBuilder*)(((RogueStringBuilder*)(RogueStringBuilder__print__Character( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(THIS), value_0 )))), (RogueCharacter)10 ))));
}

RogueStringBuilder* RogueStringBuilder__init__Int32( RogueStringBuilder* THIS, RogueInt32 initial_capacity_0 )
{
  ROGUE_GC_CHECK;
  THIS->utf8 = ((RogueByte_List*)(RogueByte_List__init__Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueByte_List*,ROGUE_CREATE_OBJECT(Byte_List))), initial_capacity_0 )));
  return (RogueStringBuilder*)(THIS);
}

RogueStringBuilder* RogueStringBuilder__clear( RogueStringBuilder* THIS )
{
  ROGUE_GC_CHECK;
  RogueByte_List__clear( ROGUE_ARG(THIS->utf8) );
  THIS->count = 0;
  THIS->at_newline = true;
  THIS->cursor_index = 0;
  THIS->cursor_offset = 0;
  return (RogueStringBuilder*)(THIS);
}

RogueLogical RogueStringBuilder__contains__Character( RogueStringBuilder* THIS, RogueCharacter ch_0 )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(((RogueStringBuilder__locate__Character( ROGUE_ARG(THIS), ch_0 ))).exists);
}

RogueCharacter RogueStringBuilder__get__Int32( RogueStringBuilder* THIS, RogueInt32 index_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((index_0) == (0))))
  {
    THIS->cursor_offset = 0;
    THIS->cursor_index = 0;
  }
  else if (ROGUE_COND(((index_0) == (((THIS->count) - (1))))))
  {
    THIS->cursor_offset = THIS->utf8->count;
    THIS->cursor_index = THIS->count;
  }
  while (ROGUE_COND(((THIS->cursor_index) < (index_0))))
  {
    ROGUE_GC_CHECK;
    ++THIS->cursor_offset;
    while (ROGUE_COND(((((((RogueInt32)(THIS->utf8->data->as_bytes[THIS->cursor_offset]))) & (192))) == (128))))
    {
      ROGUE_GC_CHECK;
      ++THIS->cursor_offset;
    }
    ++THIS->cursor_index;
  }
  while (ROGUE_COND(((THIS->cursor_index) > (index_0))))
  {
    ROGUE_GC_CHECK;
    --THIS->cursor_offset;
    while (ROGUE_COND(((((((RogueInt32)(THIS->utf8->data->as_bytes[THIS->cursor_offset]))) & (192))) == (128))))
    {
      ROGUE_GC_CHECK;
      --THIS->cursor_offset;
    }
    --THIS->cursor_index;
  }
  RogueByte ch_1 = (THIS->utf8->data->as_bytes[THIS->cursor_offset]);
  if (ROGUE_COND(!!(((((RogueInt32)(ch_1))) & (128)))))
  {
    if (ROGUE_COND(!!(((((RogueInt32)(ch_1))) & (32)))))
    {
      if (ROGUE_COND(!!(((((RogueInt32)(ch_1))) & (16)))))
      {
        return (RogueCharacter)(((RogueCharacter)(((((((((((((RogueInt32)(ch_1))) & (7))) << (18))) | (((((((RogueInt32)(THIS->utf8->data->as_bytes[((THIS->cursor_offset) + (1))]))) & (63))) << (12))))) | (((((((RogueInt32)(THIS->utf8->data->as_bytes[((THIS->cursor_offset) + (2))]))) & (63))) << (6))))) | (((((RogueInt32)(THIS->utf8->data->as_bytes[((THIS->cursor_offset) + (3))]))) & (63)))))));
      }
      else
      {
        return (RogueCharacter)(((RogueCharacter)(((((((((((RogueInt32)(ch_1))) & (15))) << (12))) | (((((((RogueInt32)(THIS->utf8->data->as_bytes[((THIS->cursor_offset) + (1))]))) & (63))) << (6))))) | (((((RogueInt32)(THIS->utf8->data->as_bytes[((THIS->cursor_offset) + (2))]))) & (63)))))));
      }
    }
    else
    {
      return (RogueCharacter)(((RogueCharacter)(((((((((RogueInt32)(ch_1))) & (31))) << (6))) | (((((RogueInt32)(THIS->utf8->data->as_bytes[((THIS->cursor_offset) + (1))]))) & (63)))))));
    }
  }
  else
  {
    return (RogueCharacter)(((RogueCharacter)(ch_1)));
  }
}

RogueOptionalInt32 RogueStringBuilder__locate__Character( RogueStringBuilder* THIS, RogueCharacter ch_0 )
{
  ROGUE_GC_CHECK;
  {
    ROGUE_DEF_LOCAL_REF(RogueStringBuilder*,_auto_536_0,(THIS));
    RogueInt32 i_0 = (0);
    RogueInt32 _auto_537_0 = (((_auto_536_0->count) - (1)));
    for (;ROGUE_COND(((i_0) <= (_auto_537_0)));++i_0)
    {
      ROGUE_GC_CHECK;
      if (ROGUE_COND(((((RogueStringBuilder__get__Int32( ROGUE_ARG(THIS), i_0 )))) == (ch_0))))
      {
        return (RogueOptionalInt32)(RogueOptionalInt32( i_0, true ));
      }
    }
  }
  return (RogueOptionalInt32)((RogueOptionalInt32__create()));
}

RogueLogical RogueStringBuilder__needs_indent( RogueStringBuilder* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(((THIS->at_newline) && (((THIS->indent) > (0)))));
}

RogueLogical RogueStringBuilder__operatorEQUALSEQUALS__String( RogueStringBuilder* THIS, RogueString* value_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((void*)value_0) == ((void*)NULL)))
  {
    return (RogueLogical)(false);
  }
  if (ROGUE_COND(((THIS->count) != (((RogueString__count( value_0 )))))))
  {
    return (RogueLogical)(false);
  }
  {
    ROGUE_DEF_LOCAL_REF(RogueStringBuilder*,_auto_607_0,(THIS));
    RogueInt32 i_0 = (0);
    RogueInt32 _auto_608_0 = (((_auto_607_0->count) - (1)));
    for (;ROGUE_COND(((i_0) <= (_auto_608_0)));++i_0)
    {
      ROGUE_GC_CHECK;
      if (ROGUE_COND(((((RogueStringBuilder__get__Int32( ROGUE_ARG(THIS), i_0 )))) != (((RogueString__get__Int32( value_0, i_0 )))))))
      {
        return (RogueLogical)(false);
      }
    }
  }
  return (RogueLogical)(true);
}

RogueStringBuilder* RogueStringBuilder__print__Character_Logical( RogueStringBuilder* THIS, RogueCharacter value_0, RogueLogical formatted_1 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(formatted_1))
  {
    if (ROGUE_COND(((value_0) == ((RogueCharacter)10))))
    {
      THIS->at_newline = true;
    }
    else if (ROGUE_COND(((RogueStringBuilder__needs_indent( ROGUE_ARG(THIS) )))))
    {
      RogueStringBuilder__print_indent( ROGUE_ARG(THIS) );
    }
  }
  ++THIS->count;
  if (ROGUE_COND(((((RogueInt32)(value_0))) < (0))))
  {
    RogueByte_List__add__Byte( ROGUE_ARG(THIS->utf8), 0 );
  }
  else if (ROGUE_COND(((value_0) <= ((RogueCharacter__create__Int32( 127 ))))))
  {
    RogueByte_List__add__Byte( ROGUE_ARG(THIS->utf8), ROGUE_ARG(((RogueByte)(value_0))) );
  }
  else if (ROGUE_COND(((value_0) <= ((RogueCharacter__create__Int32( 2047 ))))))
  {
    ([&]()->RogueByte_List*{
      ROGUE_DEF_LOCAL_REF(RogueByte_List*,_auto_2_0,(THIS->utf8));
      RogueByte_List__add__Byte( _auto_2_0, ROGUE_ARG(((RogueByte)(((192) | (((((RogueInt32)(value_0))) >> (6))))))) );
      RogueByte_List__add__Byte( _auto_2_0, ROGUE_ARG(((RogueByte)(((128) | (((((RogueInt32)(value_0))) & (63))))))) );
       return _auto_2_0;
    }());
  }
  else if (ROGUE_COND(((value_0) <= ((RogueCharacter__create__Int32( 65535 ))))))
  {
    ([&]()->RogueByte_List*{
      ROGUE_DEF_LOCAL_REF(RogueByte_List*,_auto_3_0,(THIS->utf8));
      RogueByte_List__add__Byte( _auto_3_0, ROGUE_ARG(((RogueByte)(((224) | (((((RogueInt32)(value_0))) >> (12))))))) );
      RogueByte_List__add__Byte( _auto_3_0, ROGUE_ARG(((RogueByte)(((128) | (((((((RogueInt32)(value_0))) >> (6))) & (63))))))) );
      RogueByte_List__add__Byte( _auto_3_0, ROGUE_ARG(((RogueByte)(((128) | (((((RogueInt32)(value_0))) & (63))))))) );
       return _auto_3_0;
    }());
  }
  else if (ROGUE_COND(((value_0) <= ((RogueCharacter__create__Int32( 1114111 ))))))
  {
    ([&]()->RogueByte_List*{
      ROGUE_DEF_LOCAL_REF(RogueByte_List*,_auto_4_0,(THIS->utf8));
      RogueByte_List__add__Byte( _auto_4_0, ROGUE_ARG(((RogueByte)(((240) | (((((RogueInt32)(value_0))) >> (18))))))) );
      RogueByte_List__add__Byte( _auto_4_0, ROGUE_ARG(((RogueByte)(((128) | (((((((RogueInt32)(value_0))) >> (12))) & (63))))))) );
       return _auto_4_0;
    }());
    ([&]()->RogueByte_List*{
      ROGUE_DEF_LOCAL_REF(RogueByte_List*,_auto_5_0,(THIS->utf8));
      RogueByte_List__add__Byte( _auto_5_0, ROGUE_ARG(((RogueByte)(((128) | (((((((RogueInt32)(value_0))) >> (6))) & (63))))))) );
      RogueByte_List__add__Byte( _auto_5_0, ROGUE_ARG(((RogueByte)(((128) | (((((RogueInt32)(value_0))) & (63))))))) );
       return _auto_5_0;
    }());
  }
  else
  {
    RogueByte_List__add__Byte( ROGUE_ARG(THIS->utf8), 63 );
  }
  return (RogueStringBuilder*)(THIS);
}

void RogueStringBuilder__print_indent( RogueStringBuilder* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((!(((RogueStringBuilder__needs_indent( ROGUE_ARG(THIS) ))))) || (((THIS->indent) == (0))))))
  {
    return;
  }
  {
    RogueInt32 i_0 = (1);
    RogueInt32 _auto_9_1 = (THIS->indent);
    for (;ROGUE_COND(((i_0) <= (_auto_9_1)));++i_0)
    {
      ROGUE_GC_CHECK;
      RogueByte_List__add__Byte( ROGUE_ARG(THIS->utf8), 32 );
    }
  }
  THIS->count += THIS->indent;
  THIS->at_newline = false;
}

RogueStringBuilder* RogueStringBuilder__print_to_work_bytes__Real64_Int32( RogueStringBuilder* THIS, RogueReal64 value_0, RogueInt32 decimal_places_1 )
{
  ROGUE_GC_CHECK;
  RogueByte_List__clear( ROGUE_ARG(RogueStringBuilder_work_bytes) );
  RogueReal64 whole_2 = (floor((double)value_0));
  value_0 -= whole_2;
  while (ROGUE_COND(((whole_2) >= (10.0))))
  {
    ROGUE_GC_CHECK;
    RogueByte_List__add__Byte( ROGUE_ARG(RogueStringBuilder_work_bytes), ROGUE_ARG(((RogueByte)(((RogueCharacter)(((48) + (((RogueInt32)((RogueMath__mod__Real64_Real64( whole_2, 10.0 ))))))))))) );
    whole_2 /= 10;
  }
  RogueByte_List__add__Byte( ROGUE_ARG(RogueStringBuilder_work_bytes), ROGUE_ARG(((RogueByte)(((RogueCharacter)(((48) + (((RogueInt32)((RogueMath__mod__Real64_Real64( whole_2, 10.0 ))))))))))) );
  RogueByte_List__reverse( ROGUE_ARG(RogueStringBuilder_work_bytes) );
  if (ROGUE_COND(((decimal_places_1) != (0))))
  {
    RogueByte_List__add__Byte( ROGUE_ARG(RogueStringBuilder_work_bytes), 46 );
    {
      RogueInt32 _auto_10_4 = (1);
      RogueInt32 _auto_11_5 = (decimal_places_1);
      for (;ROGUE_COND(((_auto_10_4) <= (_auto_11_5)));++_auto_10_4)
      {
        ROGUE_GC_CHECK;
        value_0 *= 10;
        RogueInt32 digit_3 = (((RogueInt32)(floor((double)value_0))));
        value_0 -= digit_3;
        RogueByte_List__add__Byte( ROGUE_ARG(RogueStringBuilder_work_bytes), ROGUE_ARG(((RogueByte)(((RogueCharacter)(((48) + (digit_3))))))) );
      }
    }
  }
  if (ROGUE_COND(((value_0) >= (0.5))))
  {
    RogueByte_List__add__Byte( ROGUE_ARG(RogueStringBuilder_work_bytes), 53 );
    RogueStringBuilder__round_off_work_bytes( ROGUE_ARG(THIS) );
  }
  return (RogueStringBuilder*)(THIS);
}

void RogueStringBuilder__print_work_bytes( RogueStringBuilder* THIS )
{
  ROGUE_GC_CHECK;
  {
    ROGUE_DEF_LOCAL_REF(RogueByte_List*,_auto_516_0,(RogueStringBuilder_work_bytes));
    RogueInt32 _auto_517_0 = (0);
    RogueInt32 _auto_518_0 = (((_auto_516_0->count) - (1)));
    for (;ROGUE_COND(((_auto_517_0) <= (_auto_518_0)));++_auto_517_0)
    {
      ROGUE_GC_CHECK;
      RogueByte digit_0 = (_auto_516_0->data->as_bytes[_auto_517_0]);
      RogueStringBuilder__print__Character( ROGUE_ARG(THIS), ROGUE_ARG(((RogueCharacter)(digit_0))) );
    }
  }
}

RogueStringBuilder* RogueStringBuilder__reserve__Int32( RogueStringBuilder* THIS, RogueInt32 additional_bytes_0 )
{
  ROGUE_GC_CHECK;
  RogueByte_List__reserve__Int32( ROGUE_ARG(THIS->utf8), additional_bytes_0 );
  return (RogueStringBuilder*)(THIS);
}

void RogueStringBuilder__round_off_work_bytes( RogueStringBuilder* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((((RogueCharacter)(((RogueByte_List__remove_last( ROGUE_ARG(RogueStringBuilder_work_bytes) )))))) >= ((RogueCharacter)'5'))))
  {
    RogueInt32 i_0 = (((RogueStringBuilder_work_bytes->count) - (1)));
    while (ROGUE_COND(((i_0) >= (0))))
    {
      ROGUE_GC_CHECK;
      if (ROGUE_COND(((((RogueCharacter)(RogueStringBuilder_work_bytes->data->as_bytes[i_0]))) != ((RogueCharacter)'.'))))
      {
        RogueStringBuilder_work_bytes->data->as_bytes[i_0] = (((RogueByte)(((((RogueInt32)(RogueStringBuilder_work_bytes->data->as_bytes[i_0]))) + (1)))));
        if (ROGUE_COND(((((RogueInt32)(RogueStringBuilder_work_bytes->data->as_bytes[i_0]))) == (58))))
        {
          RogueByte_List__set__Int32_Byte( ROGUE_ARG(RogueStringBuilder_work_bytes), i_0, 48 );
        }
        else
        {
          return;
        }
      }
      --i_0;
    }
    RogueByte_List__insert__Byte_Int32( ROGUE_ARG(RogueStringBuilder_work_bytes), 49, 0 );
  }
}

RogueReal64 RogueStringBuilder__scan_work_bytes( RogueStringBuilder* THIS )
{
  ROGUE_GC_CHECK;
  RogueReal64 whole_0 = (0.0);
  RogueReal64 decimal_1 = (0.0);
  RogueInt32 decimal_count_2 = (0);
  RogueLogical scanning_whole_3 = (true);
  {
    ROGUE_DEF_LOCAL_REF(RogueByte_List*,_auto_512_0,(RogueStringBuilder_work_bytes));
    RogueInt32 _auto_513_0 = (0);
    RogueInt32 _auto_514_0 = (((_auto_512_0->count) - (1)));
    for (;ROGUE_COND(((_auto_513_0) <= (_auto_514_0)));++_auto_513_0)
    {
      ROGUE_GC_CHECK;
      RogueByte digit_0 = (_auto_512_0->data->as_bytes[_auto_513_0]);
      if (ROGUE_COND(scanning_whole_3))
      {
        if (ROGUE_COND(((((RogueCharacter)(digit_0))) == ((RogueCharacter)'.'))))
        {
          scanning_whole_3 = ((RogueLogical)(false));
        }
        else
        {
          whole_0 = ((RogueReal64)(((((whole_0) * (10.0))) + (((RogueReal64)(((((RogueCharacter)(digit_0))) - ((RogueCharacter)'0'))))))));
        }
      }
      else
      {
        decimal_1 = ((RogueReal64)(((((decimal_1) * (10.0))) + (((RogueReal64)(((((RogueCharacter)(digit_0))) - ((RogueCharacter)'0'))))))));
        ++decimal_count_2;
      }
    }
  }
  return (RogueReal64)(((whole_0) + (((decimal_1) / (((RogueReal64) pow((double)10.0, (double)((RogueReal64)(decimal_count_2)))))))));
}

RogueInt64 RogueStringBuilder__to_Int64( RogueStringBuilder* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((RogueStringBuilder__contains__Character( ROGUE_ARG(THIS), (RogueCharacter)',' )))))
  {
    {
      ROGUE_DEF_LOCAL_REF(RogueException*,_auto_539_0,0);
      ROGUE_DEF_LOCAL_REF(RogueClassStringBuilderPool*,_auto_538_0,(RogueStringBuilder_pool));
      RogueInt64 _auto_540_0 = 0;
      ROGUE_DEF_LOCAL_REF(RogueStringBuilder*,builder_0,(((RogueStringBuilder*)(RogueStringBuilderPool__on_use( _auto_538_0 )))));
      Rogue_ignore_unused(builder_0);

      ROGUE_TRY
      {
        {
          ROGUE_DEF_LOCAL_REF(RogueStringBuilder*,_auto_542_0,(THIS));
          RogueInt32 _auto_543_0 = (0);
          RogueInt32 _auto_544_0 = (((_auto_542_0->count) - (1)));
          for (;ROGUE_COND(((_auto_543_0) <= (_auto_544_0)));++_auto_543_0)
          {
            ROGUE_GC_CHECK;
            RogueCharacter ch_0 = (((RogueStringBuilder__get__Int32( _auto_542_0, _auto_543_0 ))));
            if (ROGUE_COND(((ch_0) != ((RogueCharacter)','))))
            {
              RogueStringBuilder__print__Character( builder_0, ch_0 );
            }
          }
        }
        {
          _auto_540_0 = ((RogueInt64)(((RogueStringBuilder__to_Int64( builder_0 )))));
          goto _auto_545;
        }
      }
      ROGUE_CATCH( RogueException,_auto_541_0 )
      {
        _auto_539_0 = ((RogueException*)(_auto_541_0));
      }
      ROGUE_END_TRY
      _auto_545:;
      RogueStringBuilderPool__on_end_use__StringBuilder( _auto_538_0, builder_0 );
      if (ROGUE_COND(!!(_auto_539_0)))
      {
        throw ((RogueException*)Rogue_call_ROGUEM0( 16, _auto_539_0 ));
      }
      return (RogueInt64)(_auto_540_0);
    }
  }
  RogueByte_List__reserve__Int32( ROGUE_ARG(THIS->utf8), 1 );
  RogueByte_List__set__Int32_Byte( ROGUE_ARG(THIS->utf8), ROGUE_ARG(THIS->utf8->count), 0 );
  return (RogueInt64)(((RogueInt64)((RogueInt64)strtoll( (char*)THIS->utf8->data->as_bytes, 0, 10 ))));
}

RogueReal64 RogueStringBuilder__to_Real64( RogueStringBuilder* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((RogueStringBuilder__contains__Character( ROGUE_ARG(THIS), (RogueCharacter)',' )))))
  {
    {
      ROGUE_DEF_LOCAL_REF(RogueException*,_auto_555_0,0);
      ROGUE_DEF_LOCAL_REF(RogueClassStringBuilderPool*,_auto_554_0,(RogueStringBuilder_pool));
      RogueReal64 _auto_556_0 = 0;
      ROGUE_DEF_LOCAL_REF(RogueStringBuilder*,builder_0,(((RogueStringBuilder*)(RogueStringBuilderPool__on_use( _auto_554_0 )))));
      Rogue_ignore_unused(builder_0);

      ROGUE_TRY
      {
        {
          ROGUE_DEF_LOCAL_REF(RogueStringBuilder*,_auto_558_0,(THIS));
          RogueInt32 _auto_559_0 = (0);
          RogueInt32 _auto_560_0 = (((_auto_558_0->count) - (1)));
          for (;ROGUE_COND(((_auto_559_0) <= (_auto_560_0)));++_auto_559_0)
          {
            ROGUE_GC_CHECK;
            RogueCharacter ch_0 = (((RogueStringBuilder__get__Int32( _auto_558_0, _auto_559_0 ))));
            if (ROGUE_COND(((ch_0) != ((RogueCharacter)','))))
            {
              RogueStringBuilder__print__Character( builder_0, ch_0 );
            }
          }
        }
        {
          _auto_556_0 = ((RogueReal64)(((RogueStringBuilder__to_Real64( builder_0 )))));
          goto _auto_561;
        }
      }
      ROGUE_CATCH( RogueException,_auto_557_0 )
      {
        _auto_555_0 = ((RogueException*)(_auto_557_0));
      }
      ROGUE_END_TRY
      _auto_561:;
      RogueStringBuilderPool__on_end_use__StringBuilder( _auto_554_0, builder_0 );
      if (ROGUE_COND(!!(_auto_555_0)))
      {
        throw ((RogueException*)Rogue_call_ROGUEM0( 16, _auto_555_0 ));
      }
      return (RogueReal64)(_auto_556_0);
    }
  }
  RogueByte_List__reserve__Int32( ROGUE_ARG(THIS->utf8), 1 );
  RogueByte_List__set__Int32_Byte( ROGUE_ARG(THIS->utf8), ROGUE_ARG(THIS->utf8->count), 0 );
  return (RogueReal64)(((RogueReal64)(strtod( (char*)THIS->utf8->data->as_bytes, 0 ))));
}

void RogueStringBuilder__write__Byte( RogueStringBuilder* THIS, RogueByte byte_0 )
{
  ROGUE_GC_CHECK;
  RogueByte_List__add__Byte( ROGUE_ARG(THIS->utf8), byte_0 );
  if (ROGUE_COND(((((((RogueInt32)(byte_0))) & (192))) != (128))))
  {
    ++THIS->count;
  }
}

RogueByte_List* RogueByte_List__init_object( RogueByte_List* THIS )
{
  ROGUE_GC_CHECK;
  RogueGenericList__init_object( ROGUE_ARG(((RogueClassGenericList*)THIS)) );
  return (RogueByte_List*)(THIS);
}

RogueByte_List* RogueByte_List__init( RogueByte_List* THIS )
{
  ROGUE_GC_CHECK;
  RogueByte_List__init__Int32( ROGUE_ARG(THIS), 0 );
  return (RogueByte_List*)(THIS);
}

RogueString* RogueByte_List__to_String( RogueByte_List* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[293] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(((RogueString*)(RogueByte_List__join__String( ROGUE_ARG(THIS), Rogue_literal_strings[15] )))) ))) )))), Rogue_literal_strings[294] )))) ))));
}

RogueString* RogueByte_List__type_name( RogueByte_List* THIS )
{
  return (RogueString*)(Rogue_literal_strings[376]);
}

RogueByte_List* RogueByte_List__init__Int32( RogueByte_List* THIS, RogueInt32 initial_capacity_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((initial_capacity_0) != (((RogueByte_List__capacity( ROGUE_ARG(THIS) )))))))
  {
    THIS->data = RogueType_create_array( initial_capacity_0, sizeof(RogueByte), false, 6 );
  }
  RogueByte_List__clear( ROGUE_ARG(THIS) );
  return (RogueByte_List*)(THIS);
}

void RogueByte_List__add__Byte( RogueByte_List* THIS, RogueByte value_0 )
{
  ROGUE_GC_CHECK;
  RogueByte_List__set__Int32_Byte( ROGUE_ARG(((RogueByte_List*)(RogueByte_List__reserve__Int32( ROGUE_ARG(THIS), 1 )))), ROGUE_ARG(THIS->count), value_0 );
  THIS->count = ((THIS->count) + (1));
}

RogueInt32 RogueByte_List__capacity( RogueByte_List* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(!!(THIS->data))))
  {
    return (RogueInt32)(0);
  }
  return (RogueInt32)(THIS->data->count);
}

void RogueByte_List__clear( RogueByte_List* THIS )
{
  ROGUE_GC_CHECK;
  RogueByte_List__discard_from__Int32( ROGUE_ARG(THIS), 0 );
}

void RogueByte_List__discard__Int32_Int32( RogueByte_List* THIS, RogueInt32 i1_0, RogueInt32 n_1 )
{
  ROGUE_GC_CHECK;
  RogueInt32 i2_2 = (((i1_0) + (n_1)));
  RogueArray_set(THIS->data,i1_0,((RogueArray*)(THIS->data)),i2_2,((THIS->count) - (i2_2)));
  RogueByte_List__discard_from__Int32( ROGUE_ARG(THIS), ROGUE_ARG(((THIS->count) - (n_1))) );
}

void RogueByte_List__discard_from__Int32( RogueByte_List* THIS, RogueInt32 index_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!!(THIS->data)))
  {
    RogueInt32 n_1 = (((THIS->count) - (index_0)));
    if (ROGUE_COND(((n_1) > (0))))
    {
      RogueArray__zero__Int32_Int32( ROGUE_ARG(((RogueArray*)THIS->data)), index_0, n_1 );
      THIS->count = index_0;
    }
  }
}

void RogueByte_List__ensure_capacity__Int32( RogueByte_List* THIS, RogueInt32 desired_capacity_0 )
{
  ROGUE_GC_CHECK;
  RogueByte_List__reserve__Int32( ROGUE_ARG(THIS), ROGUE_ARG(((desired_capacity_0) - (THIS->count))) );
}

void RogueByte_List__expand__Int32( RogueByte_List* THIS, RogueInt32 additional_count_0 )
{
  ROGUE_GC_CHECK;
  RogueByte_List__reserve__Int32( ROGUE_ARG(THIS), additional_count_0 );
  THIS->count += additional_count_0;
}

void RogueByte_List__expand_to_count__Int32( RogueByte_List* THIS, RogueInt32 minimum_count_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((THIS->count) < (minimum_count_0))))
  {
    RogueByte_List__ensure_capacity__Int32( ROGUE_ARG(THIS), minimum_count_0 );
    THIS->count = minimum_count_0;
  }
}

void RogueByte_List__expand_to_include__Int32( RogueByte_List* THIS, RogueInt32 index_0 )
{
  ROGUE_GC_CHECK;
  RogueByte_List__expand_to_count__Int32( ROGUE_ARG(THIS), ROGUE_ARG(((index_0) + (1))) );
}

RogueByte RogueByte_List__first( RogueByte_List* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueByte)(THIS->data->as_bytes[0]);
}

RogueByte RogueByte_List__get__Int32( RogueByte_List* THIS, RogueInt32 index_0 )
{
  return (RogueByte)(THIS->data->as_bytes[index_0]);
}

void RogueByte_List__insert__Byte_Int32( RogueByte_List* THIS, RogueByte value_0, RogueInt32 before_index_1 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((before_index_1) < (0))))
  {
    before_index_1 = ((RogueInt32)(0));
  }
  if (ROGUE_COND(((before_index_1) >= (THIS->count))))
  {
    RogueByte_List__add__Byte( ROGUE_ARG(THIS), value_0 );
    return;
  }
  else
  {
    RogueByte_List__reserve__Int32( ROGUE_ARG(THIS), 1 );
    RogueByte_List__shift__Int32_OptionalInt32_Int32_OptionalByte( ROGUE_ARG(THIS), before_index_1, (RogueOptionalInt32__create()), 1, (RogueOptionalByte__create()) );
    THIS->data->as_bytes[before_index_1] = (value_0);
  }
}

RogueString* RogueByte_List__join__String( RogueByte_List* THIS, RogueString* separator_0 )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueStringBuilder*,builder_1,(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))));
  RogueLogical first_2 = (true);
  {
    ROGUE_DEF_LOCAL_REF(RogueByte_List*,_auto_1106_0,(THIS));
    RogueInt32 _auto_1107_0 = (0);
    RogueInt32 _auto_1108_0 = (((_auto_1106_0->count) - (1)));
    for (;ROGUE_COND(((_auto_1107_0) <= (_auto_1108_0)));++_auto_1107_0)
    {
      ROGUE_GC_CHECK;
      RogueByte item_0 = (_auto_1106_0->data->as_bytes[_auto_1107_0]);
      if (ROGUE_COND(first_2))
      {
        first_2 = ((RogueLogical)(false));
      }
      else
      {
        RogueStringBuilder__print__String( builder_1, separator_0 );
      }
      RogueStringBuilder__print__Byte( builder_1, item_0 );
    }
  }
  return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( builder_1 ))));
}

RogueByte_List* RogueByte_List__reserve__Int32( RogueByte_List* THIS, RogueInt32 additional_elements_0 )
{
  ROGUE_GC_CHECK;
  RogueInt32 required_capacity_1 = (((THIS->count) + (additional_elements_0)));
  if (ROGUE_COND(((required_capacity_1) == (0))))
  {
    return (RogueByte_List*)(THIS);
  }
  if (ROGUE_COND(!(!!(THIS->data))))
  {
    if (ROGUE_COND(((required_capacity_1) == (1))))
    {
      required_capacity_1 = ((RogueInt32)(10));
    }
    THIS->data = RogueType_create_array( required_capacity_1, sizeof(RogueByte), false, 6 );
  }
  else if (ROGUE_COND(((required_capacity_1) > (THIS->data->count))))
  {
    RogueInt32 cap_2 = (((RogueByte_List__capacity( ROGUE_ARG(THIS) ))));
    if (ROGUE_COND(((required_capacity_1) < (((cap_2) + (cap_2))))))
    {
      required_capacity_1 = ((RogueInt32)(((cap_2) + (cap_2))));
    }
    ROGUE_DEF_LOCAL_REF(RogueArray*,new_data_3,(RogueType_create_array( required_capacity_1, sizeof(RogueByte), false, 6 )));
    RogueArray_set(new_data_3,0,((RogueArray*)(THIS->data)),0,-1);
    THIS->data = new_data_3;
  }
  return (RogueByte_List*)(THIS);
}

RogueByte RogueByte_List__remove_at__Int32( RogueByte_List* THIS, RogueInt32 index_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((RogueLogical)((((unsigned int)index_0) >= (unsigned int)THIS->count)))))
  {
    throw ((RogueException*)(RogueOutOfBoundsError___throw( ROGUE_ARG(((RogueClassOutOfBoundsError*)(RogueOutOfBoundsError__init__Int32_Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassOutOfBoundsError*,ROGUE_CREATE_OBJECT(OutOfBoundsError))), index_0, ROGUE_ARG(THIS->count) )))) )));
  }
  RogueByte result_1 = (THIS->data->as_bytes[index_0]);
  RogueArray_set(THIS->data,index_0,((RogueArray*)(THIS->data)),((index_0) + (1)),-1);
  RogueByte zero_value_2 = 0;
  THIS->count = ((THIS->count) + (-1));
  THIS->data->as_bytes[THIS->count] = (zero_value_2);
  return (RogueByte)(result_1);
}

RogueByte RogueByte_List__remove_first( RogueByte_List* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueByte)(((RogueByte_List__remove_at__Int32( ROGUE_ARG(THIS), 0 ))));
}

RogueByte RogueByte_List__remove_last( RogueByte_List* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueByte)(((RogueByte_List__remove_at__Int32( ROGUE_ARG(THIS), ROGUE_ARG(((THIS->count) - (1))) ))));
}

void RogueByte_List__reverse( RogueByte_List* THIS )
{
  ROGUE_GC_CHECK;
  RogueByte_List__reverse__Int32_Int32( ROGUE_ARG(THIS), 0, ROGUE_ARG(((THIS->count) - (1))) );
}

void RogueByte_List__reverse__Int32_Int32( RogueByte_List* THIS, RogueInt32 i1_0, RogueInt32 i2_1 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((i1_0) < (0))))
  {
    i1_0 = ((RogueInt32)(0));
  }
  if (ROGUE_COND(((i2_1) >= (THIS->count))))
  {
    i2_1 = ((RogueInt32)(((THIS->count) - (1))));
  }
  ROGUE_DEF_LOCAL_REF(RogueArray*,_data_2,(THIS->data));
  while (ROGUE_COND(((i1_0) < (i2_1))))
  {
    ROGUE_GC_CHECK;
    RogueByte temp_3 = (_data_2->as_bytes[i1_0]);
    _data_2->as_bytes[i1_0] = (_data_2->as_bytes[i2_1]);
    _data_2->as_bytes[i2_1] = (temp_3);
    ++i1_0;
    --i2_1;
  }
}

void RogueByte_List__set__Int32_Byte( RogueByte_List* THIS, RogueInt32 index_0, RogueByte new_value_1 )
{
  ROGUE_GC_CHECK;
  THIS->data->as_bytes[index_0] = (new_value_1);
}

void RogueByte_List__shift__Int32_OptionalInt32_Int32_OptionalByte( RogueByte_List* THIS, RogueInt32 i1_0, RogueOptionalInt32 element_count_1, RogueInt32 delta_2, RogueOptionalByte fill_3 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((delta_2) == (0))))
  {
    return;
  }
  RogueInt32 n_4 = 0;
  if (ROGUE_COND(element_count_1.exists))
  {
    n_4 = ((RogueInt32)(element_count_1.value));
  }
  else
  {
    n_4 = ((RogueInt32)(((THIS->count) - (i1_0))));
  }
  RogueInt32 dest_i2_5 = (((((((i1_0) + (delta_2))) + (n_4))) - (1)));
  RogueByte_List__expand_to_include__Int32( ROGUE_ARG(THIS), dest_i2_5 );
  RogueArray_set(THIS->data,((i1_0) + (delta_2)),((RogueArray*)(THIS->data)),i1_0,n_4);
  if (ROGUE_COND(fill_3.exists))
  {
    RogueByte value_6 = (fill_3.value);
    if (ROGUE_COND(((delta_2) > (0))))
    {
      {
        RogueInt32 i_7 = (i1_0);
        RogueInt32 _auto_39_8 = (((((i1_0) + (delta_2))) - (1)));
        for (;ROGUE_COND(((i_7) <= (_auto_39_8)));++i_7)
        {
          ROGUE_GC_CHECK;
          THIS->data->as_bytes[i_7] = (value_6);
        }
      }
    }
    else
    {
      {
        RogueInt32 i_9 = (((((i1_0) + (delta_2))) + (n_4)));
        RogueInt32 _auto_40_10 = (((((i1_0) + (n_4))) - (1)));
        for (;ROGUE_COND(((i_9) <= (_auto_40_10)));++i_9)
        {
          ROGUE_GC_CHECK;
          THIS->data->as_bytes[i_9] = (value_6);
        }
      }
    }
  }
}

RogueLogical RogueByte_List__is_valid_utf8( RogueByte_List* THIS )
{
  ROGUE_GC_CHECK;
  int n = THIS->count;
  unsigned char* src = THIS->data->as_bytes - 1;
  while (--n >= 0)
  {
    int b = *(++src);
    if (b & 0x80)
    {
      // 1xxx_xxxx
      if (b & 0x40)
      {
        // 11xx_xxxx
        if (b & 0x20)
        {
          // 111x_xxxx
          if (b & 0x10)
          {
            // 1111_xxxx
            if (b & 8)
            {
              // 1111_1xxx is illegal
              return false;
            }
            else
            {
              // 1111_0xxx
              if (n < 2) return false;
              if ((*(++src) & 0xC0) != 0x80) return false;
              if ((*(++src) & 0xC0) != 0x80) return false;
              if ((*(++src) & 0xC0) != 0x80) return false;
              n -= 3;
            }
          }
          else
          {
            // 1110_xxxx
            if (n < 1) return false;
            if ((*(++src) & 0xC0) != 0x80) return false;
            if ((*(++src) & 0xC0) != 0x80) return false;
            n -= 2;
          }
        }
        else
        {
          // 110x_xxxx
          if (--n < 0) return false;
          if ((*(++src) & 0xC0) != 0x80) return false;
        }
      }
      else
      {
        // 10xx_xxxx is an illegal first byte of UTF8
        return false;
      }
    //else 0xxx_xxxx is fine
    }
  }
  return true;

}

RogueClassGenericList* RogueGenericList__init_object( RogueClassGenericList* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  return (RogueClassGenericList*)(THIS);
}

RogueString* RogueGenericList__type_name( RogueClassGenericList* THIS )
{
  return (RogueString*)(Rogue_literal_strings[328]);
}

RogueString* RogueArray_Byte___type_name( RogueArray* THIS )
{
  return (RogueString*)(Rogue_literal_strings[384]);
}

RogueString* RogueArray__type_name( RogueArray* THIS )
{
  return (RogueString*)(Rogue_literal_strings[329]);
}

RogueInt32 RogueArray__count( RogueArray* THIS )
{
  return (RogueInt32)(((RogueInt32)(THIS->count)));
}

RogueInt32 RogueArray__element_size( RogueArray* THIS )
{
  return (RogueInt32)(((RogueInt32)(THIS->element_size)));
}

RogueArray* RogueArray__set__Int32_Array_Int32_Int32( RogueArray* THIS, RogueInt32 i1_0, RogueArray* other_1, RogueInt32 other_i1_2, RogueInt32 copy_count_3 )
{
  return (RogueArray*)(((RogueArray*)(RogueArray_set(THIS,i1_0,other_1,other_i1_2,copy_count_3))));
}

void RogueArray__zero__Int32_Int32( RogueArray* THIS, RogueInt32 i1_0, RogueInt32 n_1 )
{
  ROGUE_GC_CHECK;
  RogueInt32 size_2 = (THIS->element_size);
  memset( THIS->as_bytes + i1_0*size_2, 0, n_1*size_2 );

}

RogueInt32 RogueInt32__clamped_low__Int32( RogueInt32 THIS, RogueInt32 low_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((THIS) < (low_0))))
  {
    return (RogueInt32)(low_0);
  }
  return (RogueInt32)(THIS);
}

RogueInt32 RogueInt32__or_larger__Int32( RogueInt32 THIS, RogueInt32 other_0 )
{
  ROGUE_GC_CHECK;
  return (RogueInt32)(((((((THIS) >= (other_0))))) ? (THIS) : other_0));
}

RogueInt32 RogueInt32__or_smaller__Int32( RogueInt32 THIS, RogueInt32 other_0 )
{
  ROGUE_GC_CHECK;
  return (RogueInt32)(((((((THIS) <= (other_0))))) ? (THIS) : other_0));
}

RogueString* RogueInt32__to_String( RogueInt32 THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((((THIS) >= (-1))) && (((THIS) <= (9))))))
  {
    switch (THIS)
    {
      case -1:
      {
        return (RogueString*)(Rogue_literal_strings[262]);
      }
      case 0:
      {
        return (RogueString*)(Rogue_literal_strings[263]);
      }
      case 1:
      {
        return (RogueString*)(Rogue_literal_strings[264]);
      }
      case 2:
      {
        return (RogueString*)(Rogue_literal_strings[265]);
      }
      case 3:
      {
        return (RogueString*)(Rogue_literal_strings[266]);
      }
      case 4:
      {
        return (RogueString*)(Rogue_literal_strings[267]);
      }
      case 5:
      {
        return (RogueString*)(Rogue_literal_strings[268]);
      }
      case 6:
      {
        return (RogueString*)(Rogue_literal_strings[269]);
      }
      case 7:
      {
        return (RogueString*)(Rogue_literal_strings[270]);
      }
      case 8:
      {
        return (RogueString*)(Rogue_literal_strings[271]);
      }
      case 9:
      {
        return (RogueString*)(Rogue_literal_strings[272]);
      }
    }
  }
  return (RogueString*)(((RogueString*)(RogueString__operatorPLUS__Int32( ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], Rogue_literal_strings[0] ))), ROGUE_ARG(THIS) ))));
}

RogueCharacter RogueInt32__to_digit__Logical( RogueInt32 THIS, RogueLogical base64_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(base64_0))
  {
    if (ROGUE_COND(((((THIS) >= (0))) && (((THIS) <= (25))))))
    {
      return (RogueCharacter)(((RogueCharacter)(((THIS) + (65)))));
    }
    if (ROGUE_COND(((((THIS) >= (26))) && (((THIS) <= (51))))))
    {
      return (RogueCharacter)(((RogueCharacter)(((((THIS) - (26))) + (97)))));
    }
    if (ROGUE_COND(((((THIS) >= (52))) && (((THIS) <= (61))))))
    {
      return (RogueCharacter)(((RogueCharacter)(((((THIS) - (52))) + (48)))));
    }
    if (ROGUE_COND(((THIS) == (62))))
    {
      return (RogueCharacter)((RogueCharacter)'+');
    }
    if (ROGUE_COND(((THIS) == (63))))
    {
      return (RogueCharacter)((RogueCharacter)'/');
    }
    return (RogueCharacter)((RogueCharacter)'=');
  }
  else
  {
    if (ROGUE_COND(((((THIS) >= (0))) && (((THIS) <= (9))))))
    {
      return (RogueCharacter)(((RogueCharacter)(((THIS) + (48)))));
    }
    if (ROGUE_COND(((((THIS) >= (10))) && (((THIS) <= (35))))))
    {
      return (RogueCharacter)(((RogueCharacter)(((((THIS) - (10))) + (65)))));
    }
    return (RogueCharacter)((RogueCharacter)'0');
  }
}

RogueInt32 RogueInt32__sign( RogueInt32 THIS )
{
  ROGUE_GC_CHECK;
  return (RogueInt32)(((((((THIS) > (0))))) ? (1) : ((((((THIS) < (0))))) ? (-1) : 0)));
}

RogueClassStringBuilderPool* RogueStringBuilderPool__init_object( RogueClassStringBuilderPool* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  THIS->available = ((RogueStringBuilder_List*)(RogueStringBuilder_List__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder_List*,ROGUE_CREATE_OBJECT(StringBuilder_List))) )));
  return (RogueClassStringBuilderPool*)(THIS);
}

RogueString* RogueStringBuilderPool__type_name( RogueClassStringBuilderPool* THIS )
{
  return (RogueString*)(Rogue_literal_strings[341]);
}

RogueStringBuilder* RogueStringBuilderPool__on_use( RogueClassStringBuilderPool* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((RogueStringBuilder_List__is_empty( ROGUE_ARG(THIS->available) )))))
  {
    return (RogueStringBuilder*)(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) ))));
  }
  return (RogueStringBuilder*)(((RogueStringBuilder*)(RogueStringBuilder_List__remove_last( ROGUE_ARG(THIS->available) ))));
}

void RogueStringBuilderPool__on_end_use__StringBuilder( RogueClassStringBuilderPool* THIS, RogueStringBuilder* builder_0 )
{
  ROGUE_GC_CHECK;
  RogueStringBuilder_List__add__StringBuilder( ROGUE_ARG(THIS->available), ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__clear( builder_0 )))) );
}

RogueStringBuilder_List* RogueStringBuilder_List__init_object( RogueStringBuilder_List* THIS )
{
  ROGUE_GC_CHECK;
  RogueGenericList__init_object( ROGUE_ARG(((RogueClassGenericList*)THIS)) );
  return (RogueStringBuilder_List*)(THIS);
}

RogueStringBuilder_List* RogueStringBuilder_List__init( RogueStringBuilder_List* THIS )
{
  ROGUE_GC_CHECK;
  RogueStringBuilder_List__init__Int32( ROGUE_ARG(THIS), 0 );
  return (RogueStringBuilder_List*)(THIS);
}

RogueString* RogueStringBuilder_List__to_String( RogueStringBuilder_List* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[293] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(((RogueString*)(RogueStringBuilder_List__join__String( ROGUE_ARG(THIS), Rogue_literal_strings[15] )))) ))) )))), Rogue_literal_strings[294] )))) ))));
}

RogueString* RogueStringBuilder_List__type_name( RogueStringBuilder_List* THIS )
{
  return (RogueString*)(Rogue_literal_strings[379]);
}

RogueStringBuilder_List* RogueStringBuilder_List__init__Int32( RogueStringBuilder_List* THIS, RogueInt32 initial_capacity_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((initial_capacity_0) != (((RogueStringBuilder_List__capacity( ROGUE_ARG(THIS) )))))))
  {
    THIS->data = RogueType_create_array( initial_capacity_0, sizeof(RogueStringBuilder*), true, 3 );
  }
  RogueStringBuilder_List__clear( ROGUE_ARG(THIS) );
  return (RogueStringBuilder_List*)(THIS);
}

void RogueStringBuilder_List__add__StringBuilder( RogueStringBuilder_List* THIS, RogueStringBuilder* value_0 )
{
  ROGUE_GC_CHECK;
  RogueStringBuilder_List__set__Int32_StringBuilder( ROGUE_ARG(((RogueStringBuilder_List*)(RogueStringBuilder_List__reserve__Int32( ROGUE_ARG(THIS), 1 )))), ROGUE_ARG(THIS->count), value_0 );
  THIS->count = ((THIS->count) + (1));
}

RogueInt32 RogueStringBuilder_List__capacity( RogueStringBuilder_List* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(!!(THIS->data))))
  {
    return (RogueInt32)(0);
  }
  return (RogueInt32)(THIS->data->count);
}

void RogueStringBuilder_List__clear( RogueStringBuilder_List* THIS )
{
  ROGUE_GC_CHECK;
  RogueStringBuilder_List__discard_from__Int32( ROGUE_ARG(THIS), 0 );
}

void RogueStringBuilder_List__discard_from__Int32( RogueStringBuilder_List* THIS, RogueInt32 index_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!!(THIS->data)))
  {
    RogueInt32 n_1 = (((THIS->count) - (index_0)));
    if (ROGUE_COND(((n_1) > (0))))
    {
      RogueArray__zero__Int32_Int32( ROGUE_ARG(((RogueArray*)THIS->data)), index_0, n_1 );
      THIS->count = index_0;
    }
  }
}

RogueStringBuilder* RogueStringBuilder_List__get__Int32( RogueStringBuilder_List* THIS, RogueInt32 index_0 )
{
  return (RogueStringBuilder*)(((RogueStringBuilder*)(THIS->data->as_objects[index_0])));
}

RogueLogical RogueStringBuilder_List__is_empty( RogueStringBuilder_List* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(((THIS->count) == (0)));
}

RogueString* RogueStringBuilder_List__join__String( RogueStringBuilder_List* THIS, RogueString* separator_0 )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueStringBuilder*,builder_1,(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))));
  RogueLogical first_2 = (true);
  {
    ROGUE_DEF_LOCAL_REF(RogueStringBuilder_List*,_auto_1206_0,(THIS));
    RogueInt32 _auto_1207_0 = (0);
    RogueInt32 _auto_1208_0 = (((_auto_1206_0->count) - (1)));
    for (;ROGUE_COND(((_auto_1207_0) <= (_auto_1208_0)));++_auto_1207_0)
    {
      ROGUE_GC_CHECK;
      ROGUE_DEF_LOCAL_REF(RogueStringBuilder*,item_0,(((RogueStringBuilder*)(_auto_1206_0->data->as_objects[_auto_1207_0]))));
      if (ROGUE_COND(first_2))
      {
        first_2 = ((RogueLogical)(false));
      }
      else
      {
        RogueStringBuilder__print__String( builder_1, separator_0 );
      }
      RogueStringBuilder__print__StringBuilder( builder_1, item_0 );
    }
  }
  return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( builder_1 ))));
}

RogueStringBuilder_List* RogueStringBuilder_List__reserve__Int32( RogueStringBuilder_List* THIS, RogueInt32 additional_elements_0 )
{
  ROGUE_GC_CHECK;
  RogueInt32 required_capacity_1 = (((THIS->count) + (additional_elements_0)));
  if (ROGUE_COND(((required_capacity_1) == (0))))
  {
    return (RogueStringBuilder_List*)(THIS);
  }
  if (ROGUE_COND(!(!!(THIS->data))))
  {
    if (ROGUE_COND(((required_capacity_1) == (1))))
    {
      required_capacity_1 = ((RogueInt32)(10));
    }
    THIS->data = RogueType_create_array( required_capacity_1, sizeof(RogueStringBuilder*), true, 3 );
  }
  else if (ROGUE_COND(((required_capacity_1) > (THIS->data->count))))
  {
    RogueInt32 cap_2 = (((RogueStringBuilder_List__capacity( ROGUE_ARG(THIS) ))));
    if (ROGUE_COND(((required_capacity_1) < (((cap_2) + (cap_2))))))
    {
      required_capacity_1 = ((RogueInt32)(((cap_2) + (cap_2))));
    }
    ROGUE_DEF_LOCAL_REF(RogueArray*,new_data_3,(RogueType_create_array( required_capacity_1, sizeof(RogueStringBuilder*), true, 3 )));
    RogueArray_set(new_data_3,0,((RogueArray*)(THIS->data)),0,-1);
    THIS->data = new_data_3;
  }
  return (RogueStringBuilder_List*)(THIS);
}

RogueStringBuilder* RogueStringBuilder_List__remove_at__Int32( RogueStringBuilder_List* THIS, RogueInt32 index_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((RogueLogical)((((unsigned int)index_0) >= (unsigned int)THIS->count)))))
  {
    throw ((RogueException*)(RogueOutOfBoundsError___throw( ROGUE_ARG(((RogueClassOutOfBoundsError*)(RogueOutOfBoundsError__init__Int32_Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassOutOfBoundsError*,ROGUE_CREATE_OBJECT(OutOfBoundsError))), index_0, ROGUE_ARG(THIS->count) )))) )));
  }
  ROGUE_DEF_LOCAL_REF(RogueStringBuilder*,result_1,(((RogueStringBuilder*)(THIS->data->as_objects[index_0]))));
  RogueArray_set(THIS->data,index_0,((RogueArray*)(THIS->data)),((index_0) + (1)),-1);
  ROGUE_DEF_LOCAL_REF(RogueStringBuilder*,zero_value_2,0);
  THIS->count = ((THIS->count) + (-1));
  THIS->data->as_objects[THIS->count] = zero_value_2;
  return (RogueStringBuilder*)(result_1);
}

RogueStringBuilder* RogueStringBuilder_List__remove_last( RogueStringBuilder_List* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueStringBuilder*)(((RogueStringBuilder*)(RogueStringBuilder_List__remove_at__Int32( ROGUE_ARG(THIS), ROGUE_ARG(((THIS->count) - (1))) ))));
}

void RogueStringBuilder_List__set__Int32_StringBuilder( RogueStringBuilder_List* THIS, RogueInt32 index_0, RogueStringBuilder* new_value_1 )
{
  ROGUE_GC_CHECK;
  THIS->data->as_objects[index_0] = new_value_1;
}

RogueString* RogueArray_StringBuilder___type_name( RogueArray* THIS )
{
  return (RogueString*)(Rogue_literal_strings[390]);
}

RogueString* RogueLogical__to_String( RogueLogical THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(RogueString__operatorPLUS__Logical( ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], Rogue_literal_strings[0] ))), ROGUE_ARG(THIS) ))));
}

Rogue_Function____List* Rogue_Function____List__init_object( Rogue_Function____List* THIS )
{
  ROGUE_GC_CHECK;
  RogueGenericList__init_object( ROGUE_ARG(((RogueClassGenericList*)THIS)) );
  return (Rogue_Function____List*)(THIS);
}

Rogue_Function____List* Rogue_Function____List__init( Rogue_Function____List* THIS )
{
  ROGUE_GC_CHECK;
  Rogue_Function____List__init__Int32( ROGUE_ARG(THIS), 0 );
  return (Rogue_Function____List*)(THIS);
}

RogueString* Rogue_Function____List__to_String( Rogue_Function____List* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[293] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(((RogueString*)(Rogue_Function____List__join__String( ROGUE_ARG(THIS), Rogue_literal_strings[15] )))) ))) )))), Rogue_literal_strings[294] )))) ))));
}

RogueString* Rogue_Function____List__type_name( Rogue_Function____List* THIS )
{
  return (RogueString*)(Rogue_literal_strings[380]);
}

Rogue_Function____List* Rogue_Function____List__init__Int32( Rogue_Function____List* THIS, RogueInt32 initial_capacity_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((initial_capacity_0) != (((Rogue_Function____List__capacity( ROGUE_ARG(THIS) )))))))
  {
    THIS->data = RogueType_create_array( initial_capacity_0, sizeof(RogueClass_Function___*), true, 15 );
  }
  Rogue_Function____List__clear( ROGUE_ARG(THIS) );
  return (Rogue_Function____List*)(THIS);
}

void Rogue_Function____List__add___Function___( Rogue_Function____List* THIS, RogueClass_Function___* value_0 )
{
  ROGUE_GC_CHECK;
  Rogue_Function____List__set__Int32__Function___( ROGUE_ARG(((Rogue_Function____List*)(Rogue_Function____List__reserve__Int32( ROGUE_ARG(THIS), 1 )))), ROGUE_ARG(THIS->count), value_0 );
  THIS->count = ((THIS->count) + (1));
}

RogueInt32 Rogue_Function____List__capacity( Rogue_Function____List* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(!!(THIS->data))))
  {
    return (RogueInt32)(0);
  }
  return (RogueInt32)(THIS->data->count);
}

void Rogue_Function____List__clear( Rogue_Function____List* THIS )
{
  ROGUE_GC_CHECK;
  Rogue_Function____List__discard_from__Int32( ROGUE_ARG(THIS), 0 );
}

void Rogue_Function____List__discard_from__Int32( Rogue_Function____List* THIS, RogueInt32 index_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!!(THIS->data)))
  {
    RogueInt32 n_1 = (((THIS->count) - (index_0)));
    if (ROGUE_COND(((n_1) > (0))))
    {
      RogueArray__zero__Int32_Int32( ROGUE_ARG(((RogueArray*)THIS->data)), index_0, n_1 );
      THIS->count = index_0;
    }
  }
}

RogueClass_Function___* Rogue_Function____List__get__Int32( Rogue_Function____List* THIS, RogueInt32 index_0 )
{
  return (RogueClass_Function___*)(((RogueClass_Function___*)(THIS->data->as_objects[index_0])));
}

RogueString* Rogue_Function____List__join__String( Rogue_Function____List* THIS, RogueString* separator_0 )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueStringBuilder*,builder_1,(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))));
  RogueLogical first_2 = (true);
  {
    ROGUE_DEF_LOCAL_REF(Rogue_Function____List*,_auto_1308_0,(THIS));
    RogueInt32 _auto_1309_0 = (0);
    RogueInt32 _auto_1310_0 = (((_auto_1308_0->count) - (1)));
    for (;ROGUE_COND(((_auto_1309_0) <= (_auto_1310_0)));++_auto_1309_0)
    {
      ROGUE_GC_CHECK;
      ROGUE_DEF_LOCAL_REF(RogueClass_Function___*,item_0,(((RogueClass_Function___*)(_auto_1308_0->data->as_objects[_auto_1309_0]))));
      if (ROGUE_COND(first_2))
      {
        first_2 = ((RogueLogical)(false));
      }
      else
      {
        RogueStringBuilder__print__String( builder_1, separator_0 );
      }
      RogueStringBuilder__print__Object( builder_1, ROGUE_ARG(((RogueObject*)(item_0))) );
    }
  }
  return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( builder_1 ))));
}

Rogue_Function____List* Rogue_Function____List__reserve__Int32( Rogue_Function____List* THIS, RogueInt32 additional_elements_0 )
{
  ROGUE_GC_CHECK;
  RogueInt32 required_capacity_1 = (((THIS->count) + (additional_elements_0)));
  if (ROGUE_COND(((required_capacity_1) == (0))))
  {
    return (Rogue_Function____List*)(THIS);
  }
  if (ROGUE_COND(!(!!(THIS->data))))
  {
    if (ROGUE_COND(((required_capacity_1) == (1))))
    {
      required_capacity_1 = ((RogueInt32)(10));
    }
    THIS->data = RogueType_create_array( required_capacity_1, sizeof(RogueClass_Function___*), true, 15 );
  }
  else if (ROGUE_COND(((required_capacity_1) > (THIS->data->count))))
  {
    RogueInt32 cap_2 = (((Rogue_Function____List__capacity( ROGUE_ARG(THIS) ))));
    if (ROGUE_COND(((required_capacity_1) < (((cap_2) + (cap_2))))))
    {
      required_capacity_1 = ((RogueInt32)(((cap_2) + (cap_2))));
    }
    ROGUE_DEF_LOCAL_REF(RogueArray*,new_data_3,(RogueType_create_array( required_capacity_1, sizeof(RogueClass_Function___*), true, 15 )));
    RogueArray_set(new_data_3,0,((RogueArray*)(THIS->data)),0,-1);
    THIS->data = new_data_3;
  }
  return (Rogue_Function____List*)(THIS);
}

void Rogue_Function____List__set__Int32__Function___( Rogue_Function____List* THIS, RogueInt32 index_0, RogueClass_Function___* new_value_1 )
{
  ROGUE_GC_CHECK;
  THIS->data->as_objects[index_0] = new_value_1;
}

RogueClass_Function___* Rogue_Function_____init_object( RogueClass_Function___* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  return (RogueClass_Function___*)(THIS);
}

RogueString* Rogue_Function_____type_name( RogueClass_Function___* THIS )
{
  return (RogueString*)(Rogue_literal_strings[352]);
}

void Rogue_Function_____call( RogueClass_Function___* THIS )
{
}

RogueString* RogueArray__Function______type_name( RogueArray* THIS )
{
  return (RogueString*)(Rogue_literal_strings[391]);
}

RogueInt32 RogueString__hash_code( RogueString* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueInt32)(((RogueInt32)(THIS->hash_code)));
}

RogueString* RogueString__to_String( RogueString* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(THIS);
}

RogueString* RogueString__type_name( RogueString* THIS )
{
  return (RogueString*)(Rogue_literal_strings[326]);
}

RogueString* RogueString__cloned( RogueString* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(RogueString_create_from_utf8( (THIS->utf8), THIS->byte_count ))));
}

RogueString* RogueString__after__Int32( RogueString* THIS, RogueInt32 index_0 )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(RogueString__from__Int32( ROGUE_ARG(THIS), ROGUE_ARG(((index_0) + (1))) ))));
}

RogueString* RogueString__after_any__String_Logical( RogueString* THIS, RogueString* st_0, RogueLogical ignore_case_1 )
{
  ROGUE_GC_CHECK;
  RogueOptionalInt32 i_2 = (((RogueString__locate_last__String_OptionalInt32_Logical( ROGUE_ARG(THIS), st_0, (RogueOptionalInt32__create()), ignore_case_1 ))));
  if (ROGUE_COND(i_2.exists))
  {
    return (RogueString*)(((RogueString*)(RogueString__from__Int32( ROGUE_ARG(THIS), ROGUE_ARG(((i_2.value) + (((RogueString__count( st_0 )))))) ))));
  }
  else
  {
    return (RogueString*)(THIS);
  }
}

RogueString* RogueString__after_first__Character_Logical( RogueString* THIS, RogueCharacter ch_0, RogueLogical ignore_case_1 )
{
  ROGUE_GC_CHECK;
  RogueOptionalInt32 i_2 = (((RogueString__locate__Character_OptionalInt32_Logical( ROGUE_ARG(THIS), ch_0, (RogueOptionalInt32__create()), ignore_case_1 ))));
  if (ROGUE_COND(i_2.exists))
  {
    return (RogueString*)(((RogueString*)(RogueString__from__Int32( ROGUE_ARG(THIS), ROGUE_ARG(((i_2.value) + (1))) ))));
  }
  else
  {
    return (RogueString*)(Rogue_literal_strings[0]);
  }
}

RogueString* RogueString__after_first__String_Logical( RogueString* THIS, RogueString* st_0, RogueLogical ignore_case_1 )
{
  ROGUE_GC_CHECK;
  RogueOptionalInt32 i_2 = (((RogueString__locate__String_OptionalInt32_Logical( ROGUE_ARG(THIS), st_0, (RogueOptionalInt32__create()), ignore_case_1 ))));
  if (ROGUE_COND(i_2.exists))
  {
    return (RogueString*)(((RogueString*)(RogueString__from__Int32( ROGUE_ARG(THIS), ROGUE_ARG(((i_2.value) + (((RogueString__count( st_0 )))))) ))));
  }
  else
  {
    return (RogueString*)(Rogue_literal_strings[0]);
  }
}

RogueString* RogueString__after_last__Character_Logical( RogueString* THIS, RogueCharacter ch_0, RogueLogical ignore_case_1 )
{
  ROGUE_GC_CHECK;
  RogueOptionalInt32 i_2 = (((RogueString__locate_last__Character_OptionalInt32_Logical( ROGUE_ARG(THIS), ch_0, (RogueOptionalInt32__create()), ignore_case_1 ))));
  if (ROGUE_COND(i_2.exists))
  {
    return (RogueString*)(((RogueString*)(RogueString__from__Int32( ROGUE_ARG(THIS), ROGUE_ARG(((i_2.value) + (1))) ))));
  }
  else
  {
    return (RogueString*)(Rogue_literal_strings[0]);
  }
}

RogueString* RogueString__after_last__String_Logical( RogueString* THIS, RogueString* st_0, RogueLogical ignore_case_1 )
{
  ROGUE_GC_CHECK;
  RogueOptionalInt32 i_2 = (((RogueString__locate_last__String_OptionalInt32_Logical( ROGUE_ARG(THIS), st_0, (RogueOptionalInt32__create()), ignore_case_1 ))));
  if (ROGUE_COND(i_2.exists))
  {
    return (RogueString*)(((RogueString*)(RogueString__from__Int32( ROGUE_ARG(THIS), ROGUE_ARG(((i_2.value) + (((RogueString__count( st_0 )))))) ))));
  }
  else
  {
    return (RogueString*)(Rogue_literal_strings[0]);
  }
}

RogueString* RogueString__before__Int32( RogueString* THIS, RogueInt32 index_0 )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(RogueString__from__Int32_Int32( ROGUE_ARG(THIS), 0, ROGUE_ARG(((index_0) - (1))) ))));
}

RogueString* RogueString__before_first__Character_Logical( RogueString* THIS, RogueCharacter ch_0, RogueLogical ignore_case_1 )
{
  ROGUE_GC_CHECK;
  RogueOptionalInt32 i_2 = (((RogueString__locate__Character_OptionalInt32_Logical( ROGUE_ARG(THIS), ch_0, (RogueOptionalInt32__create()), ignore_case_1 ))));
  if (ROGUE_COND(i_2.exists))
  {
    return (RogueString*)(((RogueString*)(RogueString__from__Int32_Int32( ROGUE_ARG(THIS), 0, ROGUE_ARG(((i_2.value) - (1))) ))));
  }
  else
  {
    return (RogueString*)(THIS);
  }
}

RogueString* RogueString__before_last__Character_Logical( RogueString* THIS, RogueCharacter ch_0, RogueLogical ignore_case_1 )
{
  ROGUE_GC_CHECK;
  RogueOptionalInt32 i_2 = (((RogueString__locate_last__Character_OptionalInt32_Logical( ROGUE_ARG(THIS), ch_0, (RogueOptionalInt32__create()), ignore_case_1 ))));
  if (ROGUE_COND(i_2.exists))
  {
    return (RogueString*)(((RogueString*)(RogueString__from__Int32_Int32( ROGUE_ARG(THIS), 0, ROGUE_ARG(((i_2.value) - (1))) ))));
  }
  else
  {
    return (RogueString*)(THIS);
  }
}

RogueString* RogueString__before_last__String_Logical( RogueString* THIS, RogueString* st_0, RogueLogical ignore_case_1 )
{
  ROGUE_GC_CHECK;
  RogueOptionalInt32 i_2 = (((RogueString__locate_last__String_OptionalInt32_Logical( ROGUE_ARG(THIS), st_0, (RogueOptionalInt32__create()), ignore_case_1 ))));
  if (ROGUE_COND(i_2.exists))
  {
    return (RogueString*)(((RogueString*)(RogueString__from__Int32_Int32( ROGUE_ARG(THIS), 0, ROGUE_ARG(((i_2.value) - (1))) ))));
  }
  else
  {
    return (RogueString*)(THIS);
  }
}

RogueString* RogueString__before_suffix__Character_Logical( RogueString* THIS, RogueCharacter ch_0, RogueLogical ignore_case_1 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((RogueString__ends_with__Character_Logical( ROGUE_ARG(THIS), ch_0, ignore_case_1 )))))
  {
    return (RogueString*)(((RogueString*)(RogueString__before_last__Character_Logical( ROGUE_ARG(THIS), ch_0, ignore_case_1 ))));
  }
  else
  {
    return (RogueString*)(THIS);
  }
}

RogueLogical RogueString__begins_with__Character_Logical( RogueString* THIS, RogueCharacter ch_0, RogueLogical ignore_case_1 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(ignore_case_1))
  {
    return (RogueLogical)(((!!(((RogueString__count( ROGUE_ARG(THIS) ))))) && (((((RogueCharacter__to_lowercase( ROGUE_ARG(((RogueString__get__Int32( ROGUE_ARG(THIS), 0 )))) )))) == (((RogueCharacter__to_lowercase( ch_0 ))))))));
  }
  else
  {
    return (RogueLogical)(((!!(((RogueString__count( ROGUE_ARG(THIS) ))))) && (((((RogueString__get__Int32( ROGUE_ARG(THIS), 0 )))) == (ch_0)))));
  }
}

RogueLogical RogueString__begins_with__String_Logical( RogueString* THIS, RogueString* other_0, RogueLogical ignore_case_1 )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(((((((RogueString__count( ROGUE_ARG(THIS) )))) >= (((RogueString__count( other_0 )))))) && (((RogueString__contains_at__String_Int32_Logical( ROGUE_ARG(THIS), other_0, 0, ignore_case_1 ))))));
}

RogueByte RogueString__byte__Int32( RogueString* THIS, RogueInt32 byte_index_0 )
{
  ROGUE_GC_CHECK;
  return (RogueByte)(((RogueByte)(THIS->utf8[ byte_index_0 ])));
}

RogueInt32 RogueString__byte_count( RogueString* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueInt32)(((RogueInt32)(THIS->byte_count)));
}

RogueInt32 RogueString__compare_to__String_Logical( RogueString* THIS, RogueString* other_0, RogueLogical ignore_case_1 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((void*)other_0) == ((void*)NULL)))
  {
    return (RogueInt32)(1);
  }
  if (ROGUE_COND(ignore_case_1))
  {
    if (THIS == other_0) return 0;

    RogueInt32 other_count = other_0->byte_count;
    RogueInt32 limit = THIS->byte_count;

    int result;
    if (limit == other_count)
    {
      // Strings are same length
      #ifdef ROGUE_PLATFORM_WINDOWS
        result = strnicmp( THIS->utf8, other_0->utf8, limit );
      #else
        result = strncasecmp( THIS->utf8, other_0->utf8, limit );
      #endif
      if (result == 0) return 0;
    }
    else
    {
      // Strings differ in length.  Compare the part that matches first.
      if (limit > other_count) limit = other_count;
      #ifdef ROGUE_PLATFORM_WINDOWS
        result = strnicmp( THIS->utf8, other_0->utf8, limit );
      #else
        result = strncasecmp( THIS->utf8, other_0->utf8, limit );
      #endif
      if (result == 0)
      {
        // Equal so far - the shorter string comes before the longer one.
        if (limit == other_count) return 1;
        return -1;
      }
    }
    if (result < 0) return -1;
    else            return 1;

  }
  else
  {
    return (RogueInt32)((RogueString__operatorLTGT__String_String( ROGUE_ARG(THIS), other_0 )));
  }
}

RogueString* RogueString__consolidated( RogueString* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(RogueStringConsolidationTable__get__String( ((RogueClassStringConsolidationTable*)ROGUE_SINGLETON(StringConsolidationTable)), ROGUE_ARG(THIS) ))));
}

RogueLogical RogueString__contains__Character_Logical( RogueString* THIS, RogueCharacter ch_0, RogueLogical ignore_case_1 )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(((RogueString__locate__Character_OptionalInt32_Logical( ROGUE_ARG(THIS), ch_0, (RogueOptionalInt32__create()), ignore_case_1 ))).exists);
}

RogueLogical RogueString__contains__String_Logical( RogueString* THIS, RogueString* substring_0, RogueLogical ignore_case_1 )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(((RogueString__locate__String_OptionalInt32_Logical( ROGUE_ARG(THIS), substring_0, (RogueOptionalInt32__create()), ignore_case_1 ))).exists);
}

RogueLogical RogueString__contains_at__String_Int32_Logical( RogueString* THIS, RogueString* substring_0, RogueInt32 at_index_1, RogueLogical ignore_case_2 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((at_index_1) < (0))))
  {
    return (RogueLogical)(false);
  }
  if (ROGUE_COND(ignore_case_2))
  {
    if (ROGUE_COND(((((at_index_1) + (((RogueString__count( substring_0 )))))) > (((RogueString__count( ROGUE_ARG(THIS) )))))))
    {
      return (RogueLogical)(false);
    }
    {
      ROGUE_DEF_LOCAL_REF(RogueString*,_auto_470_0,(substring_0));
      RogueInt32 _auto_471_0 = (0);
      RogueInt32 _auto_472_0 = (((((RogueString__count( _auto_470_0 )))) - (1)));
      for (;ROGUE_COND(((_auto_471_0) <= (_auto_472_0)));++_auto_471_0)
      {
        ROGUE_GC_CHECK;
        RogueCharacter other_ch_0 = (((RogueString__get__Int32( _auto_470_0, _auto_471_0 ))));
        if (ROGUE_COND(((((RogueCharacter__to_lowercase( other_ch_0 )))) != (((RogueCharacter__to_lowercase( ROGUE_ARG(((RogueString__get__Int32( ROGUE_ARG(THIS), at_index_1 )))) )))))))
        {
          return (RogueLogical)(false);
        }
        ++at_index_1;
      }
    }
    return (RogueLogical)(true);
  }
  else
  {
    RogueInt32 offset = RogueString_set_cursor( THIS, at_index_1 );
    RogueInt32 other_count = substring_0->byte_count;
    if (offset + other_count > THIS->byte_count) return false;

    return (0 == memcmp(THIS->utf8 + offset, substring_0->utf8, other_count));

  }
}

RogueLogical RogueString__contains_pattern__String_Logical( RogueString* THIS, RogueString* pattern_0, RogueLogical ignore_case_1 )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(((RogueString__locate_pattern__String_Int32_Logical( ROGUE_ARG(THIS), pattern_0, 0, ignore_case_1 ))).exists);
}

RogueInt32 RogueString__count( RogueString* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueInt32)(((RogueInt32)(THIS->character_count)));
}

RogueInt32 RogueString__count__Character( RogueString* THIS, RogueCharacter look_for_0 )
{
  ROGUE_GC_CHECK;
  RogueInt32 result_1 = (0);
  if (ROGUE_COND(((((RogueInt32)(look_for_0))) < (128))))
  {
    int n = THIS->byte_count;
    char* src = THIS->utf8 + n;
    while (--n >= 0)
    {
      if (*(--src) == look_for_0) ++result_1;
    }

    return (RogueInt32)(result_1);
  }
  else
  {
    {
      ROGUE_DEF_LOCAL_REF(RogueString*,_auto_385_0,(THIS));
      RogueInt32 _auto_386_0 = (0);
      RogueInt32 _auto_387_0 = (((((RogueString__count( _auto_385_0 )))) - (1)));
      for (;ROGUE_COND(((_auto_386_0) <= (_auto_387_0)));++_auto_386_0)
      {
        ROGUE_GC_CHECK;
        RogueCharacter ch_0 = (((RogueString__get__Int32( _auto_385_0, _auto_386_0 ))));
        if (ROGUE_COND(((ch_0) == (look_for_0))))
        {
          ++result_1;
        }
      }
    }
  }
  return (RogueInt32)(result_1);
}

RogueLogical RogueString__ends_with__Character_Logical( RogueString* THIS, RogueCharacter ch_0, RogueLogical ignore_case_1 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(ignore_case_1))
  {
    return (RogueLogical)(((((((RogueString__count( ROGUE_ARG(THIS) )))) > (0))) && (((((RogueCharacter__to_lowercase( ROGUE_ARG(((RogueString__get__Int32( ROGUE_ARG(THIS), ROGUE_ARG(((((RogueString__count( ROGUE_ARG(THIS) )))) - (1))) )))) )))) == (((RogueCharacter__to_lowercase( ch_0 ))))))));
  }
  else
  {
    return (RogueLogical)(((((((RogueString__count( ROGUE_ARG(THIS) )))) > (0))) && (((((RogueString__get__Int32( ROGUE_ARG(THIS), ROGUE_ARG(((((RogueString__count( ROGUE_ARG(THIS) )))) - (1))) )))) == (ch_0)))));
  }
}

RogueLogical RogueString__ends_with__String_Logical( RogueString* THIS, RogueString* other_0, RogueLogical ignore_case_1 )
{
  ROGUE_GC_CHECK;
  RogueInt32 other_count_2 = (((RogueString__count( other_0 ))));
  return (RogueLogical)(((((((((RogueString__count( ROGUE_ARG(THIS) )))) >= (other_count_2))) && (((other_count_2) > (0))))) && (((RogueString__contains_at__String_Int32_Logical( ROGUE_ARG(THIS), other_0, ROGUE_ARG(((((RogueString__count( ROGUE_ARG(THIS) )))) - (other_count_2))), ignore_case_1 ))))));
}

RogueLogical RogueString__equals__String_Logical( RogueString* THIS, RogueString* other_0, RogueLogical ignore_case_1 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(ignore_case_1))
  {
    if (ROGUE_COND(((((RogueString__count( ROGUE_ARG(THIS) )))) != (((RogueString__count( other_0 )))))))
    {
      return (RogueLogical)(false);
    }
    {
      ROGUE_DEF_LOCAL_REF(RogueString*,_auto_504_0,(THIS));
      RogueInt32 i_0 = (0);
      RogueInt32 _auto_505_0 = (((((RogueString__count( _auto_504_0 )))) - (1)));
      for (;ROGUE_COND(((i_0) <= (_auto_505_0)));++i_0)
      {
        ROGUE_GC_CHECK;
        RogueCharacter ch_0 = (((RogueString__get__Int32( _auto_504_0, i_0 ))));
        if (ROGUE_COND(((((RogueCharacter__to_lowercase( ch_0 )))) != (((RogueCharacter__to_lowercase( ROGUE_ARG(((RogueString__get__Int32( other_0, i_0 )))) )))))))
        {
          return (RogueLogical)(false);
        }
      }
    }
    return (RogueLogical)(true);
  }
  else
  {
    return (RogueLogical)((RogueString__operatorEQUALSEQUALS__String_String( ROGUE_ARG(THIS), other_0 )));
  }
}

RogueString* RogueString__extract_string__String_Logical( RogueString* THIS, RogueString* format_0, RogueLogical ignore_case_1 )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueString_List*,strings_2,(((RogueString_List*)(RogueString__extract_strings__String_Logical( ROGUE_ARG(THIS), format_0, ignore_case_1 )))));
  if (ROGUE_COND(((!(!!(strings_2))) || (((RogueString_List__is_empty( strings_2 )))))))
  {
    return (RogueString*)(((RogueString*)(NULL)));
  }
  return (RogueString*)(((RogueString*)(RogueString_List__first( strings_2 ))));
}

RogueString_List* RogueString__extract_strings__String_Logical( RogueString* THIS, RogueString* format_0, RogueLogical ignore_case_1 )
{
  ROGUE_GC_CHECK;
  return (RogueString_List*)(((RogueString_List*)(RogueString__extract_strings__String_String_List_Logical( ROGUE_ARG(THIS), format_0, ROGUE_ARG(((RogueString_List*)(RogueString_List__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueString_List*,ROGUE_CREATE_OBJECT(String_List))) )))), ignore_case_1 ))));
}

RogueString_List* RogueString__extract_strings__String_String_List_Logical( RogueString* THIS, RogueString* format_0, RogueString_List* results_1, RogueLogical ignore_case_2 )
{
  ROGUE_GC_CHECK;
  RogueInt32 i1_3 = (results_1->count);
  RogueOptionalSpan span_4 = (((RogueString__locate_pattern__String_Int32_Logical( ROGUE_ARG(THIS), format_0, 0, ignore_case_2 ))));
  if (ROGUE_COND(!(span_4.exists)))
  {
    return (RogueString_List*)(((RogueString_List*)(NULL)));
  }
  RogueInt32 i_5 = (span_4.value.index);
  RogueInt32 n_6 = (span_4.value.count);
  if (ROGUE_COND(((((i_5) > (0))) || (((n_6) < (((RogueString__count( ROGUE_ARG(THIS) )))))))))
  {
    return (RogueString_List*)(((RogueString_List*)(NULL)));
  }
  if (ROGUE_COND(!(((RogueString___extract_strings__Int32_Int32_String_Int32_Int32_String_List_Logical( ROGUE_ARG(((RogueString*)(RogueString__substring__Int32_Int32( ROGUE_ARG(THIS), i_5, n_6 )))), 0, n_6, format_0, 0, ROGUE_ARG(((RogueString__count( format_0 )))), results_1, ignore_case_2 ))))))
  {
    return (RogueString_List*)(((RogueString_List*)(NULL)));
  }
  RogueInt32 i2_7 = (((results_1->count) - (1)));
  while (ROGUE_COND(((i1_3) < (i2_7))))
  {
    ROGUE_GC_CHECK;
    RogueString_List__swap__Int32_Int32( results_1, i1_3, i2_7 );
    ++i1_3;
    --i2_7;
  }
  return (RogueString_List*)(results_1);
}

RogueLogical RogueString___extract_strings__Int32_Int32_String_Int32_Int32_String_List_Logical( RogueString* THIS, RogueInt32 i0_0, RogueInt32 remaining_count_1, RogueString* format_2, RogueInt32 f0_3, RogueInt32 fcount_4, RogueString_List* results_5, RogueLogical ignore_case_6 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((fcount_4) == (0))))
  {
    while (ROGUE_COND(((((!!(remaining_count_1)) && (((((((RogueString__get__Int32( ROGUE_ARG(THIS), i0_0 )))) == ((RogueCharacter)' '))) || (((((RogueString__get__Int32( ROGUE_ARG(THIS), i0_0 )))) == ((RogueCharacter)9))))))) && (!!(remaining_count_1)))))
    {
      ROGUE_GC_CHECK;
      ++i0_0;
      --remaining_count_1;
    }
    if (ROGUE_COND(((remaining_count_1) > (0))))
    {
      return (RogueLogical)(false);
    }
    return (RogueLogical)(true);
  }
  RogueCharacter format_ch_7 = (((RogueString__get__Int32( format_2, f0_3 ))));
  {
    {
      {
        switch (format_ch_7)
        {
          case (RogueCharacter)'*':
          {
            {
              RogueInt32 n_8 = (0);
              RogueInt32 _auto_123_9 = (remaining_count_1);
              for (;ROGUE_COND(((n_8) <= (_auto_123_9)));++n_8)
              {
                ROGUE_GC_CHECK;
                if (ROGUE_COND(((RogueString___extract_strings__Int32_Int32_String_Int32_Int32_String_List_Logical( ROGUE_ARG(THIS), ROGUE_ARG(((i0_0) + (n_8))), ROGUE_ARG(((remaining_count_1) - (n_8))), format_2, ROGUE_ARG(((f0_3) + (1))), ROGUE_ARG(((fcount_4) - (1))), results_5, ignore_case_6 )))))
                {
                  return (RogueLogical)(!!(results_5));
                }
              }
            }
            break;
          }
          case (RogueCharacter)' ':
          {
            RogueInt32 n_10 = (0);
            while (ROGUE_COND(((n_10) < (remaining_count_1))))
            {
              ROGUE_GC_CHECK;
              RogueCharacter ch_11 = (((RogueString__get__Int32( ROGUE_ARG(THIS), ROGUE_ARG(((i0_0) + (n_10))) ))));
              if (ROGUE_COND(((((ch_11) != ((RogueCharacter)' '))) && (((ch_11) != ((RogueCharacter)9))))))
              {
                goto _auto_659;
              }
              ++n_10;
            }
            _auto_659:;
            if (ROGUE_COND(((n_10) == (0))))
            {
              return (RogueLogical)(false);
            }
            return (RogueLogical)(((RogueString___extract_strings__Int32_Int32_String_Int32_Int32_String_List_Logical( ROGUE_ARG(THIS), ROGUE_ARG(((i0_0) + (n_10))), ROGUE_ARG(((remaining_count_1) - (n_10))), format_2, ROGUE_ARG(((f0_3) + (1))), ROGUE_ARG(((fcount_4) - (1))), results_5, ignore_case_6 ))));
          }
          case (RogueCharacter)'$':
          {
            while (ROGUE_COND(((!!(remaining_count_1)) && (((((((RogueString__get__Int32( ROGUE_ARG(THIS), i0_0 )))) == ((RogueCharacter)' '))) || (((((RogueString__get__Int32( ROGUE_ARG(THIS), i0_0 )))) == ((RogueCharacter)9))))))))
            {
              ROGUE_GC_CHECK;
              ++i0_0;
              --remaining_count_1;
            }
            RogueInt32 m_start_12 = (i0_0);
            if (ROGUE_COND(((((fcount_4) >= (3))) && (((((RogueString__get__Int32( format_2, ROGUE_ARG(((f0_3) + (1))) )))) == ((RogueCharacter)'('))))))
            {
              if (ROGUE_COND(((((RogueString__get__Int32( format_2, ROGUE_ARG(((f0_3) + (2))) )))) == ((RogueCharacter)')'))))
              {
                f0_3 += 2;
                fcount_4 -= 2;
              }
              else if (ROGUE_COND(((((fcount_4) >= (4))) && (((((RogueString__get__Int32( format_2, ROGUE_ARG(((f0_3) + (3))) )))) == ((RogueCharacter)')'))))))
              {
                switch (((RogueString__get__Int32( format_2, ROGUE_ARG(((f0_3) + (2))) ))))
                {
                  case (RogueCharacter)'I':
                  {
                    RogueInt32 i1_13 = (i0_0);
                    RogueInt32 rcount_14 = (remaining_count_1);
                    if (ROGUE_COND(((!!(rcount_14)) && (((((RogueString__get__Int32( ROGUE_ARG(THIS), i1_13 )))) == ((RogueCharacter)'-'))))))
                    {
                      ++i1_13;
                      --rcount_14;
                    }
                    RogueLogical found_digit_15 = (false);
                    while (ROGUE_COND(((!!(rcount_14)) && (((RogueCharacter__is_number__Int32( ROGUE_ARG(((RogueString__get__Int32( ROGUE_ARG(THIS), i1_13 )))), 10 )))))))
                    {
                      ROGUE_GC_CHECK;
                      ++i1_13;
                      --rcount_14;
                      found_digit_15 = ((RogueLogical)(true));
                    }
                    if (ROGUE_COND(found_digit_15))
                    {
                      if (ROGUE_COND(((RogueString___extract_strings__Int32_Int32_String_Int32_Int32_String_List_Logical( ROGUE_ARG(THIS), i1_13, rcount_14, format_2, ROGUE_ARG(((f0_3) + (4))), ROGUE_ARG(((fcount_4) - (4))), results_5, ignore_case_6 )))))
                      {
                        RogueString_List__add__String( results_5, ROGUE_ARG(((RogueString*)(RogueString__substring__Int32_Int32( ROGUE_ARG(THIS), i0_0, ROGUE_ARG(((i1_13) - (i0_0))) )))) );
                        return (RogueLogical)(true);
                      }
                    }
                    break;
                  }
                  case (RogueCharacter)'R':
                  {
                    RogueInt32 i1_16 = (i0_0);
                    RogueInt32 rcount_17 = (remaining_count_1);
                    if (ROGUE_COND(((!!(rcount_17)) && (((((RogueString__get__Int32( ROGUE_ARG(THIS), i1_16 )))) == ((RogueCharacter)'-'))))))
                    {
                      ++i1_16;
                      --rcount_17;
                    }
                    RogueLogical found_digit_18 = (false);
                    while (ROGUE_COND(((!!(rcount_17)) && (((RogueCharacter__is_number__Int32( ROGUE_ARG(((RogueString__get__Int32( ROGUE_ARG(THIS), i1_16 )))), 10 )))))))
                    {
                      ROGUE_GC_CHECK;
                      ++i1_16;
                      --rcount_17;
                      found_digit_18 = ((RogueLogical)(true));
                    }
                    if (ROGUE_COND(((!!(rcount_17)) && (((((RogueString__get__Int32( ROGUE_ARG(THIS), i1_16 )))) == ((RogueCharacter)'.'))))))
                    {
                      ++i1_16;
                      --rcount_17;
                    }
                    while (ROGUE_COND(((!!(rcount_17)) && (((RogueCharacter__is_number__Int32( ROGUE_ARG(((RogueString__get__Int32( ROGUE_ARG(THIS), i1_16 )))), 10 )))))))
                    {
                      ROGUE_GC_CHECK;
                      ++i1_16;
                      --rcount_17;
                      found_digit_18 = ((RogueLogical)(true));
                    }
                    if (ROGUE_COND(found_digit_18))
                    {
                      if (ROGUE_COND(((RogueString___extract_strings__Int32_Int32_String_Int32_Int32_String_List_Logical( ROGUE_ARG(THIS), i1_16, rcount_17, format_2, ROGUE_ARG(((f0_3) + (4))), ROGUE_ARG(((fcount_4) - (4))), results_5, ignore_case_6 )))))
                      {
                        RogueString_List__add__String( results_5, ROGUE_ARG(((RogueString*)(RogueString__substring__Int32_Int32( ROGUE_ARG(THIS), i0_0, ROGUE_ARG(((i1_16) - (i0_0))) )))) );
                        return (RogueLogical)(true);
                      }
                    }
                    break;
                  }
                  case (RogueCharacter)'#':
                  {
                    if (ROGUE_COND(((!!(remaining_count_1)) && (((RogueCharacter__is_number__Int32( ROGUE_ARG(((RogueString__get__Int32( ROGUE_ARG(THIS), i0_0 )))), 10 )))))))
                    {
                      if (ROGUE_COND(((RogueString___extract_strings__Int32_Int32_String_Int32_Int32_String_List_Logical( ROGUE_ARG(THIS), ROGUE_ARG(((i0_0) + (1))), ROGUE_ARG(((remaining_count_1) - (1))), format_2, ROGUE_ARG(((f0_3) + (4))), ROGUE_ARG(((fcount_4) - (4))), results_5, ignore_case_6 )))))
                      {
                        RogueString_List__add__String( results_5, ROGUE_ARG(((RogueString*)(RogueString__substring__Int32_Int32( ROGUE_ARG(THIS), m_start_12, 1 )))) );
                        return (RogueLogical)(true);
                      }
                    }
                    break;
                  }
                  case (RogueCharacter)'$':
                  {
                    if (ROGUE_COND(((!!(remaining_count_1)) && (((((RogueString__get__Int32( ROGUE_ARG(THIS), i0_0 )))) == ((RogueCharacter)'$'))))))
                    {
                      if (ROGUE_COND(((RogueString___extract_strings__Int32_Int32_String_Int32_Int32_String_List_Logical( ROGUE_ARG(THIS), ROGUE_ARG(((i0_0) + (1))), ROGUE_ARG(((remaining_count_1) - (1))), format_2, ROGUE_ARG(((f0_3) + (4))), ROGUE_ARG(((fcount_4) - (4))), results_5, ignore_case_6 )))))
                      {
                        return (RogueLogical)(true);
                      }
                    }
                    break;
                  }
                }
                return (RogueLogical)(false);
              }
            }
            {
              RogueInt32 n_19 = (1);
              RogueInt32 _auto_124_20 = (remaining_count_1);
              for (;ROGUE_COND(((n_19) <= (_auto_124_20)));++n_19)
              {
                ROGUE_GC_CHECK;
                if (ROGUE_COND(((RogueString___extract_strings__Int32_Int32_String_Int32_Int32_String_List_Logical( ROGUE_ARG(THIS), ROGUE_ARG(((i0_0) + (n_19))), ROGUE_ARG(((remaining_count_1) - (n_19))), format_2, ROGUE_ARG(((f0_3) + (1))), ROGUE_ARG(((fcount_4) - (1))), results_5, ignore_case_6 )))))
                {
                  RogueString_List__add__String( results_5, ROGUE_ARG(((RogueString*)(RogueString__substring__Int32_Int32( ROGUE_ARG(THIS), m_start_12, n_19 )))) );
                  return (RogueLogical)(true);
                }
              }
            }
            break;
          }
          case (RogueCharacter)'?':
          {
            if (ROGUE_COND(((remaining_count_1) == (0))))
            {
              return (RogueLogical)(false);
            }
            return (RogueLogical)(((RogueString___extract_strings__Int32_Int32_String_Int32_Int32_String_List_Logical( ROGUE_ARG(THIS), ROGUE_ARG(((i0_0) + (1))), ROGUE_ARG(((remaining_count_1) - (1))), format_2, ROGUE_ARG(((f0_3) + (1))), ROGUE_ARG(((fcount_4) - (1))), results_5, ignore_case_6 ))));
          }
          case (RogueCharacter)'\\':
          {
            if (ROGUE_COND(((remaining_count_1) == (0))))
            {
              return (RogueLogical)(false);
            }
            if (ROGUE_COND(((fcount_4) == (1))))
            {
              return (RogueLogical)(((RogueString___extract_strings__Int32_Int32_String_Int32_Int32_String_List_Logical( ROGUE_ARG(THIS), i0_0, remaining_count_1, format_2, ROGUE_ARG(((f0_3) + (1))), ROGUE_ARG(((fcount_4) - (1))), results_5, ignore_case_6 ))));
            }
            ++f0_3;
            --fcount_4;
            format_ch_7 = ((RogueCharacter)(((RogueString__get__Int32( format_2, f0_3 )))));
            if (ROGUE_COND(true)) goto _auto_660;
            break;
          }
          default:
          {
            if (ROGUE_COND(true)) goto _auto_660;
          }
        }
        goto _auto_658;
        }
      _auto_660:;
      {
        if (ROGUE_COND(((remaining_count_1) == (0))))
        {
          return (RogueLogical)(false);
        }
        RogueCharacter ch_21 = (((RogueString__get__Int32( ROGUE_ARG(THIS), i0_0 ))));
        if (ROGUE_COND(ignore_case_6))
        {
          ch_21 = ((RogueCharacter)(((RogueCharacter__to_lowercase( ch_21 )))));
          format_ch_7 = ((RogueCharacter)(((RogueCharacter__to_lowercase( format_ch_7 )))));
        }
        if (ROGUE_COND(((format_ch_7) == (ch_21))))
        {
          return (RogueLogical)(((RogueString___extract_strings__Int32_Int32_String_Int32_Int32_String_List_Logical( ROGUE_ARG(THIS), ROGUE_ARG(((i0_0) + (1))), ROGUE_ARG(((remaining_count_1) - (1))), format_2, ROGUE_ARG(((f0_3) + (1))), ROGUE_ARG(((fcount_4) - (1))), results_5, ignore_case_6 ))));
        }
        else if (ROGUE_COND(((((ch_21) == ((RogueCharacter)' '))) || (((ch_21) == ((RogueCharacter)9))))))
        {
          if (ROGUE_COND(((f0_3) > (0))))
          {
            RogueInt32 n_22 = (1);
            while (ROGUE_COND(((n_22) < (remaining_count_1))))
            {
              ROGUE_GC_CHECK;
              ch_21 = ((RogueCharacter)(((RogueString__get__Int32( ROGUE_ARG(THIS), ROGUE_ARG(((i0_0) + (n_22))) )))));
              if (ROGUE_COND(((((ch_21) != ((RogueCharacter)' '))) && (((ch_21) != ((RogueCharacter)9))))))
              {
                goto _auto_661;
              }
              ++n_22;
            }
            _auto_661:;
            if (ROGUE_COND(((((((((i0_0) == (0))) || (((n_22) == (remaining_count_1))))) || (!(((RogueCharacter__is_identifier__Logical_Logical( ROGUE_ARG(((RogueString__get__Int32( ROGUE_ARG(THIS), ROGUE_ARG(((i0_0) - (1))) )))), false, false ))))))) || (!(((RogueCharacter__is_identifier__Logical_Logical( ROGUE_ARG(((RogueString__get__Int32( ROGUE_ARG(THIS), ROGUE_ARG(((i0_0) + (n_22))) )))), false, false ))))))))
            {
              return (RogueLogical)(((RogueString___extract_strings__Int32_Int32_String_Int32_Int32_String_List_Logical( ROGUE_ARG(THIS), ROGUE_ARG(((i0_0) + (n_22))), ROGUE_ARG(((remaining_count_1) - (n_22))), format_2, f0_3, fcount_4, results_5, ignore_case_6 ))));
            }
          }
        }
        }
      goto _auto_658;
    }
  }
  _auto_658:;
  return (RogueLogical)(false);
}

RogueInt32 RogueString___pattern_match_count__Int32_Int32_String_Int32_Int32_Logical( RogueString* THIS, RogueInt32 i0_0, RogueInt32 remaining_count_1, RogueString* format_2, RogueInt32 f0_3, RogueInt32 fcount_4, RogueLogical ignore_case_5 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((fcount_4) == (0))))
  {
    return (RogueInt32)(i0_0);
  }
  RogueCharacter format_ch_6 = (((RogueString__get__Int32( format_2, f0_3 ))));
  {
    {
      {
        switch (format_ch_6)
        {
          case (RogueCharacter)'*':
          {
            if (ROGUE_COND(((fcount_4) == (1))))
            {
              RogueInt32 result_7 = (-1);
              {
                RogueInt32 n_9 = (0);
                RogueInt32 _auto_125_10 = (remaining_count_1);
                for (;ROGUE_COND(((n_9) <= (_auto_125_10)));++n_9)
                {
                  ROGUE_GC_CHECK;
                  RogueInt32 count_8 = (((RogueString___pattern_match_count__Int32_Int32_String_Int32_Int32_Logical( ROGUE_ARG(THIS), ROGUE_ARG(((i0_0) + (n_9))), ROGUE_ARG(((remaining_count_1) - (n_9))), format_2, ROGUE_ARG(((f0_3) + (1))), ROGUE_ARG(((fcount_4) - (1))), ignore_case_5 ))));
                  if (ROGUE_COND(((count_8) == (-1))))
                  {
                    return (RogueInt32)(result_7);
                  }
                  result_7 = ((RogueInt32)(count_8));
                }
              }
              return (RogueInt32)(result_7);
            }
            else
            {
              {
                RogueInt32 n_12 = (0);
                RogueInt32 _auto_126_13 = (remaining_count_1);
                for (;ROGUE_COND(((n_12) <= (_auto_126_13)));++n_12)
                {
                  ROGUE_GC_CHECK;
                  RogueInt32 count_11 = (((RogueString___pattern_match_count__Int32_Int32_String_Int32_Int32_Logical( ROGUE_ARG(THIS), ROGUE_ARG(((i0_0) + (n_12))), ROGUE_ARG(((remaining_count_1) - (n_12))), format_2, ROGUE_ARG(((f0_3) + (1))), ROGUE_ARG(((fcount_4) - (1))), ignore_case_5 ))));
                  if (ROGUE_COND(((count_11) != (-1))))
                  {
                    return (RogueInt32)(count_11);
                  }
                }
              }
              return (RogueInt32)(-1);
            }
            break;
          }
          case (RogueCharacter)' ':
          {
            RogueInt32 n_14 = (0);
            while (ROGUE_COND(((n_14) < (remaining_count_1))))
            {
              ROGUE_GC_CHECK;
              RogueCharacter ch_15 = (((RogueString__get__Int32( ROGUE_ARG(THIS), ROGUE_ARG(((i0_0) + (n_14))) ))));
              if (ROGUE_COND(((((ch_15) != ((RogueCharacter)' '))) && (((ch_15) != ((RogueCharacter)9))))))
              {
                goto _auto_639;
              }
              ++n_14;
            }
            _auto_639:;
            if (ROGUE_COND(((n_14) == (0))))
            {
              return (RogueInt32)(-1);
            }
            return (RogueInt32)(((RogueString___pattern_match_count__Int32_Int32_String_Int32_Int32_Logical( ROGUE_ARG(THIS), ROGUE_ARG(((i0_0) + (n_14))), ROGUE_ARG(((remaining_count_1) - (n_14))), format_2, ROGUE_ARG(((f0_3) + (1))), ROGUE_ARG(((fcount_4) - (1))), ignore_case_5 ))));
          }
          case (RogueCharacter)'$':
          {
            while (ROGUE_COND(((!!(remaining_count_1)) && (((((((RogueString__get__Int32( ROGUE_ARG(THIS), i0_0 )))) == ((RogueCharacter)' '))) || (((((RogueString__get__Int32( ROGUE_ARG(THIS), i0_0 )))) == ((RogueCharacter)9))))))))
            {
              ROGUE_GC_CHECK;
              ++i0_0;
              --remaining_count_1;
            }
            if (ROGUE_COND(((((fcount_4) >= (3))) && (((((RogueString__get__Int32( format_2, ROGUE_ARG(((f0_3) + (1))) )))) == ((RogueCharacter)'('))))))
            {
              if (ROGUE_COND(((((RogueString__get__Int32( format_2, ROGUE_ARG(((f0_3) + (2))) )))) == ((RogueCharacter)')'))))
              {
                f0_3 += 2;
                fcount_4 -= 2;
              }
              else if (ROGUE_COND(((((fcount_4) >= (4))) && (((((RogueString__get__Int32( format_2, ROGUE_ARG(((f0_3) + (3))) )))) == ((RogueCharacter)')'))))))
              {
                switch (((RogueString__get__Int32( format_2, ROGUE_ARG(((f0_3) + (2))) ))))
                {
                  case (RogueCharacter)'I':
                  {
                    RogueInt32 i1_16 = (i0_0);
                    RogueInt32 rcount_17 = (remaining_count_1);
                    if (ROGUE_COND(((!!(rcount_17)) && (((((RogueString__get__Int32( ROGUE_ARG(THIS), i1_16 )))) == ((RogueCharacter)'-'))))))
                    {
                      ++i1_16;
                      --rcount_17;
                    }
                    RogueLogical found_digit_18 = (false);
                    while (ROGUE_COND(((!!(rcount_17)) && (((RogueCharacter__is_number__Int32( ROGUE_ARG(((RogueString__get__Int32( ROGUE_ARG(THIS), i1_16 )))), 10 )))))))
                    {
                      ROGUE_GC_CHECK;
                      ++i1_16;
                      --rcount_17;
                      found_digit_18 = ((RogueLogical)(true));
                    }
                    if (ROGUE_COND(found_digit_18))
                    {
                      return (RogueInt32)(((RogueString___pattern_match_count__Int32_Int32_String_Int32_Int32_Logical( ROGUE_ARG(THIS), i1_16, rcount_17, format_2, ROGUE_ARG(((f0_3) + (4))), ROGUE_ARG(((fcount_4) - (4))), ignore_case_5 ))));
                    }
                    break;
                  }
                  case (RogueCharacter)'R':
                  {
                    RogueInt32 i1_19 = (i0_0);
                    RogueInt32 rcount_20 = (remaining_count_1);
                    if (ROGUE_COND(((!!(rcount_20)) && (((((RogueString__get__Int32( ROGUE_ARG(THIS), i1_19 )))) == ((RogueCharacter)'-'))))))
                    {
                      ++i1_19;
                      --rcount_20;
                    }
                    RogueLogical found_digit_21 = (false);
                    while (ROGUE_COND(((!!(rcount_20)) && (((RogueCharacter__is_number__Int32( ROGUE_ARG(((RogueString__get__Int32( ROGUE_ARG(THIS), i1_19 )))), 10 )))))))
                    {
                      ROGUE_GC_CHECK;
                      ++i1_19;
                      --rcount_20;
                      found_digit_21 = ((RogueLogical)(true));
                    }
                    if (ROGUE_COND(((!!(rcount_20)) && (((((RogueString__get__Int32( ROGUE_ARG(THIS), i1_19 )))) == ((RogueCharacter)'.'))))))
                    {
                      ++i1_19;
                      --rcount_20;
                    }
                    while (ROGUE_COND(((!!(rcount_20)) && (((RogueCharacter__is_number__Int32( ROGUE_ARG(((RogueString__get__Int32( ROGUE_ARG(THIS), i1_19 )))), 10 )))))))
                    {
                      ROGUE_GC_CHECK;
                      ++i1_19;
                      --rcount_20;
                      found_digit_21 = ((RogueLogical)(true));
                    }
                    if (ROGUE_COND(found_digit_21))
                    {
                      return (RogueInt32)(((RogueString___pattern_match_count__Int32_Int32_String_Int32_Int32_Logical( ROGUE_ARG(THIS), i1_19, rcount_20, format_2, ROGUE_ARG(((f0_3) + (4))), ROGUE_ARG(((fcount_4) - (4))), ignore_case_5 ))));
                    }
                    break;
                  }
                  case (RogueCharacter)'#':
                  {
                    if (ROGUE_COND(((!!(remaining_count_1)) && (((RogueCharacter__is_number__Int32( ROGUE_ARG(((RogueString__get__Int32( ROGUE_ARG(THIS), i0_0 )))), 10 )))))))
                    {
                      return (RogueInt32)(((RogueString___pattern_match_count__Int32_Int32_String_Int32_Int32_Logical( ROGUE_ARG(THIS), ROGUE_ARG(((i0_0) + (1))), ROGUE_ARG(((remaining_count_1) - (1))), format_2, ROGUE_ARG(((f0_3) + (4))), ROGUE_ARG(((fcount_4) - (4))), ignore_case_5 ))));
                    }
                    break;
                  }
                  case (RogueCharacter)'$':
                  {
                    if (ROGUE_COND(((!!(remaining_count_1)) && (((((RogueString__get__Int32( ROGUE_ARG(THIS), i0_0 )))) == ((RogueCharacter)'$'))))))
                    {
                      return (RogueInt32)(((RogueString___pattern_match_count__Int32_Int32_String_Int32_Int32_Logical( ROGUE_ARG(THIS), ROGUE_ARG(((i0_0) + (1))), ROGUE_ARG(((remaining_count_1) - (1))), format_2, ROGUE_ARG(((f0_3) + (4))), ROGUE_ARG(((fcount_4) - (4))), ignore_case_5 ))));
                    }
                    break;
                  }
                }
                return (RogueInt32)(-1);
              }
            }
            if (ROGUE_COND(((fcount_4) == (1))))
            {
              RogueInt32 result_22 = (-1);
              {
                RogueInt32 len_24 = (0);
                RogueInt32 _auto_127_25 = (remaining_count_1);
                for (;ROGUE_COND(((len_24) <= (_auto_127_25)));++len_24)
                {
                  ROGUE_GC_CHECK;
                  RogueInt32 n_23 = (((RogueString___pattern_match_count__Int32_Int32_String_Int32_Int32_Logical( ROGUE_ARG(THIS), ROGUE_ARG(((i0_0) + (len_24))), ROGUE_ARG(((remaining_count_1) - (len_24))), format_2, ROGUE_ARG(((f0_3) + (1))), ROGUE_ARG(((fcount_4) - (1))), ignore_case_5 ))));
                  if (ROGUE_COND(((n_23) == (-1))))
                  {
                    return (RogueInt32)(result_22);
                  }
                  result_22 = ((RogueInt32)(((i0_0) + (len_24))));
                }
              }
              return (RogueInt32)(result_22);
            }
            else
            {
              {
                RogueInt32 n_27 = (1);
                RogueInt32 _auto_128_28 = (remaining_count_1);
                for (;ROGUE_COND(((n_27) <= (_auto_128_28)));++n_27)
                {
                  ROGUE_GC_CHECK;
                  RogueInt32 count_26 = (((RogueString___pattern_match_count__Int32_Int32_String_Int32_Int32_Logical( ROGUE_ARG(THIS), ROGUE_ARG(((i0_0) + (n_27))), ROGUE_ARG(((remaining_count_1) - (n_27))), format_2, ROGUE_ARG(((f0_3) + (1))), ROGUE_ARG(((fcount_4) - (1))), ignore_case_5 ))));
                  if (ROGUE_COND(((count_26) != (-1))))
                  {
                    return (RogueInt32)(count_26);
                  }
                }
              }
              return (RogueInt32)(-1);
            }
            break;
          }
          case (RogueCharacter)'?':
          {
            if (ROGUE_COND(((remaining_count_1) == (0))))
            {
              return (RogueInt32)(-1);
            }
            return (RogueInt32)(((RogueString___pattern_match_count__Int32_Int32_String_Int32_Int32_Logical( ROGUE_ARG(THIS), ROGUE_ARG(((i0_0) + (1))), ROGUE_ARG(((remaining_count_1) - (1))), format_2, ROGUE_ARG(((f0_3) + (1))), ROGUE_ARG(((fcount_4) - (1))), ignore_case_5 ))));
          }
          case (RogueCharacter)'\\':
          {
            if (ROGUE_COND(((remaining_count_1) == (0))))
            {
              return (RogueInt32)(-1);
            }
            if (ROGUE_COND(((fcount_4) == (1))))
            {
              return (RogueInt32)(((RogueString___pattern_match_count__Int32_Int32_String_Int32_Int32_Logical( ROGUE_ARG(THIS), i0_0, remaining_count_1, format_2, ROGUE_ARG(((f0_3) + (1))), ROGUE_ARG(((fcount_4) - (1))), ignore_case_5 ))));
            }
            ++f0_3;
            --fcount_4;
            format_ch_6 = ((RogueCharacter)(((RogueString__get__Int32( format_2, f0_3 )))));
            if (ROGUE_COND(true)) goto _auto_640;
            break;
          }
          default:
          {
            if (ROGUE_COND(true)) goto _auto_640;
          }
        }
        goto _auto_638;
        }
      _auto_640:;
      {
        if (ROGUE_COND(((remaining_count_1) == (0))))
        {
          return (RogueInt32)(-1);
        }
        RogueCharacter ch_29 = (((RogueString__get__Int32( ROGUE_ARG(THIS), i0_0 ))));
        if (ROGUE_COND(ignore_case_5))
        {
          ch_29 = ((RogueCharacter)(((RogueCharacter__to_lowercase( ch_29 )))));
          format_ch_6 = ((RogueCharacter)(((RogueCharacter__to_lowercase( format_ch_6 )))));
        }
        if (ROGUE_COND(((format_ch_6) == (ch_29))))
        {
          return (RogueInt32)(((RogueString___pattern_match_count__Int32_Int32_String_Int32_Int32_Logical( ROGUE_ARG(THIS), ROGUE_ARG(((i0_0) + (1))), ROGUE_ARG(((remaining_count_1) - (1))), format_2, ROGUE_ARG(((f0_3) + (1))), ROGUE_ARG(((fcount_4) - (1))), ignore_case_5 ))));
        }
        else if (ROGUE_COND(((((ch_29) == ((RogueCharacter)' '))) || (((ch_29) == ((RogueCharacter)9))))))
        {
          if (ROGUE_COND(((f0_3) > (0))))
          {
            RogueInt32 n_30 = (1);
            while (ROGUE_COND(((n_30) < (remaining_count_1))))
            {
              ROGUE_GC_CHECK;
              ch_29 = ((RogueCharacter)(((RogueString__get__Int32( ROGUE_ARG(THIS), ROGUE_ARG(((i0_0) + (n_30))) )))));
              if (ROGUE_COND(((((ch_29) != ((RogueCharacter)' '))) && (((ch_29) != ((RogueCharacter)9))))))
              {
                goto _auto_641;
              }
              ++n_30;
            }
            _auto_641:;
            if (ROGUE_COND(((((((((i0_0) == (0))) || (((n_30) == (remaining_count_1))))) || (!(((RogueCharacter__is_identifier__Logical_Logical( ROGUE_ARG(((RogueString__get__Int32( ROGUE_ARG(THIS), ROGUE_ARG(((i0_0) - (1))) )))), false, false ))))))) || (!(((RogueCharacter__is_identifier__Logical_Logical( ROGUE_ARG(((RogueString__get__Int32( ROGUE_ARG(THIS), ROGUE_ARG(((i0_0) + (n_30))) )))), false, false ))))))))
            {
              return (RogueInt32)(((RogueString___pattern_match_count__Int32_Int32_String_Int32_Int32_Logical( ROGUE_ARG(THIS), ROGUE_ARG(((i0_0) + (n_30))), ROGUE_ARG(((remaining_count_1) - (n_30))), format_2, f0_3, fcount_4, ignore_case_5 ))));
            }
          }
        }
        }
      goto _auto_638;
    }
  }
  _auto_638:;
  return (RogueInt32)(-1);
}

RogueString* RogueString__from__Int32( RogueString* THIS, RogueInt32 i1_0 )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(RogueString__from__Int32_Int32( ROGUE_ARG(THIS), i1_0, ROGUE_ARG(((((RogueString__count( ROGUE_ARG(THIS) )))) - (1))) ))));
}

RogueString* RogueString__from__Int32_Int32( RogueString* THIS, RogueInt32 i1_0, RogueInt32 i2_1 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((i1_0) < (0))))
  {
    i1_0 = ((RogueInt32)(0));
  }
  else if (ROGUE_COND(((i2_1) >= (((RogueString__count( ROGUE_ARG(THIS) )))))))
  {
    i2_1 = ((RogueInt32)(((((RogueString__count( ROGUE_ARG(THIS) )))) - (1))));
  }
  if (ROGUE_COND(((i1_0) > (i2_1))))
  {
    return (RogueString*)(Rogue_literal_strings[0]);
  }
  if (ROGUE_COND(((i1_0) == (i2_1))))
  {
    return (RogueString*)(((RogueString*)(RogueString__operatorPLUS__Character( ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], Rogue_literal_strings[0] ))), ROGUE_ARG(((RogueString__get__Int32( ROGUE_ARG(THIS), i1_0 )))) ))));
  }
  if (ROGUE_COND(((((i1_0) == (0))) && (((i2_1) == (((((RogueString__count( ROGUE_ARG(THIS) )))) - (1))))))))
  {
    return (RogueString*)(THIS);
  }
  RogueInt32 byte_i1 = RogueString_set_cursor( THIS, i1_0 );
  RogueInt32 byte_limit = RogueString_set_cursor( THIS, i2_1+1 );
  int new_count = (byte_limit - byte_i1);
  RogueString* result = RogueString_create_with_byte_count( new_count );
  memcpy( result->utf8, THIS->utf8+byte_i1, new_count );
  return RogueString_validate( result );

}

RogueString* RogueString__from_first__Character_Logical( RogueString* THIS, RogueCharacter ch_0, RogueLogical ignore_case_1 )
{
  ROGUE_GC_CHECK;
  RogueOptionalInt32 i_2 = (((RogueString__locate__Character_OptionalInt32_Logical( ROGUE_ARG(THIS), ch_0, (RogueOptionalInt32__create()), ignore_case_1 ))));
  if (ROGUE_COND(!(i_2.exists)))
  {
    return (RogueString*)(Rogue_literal_strings[0]);
  }
  return (RogueString*)(((RogueString*)(RogueString__from__Int32( ROGUE_ARG(THIS), ROGUE_ARG(i_2.value) ))));
}

RogueCharacter RogueString__get__Int32( RogueString* THIS, RogueInt32 index_0 )
{
  ROGUE_GC_CHECK;
  return (RogueCharacter)(((RogueCharacter)(RogueString_character_at(THIS,index_0))));
}

RogueLogical RogueString__is_false( RogueString* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((RogueString__equals__String_Logical( ROGUE_ARG(THIS), Rogue_literal_strings[131], true )))))
  {
    return (RogueLogical)(true);
  }
  if (ROGUE_COND(((RogueString__equals__String_Logical( ROGUE_ARG(THIS), Rogue_literal_strings[409], true )))))
  {
    return (RogueLogical)(true);
  }
  if (ROGUE_COND(((RogueString__equals__String_Logical( ROGUE_ARG(THIS), Rogue_literal_strings[410], true )))))
  {
    return (RogueLogical)(true);
  }
  if (ROGUE_COND(((RogueString__equals__String_Logical( ROGUE_ARG(THIS), Rogue_literal_strings[411], true )))))
  {
    return (RogueLogical)(true);
  }
  if (ROGUE_COND(((RogueString__equals__String_Logical( ROGUE_ARG(THIS), Rogue_literal_strings[412], true )))))
  {
    return (RogueLogical)(true);
  }
  if (ROGUE_COND(((RogueString__equals__String_Logical( ROGUE_ARG(THIS), Rogue_literal_strings[413], true )))))
  {
    return (RogueLogical)(true);
  }
  if (ROGUE_COND(((RogueString__equals__String_Logical( ROGUE_ARG(THIS), Rogue_literal_strings[414], true )))))
  {
    return (RogueLogical)(true);
  }
  if (ROGUE_COND(((RogueString__equals__String_Logical( ROGUE_ARG(THIS), Rogue_literal_strings[263], false )))))
  {
    return (RogueLogical)(true);
  }
  return (RogueLogical)(false);
}

RogueCharacter RogueString__last( RogueString* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueCharacter)(((RogueString__get__Int32( ROGUE_ARG(THIS), ROGUE_ARG(((((RogueString__count( ROGUE_ARG(THIS) )))) - (1))) ))));
}

RogueString* RogueString__left_justified__Int32_Character( RogueString* THIS, RogueInt32 spaces_0, RogueCharacter fill_1 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((((RogueString__count( ROGUE_ARG(THIS) )))) >= (spaces_0))))
  {
    return (RogueString*)(THIS);
  }
  ROGUE_DEF_LOCAL_REF(RogueStringBuilder*,buffer_2,(((RogueStringBuilder*)(RogueStringBuilder__init__Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))), spaces_0 )))));
  RogueStringBuilder__print__String( buffer_2, ROGUE_ARG(THIS) );
  {
    RogueInt32 _auto_129_3 = (((RogueString__count( ROGUE_ARG(THIS) ))));
    RogueInt32 _auto_130_4 = (spaces_0);
    for (;ROGUE_COND(((_auto_129_3) < (_auto_130_4)));++_auto_129_3)
    {
      ROGUE_GC_CHECK;
      RogueStringBuilder__print__Character( buffer_2, fill_1 );
    }
  }
  return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( buffer_2 ))));
}

RogueString* RogueString__leftmost__Int32( RogueString* THIS, RogueInt32 n_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((n_0) >= (0))))
  {
    return (RogueString*)(((RogueString*)(RogueString__from__Int32_Int32( ROGUE_ARG(THIS), 0, ROGUE_ARG(((n_0) - (1))) ))));
  }
  else
  {
    return (RogueString*)(((RogueString*)(RogueString__from__Int32_Int32( ROGUE_ARG(THIS), 0, ROGUE_ARG(((((((RogueString__count( ROGUE_ARG(THIS) )))) + (n_0))) - (1))) ))));
  }
}

RogueOptionalInt32 RogueString__locate__Character_OptionalInt32_Logical( RogueString* THIS, RogueCharacter ch_0, RogueOptionalInt32 optional_i1_1, RogueLogical ignore_case_2 )
{
  ROGUE_GC_CHECK;
  RogueInt32 i_3 = (0);
  RogueInt32 limit_4 = (((RogueString__count( ROGUE_ARG(THIS) ))));
  if (ROGUE_COND(optional_i1_1.exists))
  {
    i_3 = ((RogueInt32)(optional_i1_1.value));
  }
  if (ROGUE_COND(ignore_case_2))
  {
    while (ROGUE_COND(((i_3) < (limit_4))))
    {
      ROGUE_GC_CHECK;
      if (ROGUE_COND(((((RogueCharacter__to_lowercase( ROGUE_ARG(((RogueString__get__Int32( ROGUE_ARG(THIS), i_3 )))) )))) == (((RogueCharacter__to_lowercase( ch_0 )))))))
      {
        return (RogueOptionalInt32)(RogueOptionalInt32( i_3, true ));
      }
      ++i_3;
    }
  }
  else
  {
    while (ROGUE_COND(((i_3) < (limit_4))))
    {
      ROGUE_GC_CHECK;
      if (ROGUE_COND(((((RogueString__get__Int32( ROGUE_ARG(THIS), i_3 )))) == (ch_0))))
      {
        return (RogueOptionalInt32)(RogueOptionalInt32( i_3, true ));
      }
      ++i_3;
    }
  }
  return (RogueOptionalInt32)((RogueOptionalInt32__create()));
}

RogueOptionalInt32 RogueString__locate__String_OptionalInt32_Logical( RogueString* THIS, RogueString* other_0, RogueOptionalInt32 optional_i1_1, RogueLogical ignore_case_2 )
{
  ROGUE_GC_CHECK;
  RogueInt32 other_count_3 = (((RogueString__count( other_0 ))));
  if (ROGUE_COND(((other_count_3) == (1))))
  {
    return (RogueOptionalInt32)(((RogueString__locate__Character_OptionalInt32_Logical( ROGUE_ARG(THIS), ROGUE_ARG(((RogueString__get__Int32( other_0, 0 )))), optional_i1_1, false ))));
  }
  RogueInt32 this_limit_4 = (((((((RogueString__count( ROGUE_ARG(THIS) )))) - (other_count_3))) + (1)));
  if (ROGUE_COND(((((other_count_3) == (0))) || (((this_limit_4) <= (0))))))
  {
    return (RogueOptionalInt32)((RogueOptionalInt32__create()));
  }
  RogueOptionalInt32 _auto_132_5 = (optional_i1_1);
  {
    RogueInt32 i_6 = (((((_auto_132_5.exists))) ? (_auto_132_5.value) : 0));
    RogueInt32 _auto_133_7 = (this_limit_4);
    for (;ROGUE_COND(((i_6) < (_auto_133_7)));++i_6)
    {
      ROGUE_GC_CHECK;
      if (ROGUE_COND(((RogueString__contains_at__String_Int32_Logical( ROGUE_ARG(THIS), other_0, i_6, ignore_case_2 )))))
      {
        return (RogueOptionalInt32)(RogueOptionalInt32( i_6, true ));
      }
    }
  }
  return (RogueOptionalInt32)((RogueOptionalInt32__create()));
}

RogueOptionalInt32 RogueString__locate_last__Character_OptionalInt32_Logical( RogueString* THIS, RogueCharacter ch_0, RogueOptionalInt32 starting_index_1, RogueLogical ignore_case_2 )
{
  ROGUE_GC_CHECK;
  RogueInt32 i_3 = (((((RogueString__count( ROGUE_ARG(THIS) )))) - (1)));
  if (ROGUE_COND(starting_index_1.exists))
  {
    i_3 = ((RogueInt32)(starting_index_1.value));
  }
  if (ROGUE_COND(ignore_case_2))
  {
    while (ROGUE_COND(((i_3) >= (0))))
    {
      ROGUE_GC_CHECK;
      if (ROGUE_COND(((((RogueCharacter__to_lowercase( ROGUE_ARG(((RogueString__get__Int32( ROGUE_ARG(THIS), i_3 )))) )))) == (((RogueCharacter__to_lowercase( ch_0 )))))))
      {
        return (RogueOptionalInt32)(RogueOptionalInt32( i_3, true ));
      }
      --i_3;
    }
  }
  else
  {
    while (ROGUE_COND(((i_3) >= (0))))
    {
      ROGUE_GC_CHECK;
      if (ROGUE_COND(((((RogueString__get__Int32( ROGUE_ARG(THIS), i_3 )))) == (ch_0))))
      {
        return (RogueOptionalInt32)(RogueOptionalInt32( i_3, true ));
      }
      --i_3;
    }
  }
  return (RogueOptionalInt32)((RogueOptionalInt32__create()));
}

RogueOptionalInt32 RogueString__locate_last__String_OptionalInt32_Logical( RogueString* THIS, RogueString* other_0, RogueOptionalInt32 starting_index_1, RogueLogical ignore_case_2 )
{
  ROGUE_GC_CHECK;
  RogueInt32 other_count_3 = (((RogueString__count( other_0 ))));
  if (ROGUE_COND(((other_count_3) == (1))))
  {
    return (RogueOptionalInt32)(((RogueString__locate_last__Character_OptionalInt32_Logical( ROGUE_ARG(THIS), ROGUE_ARG(((RogueString__get__Int32( other_0, 0 )))), starting_index_1, false ))));
  }
  RogueInt32 this_limit_4 = (((((((RogueString__count( ROGUE_ARG(THIS) )))) - (other_count_3))) + (1)));
  if (ROGUE_COND(((((other_count_3) == (0))) || (((this_limit_4) <= (0))))))
  {
    return (RogueOptionalInt32)((RogueOptionalInt32__create()));
  }
  RogueInt32 i_5 = 0;
  if (ROGUE_COND(starting_index_1.exists))
  {
    i_5 = ((RogueInt32)(((starting_index_1.value) + (1))));
    if (ROGUE_COND(((i_5) > (this_limit_4))))
    {
      i_5 = ((RogueInt32)(this_limit_4));
    }
  }
  else
  {
    i_5 = ((RogueInt32)(this_limit_4));
  }
  while (ROGUE_COND(((((RogueInt32)(--i_5
  ))) >= (0))))
  {
    ROGUE_GC_CHECK;
    if (ROGUE_COND(((RogueString__contains_at__String_Int32_Logical( ROGUE_ARG(THIS), other_0, i_5, ignore_case_2 )))))
    {
      return (RogueOptionalInt32)(RogueOptionalInt32( i_5, true ));
    }
  }
  return (RogueOptionalInt32)((RogueOptionalInt32__create()));
}

RogueOptionalSpan RogueString__locate_pattern__String_Int32_Logical( RogueString* THIS, RogueString* pattern_0, RogueInt32 i1_1, RogueLogical ignore_case_2 )
{
  ROGUE_GC_CHECK;
  {
    RogueInt32 i_4 = (i1_1);
    RogueInt32 _auto_142_5 = (((RogueString__count( ROGUE_ARG(THIS) ))));
    for (;ROGUE_COND(((i_4) < (_auto_142_5)));++i_4)
    {
      ROGUE_GC_CHECK;
      RogueInt32 n_3 = (((RogueString___pattern_match_count__Int32_Int32_String_Int32_Int32_Logical( ROGUE_ARG(THIS), i_4, ROGUE_ARG(((((RogueString__count( ROGUE_ARG(THIS) )))) - (i_4))), pattern_0, 0, ROGUE_ARG(((RogueString__count( pattern_0 )))), ignore_case_2 ))));
      if (ROGUE_COND(((n_3) != (-1))))
      {
        return (RogueOptionalSpan)(RogueOptionalSpan( RogueClassSpan( i_4, ((n_3) - (i_4)) ), true ));
      }
    }
  }
  return (RogueOptionalSpan)((RogueOptionalSpan__create()));
}

RogueInt32 RogueString__longest_line( RogueString* THIS )
{
  ROGUE_GC_CHECK;
  RogueInt32 longest_0 = (0);
  RogueInt32 cur_1 = (0);
  {
    ROGUE_DEF_LOCAL_REF(RogueString*,_auto_672_0,(THIS));
    RogueInt32 _auto_673_0 = (0);
    RogueInt32 _auto_674_0 = (((((RogueString__count( _auto_672_0 )))) - (1)));
    for (;ROGUE_COND(((_auto_673_0) <= (_auto_674_0)));++_auto_673_0)
    {
      ROGUE_GC_CHECK;
      RogueCharacter ch_0 = (((RogueString__get__Int32( _auto_672_0, _auto_673_0 ))));
      switch (ch_0)
      {
        case (RogueCharacter)13:
        {
          continue;
          break;
        }
        case (RogueCharacter)10:
        {
          longest_0 = ((RogueInt32)(((RogueInt32__or_larger__Int32( longest_0, cur_1 )))));
          cur_1 = ((RogueInt32)(0));
          break;
        }
        default:
        {
          ++cur_1;
        }
      }
    }
  }
  return (RogueInt32)(((RogueInt32__or_larger__Int32( longest_0, cur_1 ))));
}

RogueString* RogueString__operatorPLUS__Character( RogueString* THIS, RogueCharacter value_0 )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__Character( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), ROGUE_ARG(THIS) )))), value_0 )))) ))));
}

RogueString* RogueString__operatorPLUS__Int32( RogueString* THIS, RogueInt32 value_0 )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__Int32( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), ROGUE_ARG(THIS) )))), value_0 )))) ))));
}

RogueLogical RogueString__operatorEQUALSEQUALS__String( RogueString* THIS, RogueString* value_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((void*)value_0) == ((void*)NULL)))
  {
    return (RogueLogical)(false);
  }
  if (ROGUE_COND(((((((RogueString__hash_code( ROGUE_ARG(THIS) )))) != (((RogueString__hash_code( value_0 )))))) || (((((RogueString__count( ROGUE_ARG(THIS) )))) != (((RogueString__count( value_0 )))))))))
  {
    return (RogueLogical)(false);
  }
  return (RogueLogical)(((RogueLogical)((0==memcmp(THIS->utf8,value_0->utf8,THIS->byte_count)))));
}

RogueInt32 RogueString__operatorLTGT__String( RogueString* THIS, RogueString* other_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((void*)other_0) == ((void*)NULL)))
  {
    return (RogueInt32)(1);
  }
  if (THIS == other_0) return 0;

  RogueInt32 other_count = other_0->byte_count;
  RogueInt32 limit = THIS->byte_count;

  int result;
  if (limit == other_count)
  {
    // Strings are same length
    result = memcmp( THIS->utf8, other_0->utf8, limit );
    if (result == 0) return 0;
  }
  else
  {
    // Strings differ in length.  Compare the part that matches first.
    if (limit > other_count) limit = other_count;
    result = memcmp( THIS->utf8, other_0->utf8, limit );
    if (result == 0)
    {
      // Equal so far - the shorter string comes before the longer one.
      if (limit == other_count) return 1;
      return -1;
    }
  }
  if (result < 0) return -1;
  else            return 1;

}

RogueString* RogueString__operatorPLUS__Logical( RogueString* THIS, RogueLogical value_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(value_0))
  {
    return (RogueString*)((RogueString__operatorPLUS__String_String( ROGUE_ARG(THIS), Rogue_literal_strings[130] )));
  }
  else
  {
    return (RogueString*)((RogueString__operatorPLUS__String_String( ROGUE_ARG(THIS), Rogue_literal_strings[131] )));
  }
}

RogueString* RogueString__operatorPLUS__Object( RogueString* THIS, RogueObject* value_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!!(value_0)))
  {
    return (RogueString*)((RogueString__operatorPLUS__String_String( ROGUE_ARG(THIS), ROGUE_ARG(((RogueString*)Rogue_call_ROGUEM0( 8, value_0 ))) )));
  }
  else
  {
    return (RogueString*)((RogueString__operatorPLUS__String_String( ROGUE_ARG(THIS), Rogue_literal_strings[6] )));
  }
}

RogueString* RogueString__operatorPLUS__String( RogueString* THIS, RogueString* value_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((void*)value_0) == ((void*)NULL)))
  {
    return (RogueString*)((RogueString__operatorPLUS__String_String( ROGUE_ARG(THIS), Rogue_literal_strings[6] )));
  }
  if (ROGUE_COND(((((RogueString__count( ROGUE_ARG(THIS) )))) == (0))))
  {
    return (RogueString*)(value_0);
  }
  if (ROGUE_COND(((((RogueString__count( value_0 )))) == (0))))
  {
    return (RogueString*)(THIS);
  }
  return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), ROGUE_ARG(THIS) )))), value_0 )))) ))));
}

RogueString* RogueString__operatorTIMES__Int32( RogueString* THIS, RogueInt32 value_0 )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(RogueString__times__Int32( ROGUE_ARG(THIS), value_0 ))));
}

RogueString* RogueString__pluralized__Int32( RogueString* THIS, RogueInt32 quantity_0 )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueString*,st_1,(((RogueString*)(RogueString__replacing__String_String_Logical( ROGUE_ARG(THIS), Rogue_literal_strings[27], ROGUE_ARG(((RogueString*)(RogueString__operatorPLUS__Int32( ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], Rogue_literal_strings[0] ))), quantity_0 )))), false )))));
  if (ROGUE_COND(((RogueString__contains__Character_Logical( st_1, (RogueCharacter)'/', false )))))
  {
    if (ROGUE_COND(((quantity_0) == (1))))
    {
      return (RogueString*)(((RogueString*)(RogueString__before_first__Character_Logical( st_1, (RogueCharacter)'/', false ))));
    }
    else
    {
      return (RogueString*)(((RogueString*)(RogueString__after_last__Character_Logical( st_1, (RogueCharacter)'/', false ))));
    }
  }
  else
  {
    RogueOptionalInt32 alt1_2 = (((RogueString__locate__Character_OptionalInt32_Logical( st_1, (RogueCharacter)'(', (RogueOptionalInt32__create()), false ))));
    if (ROGUE_COND(alt1_2.exists))
    {
      RogueOptionalInt32 alt2_3 = (((RogueString__locate__Character_OptionalInt32_Logical( st_1, (RogueCharacter)')', RogueOptionalInt32( ((alt1_2.value) + (1)), true ), false ))));
      if (ROGUE_COND(!(alt2_3.exists)))
      {
        return (RogueString*)(THIS);
      }
      if (ROGUE_COND(((quantity_0) == (1))))
      {
        return (RogueString*)((RogueString__operatorPLUS__String_String( ROGUE_ARG(((RogueString*)(RogueString__before__Int32( st_1, ROGUE_ARG(alt1_2.value) )))), ROGUE_ARG(((RogueString*)(RogueString__after__Int32( st_1, ROGUE_ARG(alt2_3.value) )))) )));
      }
      return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(((RogueString*)(RogueString__before__Int32( st_1, ROGUE_ARG(alt1_2.value) )))) ))) )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(((RogueString*)(RogueString__from__Int32_Int32( st_1, ROGUE_ARG(((alt1_2.value) + (1))), ROGUE_ARG(((alt2_3.value) - (1))) )))) ))) )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(((RogueString*)(RogueString__after__Int32( st_1, ROGUE_ARG(alt2_3.value) )))) ))) )))) ))));
    }
    else
    {
      if (ROGUE_COND(((quantity_0) == (1))))
      {
        return (RogueString*)(st_1);
      }
      RogueInt32 index_4 = (0);
      RogueInt32 i_5 = (((RogueString__count( st_1 ))));
      while (ROGUE_COND(((i_5) > (0))))
      {
        ROGUE_GC_CHECK;
        --i_5;
        if (ROGUE_COND(((RogueCharacter__is_letter( ROGUE_ARG(((RogueString__get__Int32( st_1, i_5 )))) )))))
        {
          index_4 = ((RogueInt32)(i_5));
          goto _auto_473;
        }
      }
      _auto_473:;
      if (ROGUE_COND(((((RogueString__get__Int32( st_1, index_4 )))) == ((RogueCharacter)'s'))))
      {
        return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(((RogueString*)(RogueString__before__Int32( st_1, ROGUE_ARG(((index_4) + (1))) )))) ))) )))), Rogue_literal_strings[28] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(((RogueString*)(RogueString__after__Int32( st_1, index_4 )))) ))) )))) ))));
      }
      else
      {
        return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(((RogueString*)(RogueString__before__Int32( st_1, ROGUE_ARG(((index_4) + (1))) )))) ))) )))), Rogue_literal_strings[29] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(((RogueString*)(RogueString__after__Int32( st_1, index_4 )))) ))) )))) ))));
      }
    }
  }
}

RogueString* RogueString__replacing__Character_Character_Logical( RogueString* THIS, RogueCharacter look_for_0, RogueCharacter replace_with_1, RogueLogical ignore_case_2 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(((RogueString__contains__Character_Logical( ROGUE_ARG(THIS), look_for_0, ignore_case_2 ))))))
  {
    return (RogueString*)(THIS);
  }
  ROGUE_DEF_LOCAL_REF(RogueStringBuilder*,result_3,(((RogueStringBuilder*)(RogueStringBuilder__init__Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))), ROGUE_ARG(((RogueString__count( ROGUE_ARG(THIS) )))) )))));
  if (ROGUE_COND(ignore_case_2))
  {
    look_for_0 = ((RogueCharacter)(((RogueCharacter__to_lowercase( look_for_0 )))));
    {
      ROGUE_DEF_LOCAL_REF(RogueString*,_auto_523_0,(THIS));
      RogueInt32 _auto_524_0 = (0);
      RogueInt32 _auto_525_0 = (((((RogueString__count( _auto_523_0 )))) - (1)));
      for (;ROGUE_COND(((_auto_524_0) <= (_auto_525_0)));++_auto_524_0)
      {
        ROGUE_GC_CHECK;
        RogueCharacter ch_0 = (((RogueString__get__Int32( _auto_523_0, _auto_524_0 ))));
        if (ROGUE_COND(((((RogueCharacter__to_lowercase( ch_0 )))) == (look_for_0))))
        {
          RogueStringBuilder__print__Character( result_3, replace_with_1 );
        }
        else
        {
          RogueStringBuilder__print__Character( result_3, ch_0 );
        }
      }
    }
  }
  else
  {
    {
      ROGUE_DEF_LOCAL_REF(RogueString*,_auto_526_0,(THIS));
      RogueInt32 _auto_527_0 = (0);
      RogueInt32 _auto_528_0 = (((((RogueString__count( _auto_526_0 )))) - (1)));
      for (;ROGUE_COND(((_auto_527_0) <= (_auto_528_0)));++_auto_527_0)
      {
        ROGUE_GC_CHECK;
        RogueCharacter ch_0 = (((RogueString__get__Int32( _auto_526_0, _auto_527_0 ))));
        if (ROGUE_COND(((ch_0) == (look_for_0))))
        {
          RogueStringBuilder__print__Character( result_3, replace_with_1 );
        }
        else
        {
          RogueStringBuilder__print__Character( result_3, ch_0 );
        }
      }
    }
  }
  return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( result_3 ))));
}

RogueString* RogueString__replacing__String_String_Logical( RogueString* THIS, RogueString* look_for_0, RogueString* replace_with_1, RogueLogical ignore_case_2 )
{
  ROGUE_GC_CHECK;
  RogueOptionalInt32 i1_3 = (((RogueString__locate__String_OptionalInt32_Logical( ROGUE_ARG(THIS), look_for_0, (RogueOptionalInt32__create()), ignore_case_2 ))));
  if (ROGUE_COND(!(i1_3.exists)))
  {
    return (RogueString*)(THIS);
  }
  RogueInt32 i0_4 = (0);
  ROGUE_DEF_LOCAL_REF(RogueStringBuilder*,buffer_5,(((RogueStringBuilder*)(RogueStringBuilder__init__Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))), ROGUE_ARG(((RogueInt32)(((((RogueReal64)(((RogueString__count( ROGUE_ARG(THIS) )))))) * (1.1))))) )))));
  while (ROGUE_COND(i1_3.exists))
  {
    ROGUE_GC_CHECK;
    {
      RogueInt32 i_6 = (i0_4);
      RogueInt32 _auto_143_7 = (i1_3.value);
      for (;ROGUE_COND(((i_6) < (_auto_143_7)));++i_6)
      {
        ROGUE_GC_CHECK;
        RogueStringBuilder__print__Character( buffer_5, ROGUE_ARG(((RogueString__get__Int32( ROGUE_ARG(THIS), i_6 )))) );
      }
    }
    RogueStringBuilder__print__String( buffer_5, replace_with_1 );
    i0_4 = ((RogueInt32)(((i1_3.value) + (((RogueString__count( look_for_0 )))))));
    i1_3 = ((RogueOptionalInt32)(((RogueString__locate__String_OptionalInt32_Logical( ROGUE_ARG(THIS), look_for_0, RogueOptionalInt32( i0_4, true ), ignore_case_2 )))));
  }
  {
    RogueInt32 i_8 = (i0_4);
    RogueInt32 _auto_144_9 = (((RogueString__count( ROGUE_ARG(THIS) ))));
    for (;ROGUE_COND(((i_8) < (_auto_144_9)));++i_8)
    {
      ROGUE_GC_CHECK;
      RogueStringBuilder__print__Character( buffer_5, ROGUE_ARG(((RogueString__get__Int32( ROGUE_ARG(THIS), i_8 )))) );
    }
  }
  return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( buffer_5 ))));
}

RogueString* RogueString__replacing_at__Int32_Int32_String( RogueString* THIS, RogueInt32 index_0, RogueInt32 n_1, RogueString* replace_with_2 )
{
  ROGUE_GC_CHECK;
  {
    ROGUE_DEF_LOCAL_REF(RogueException*,_auto_401_0,0);
    ROGUE_DEF_LOCAL_REF(RogueClassStringBuilderPool*,_auto_400_0,(RogueStringBuilder_pool));
    ROGUE_DEF_LOCAL_REF(RogueString*,_auto_402_0,0);
    ROGUE_DEF_LOCAL_REF(RogueStringBuilder*,builder_0,(((RogueStringBuilder*)(RogueStringBuilderPool__on_use( _auto_400_0 )))));
    Rogue_ignore_unused(builder_0);

    ROGUE_TRY
    {
      RogueStringBuilder__reserve__Int32( builder_0, ROGUE_ARG(((RogueInt32__clamped_low__Int32( ROGUE_ARG(((((RogueString__count( ROGUE_ARG(THIS) )))) + (((((RogueString__count( replace_with_2 )))) - (n_1))))), 0 )))) );
      {
        RogueInt32 i_3 = (0);
        RogueInt32 _auto_145_4 = (index_0);
        for (;ROGUE_COND(((i_3) < (_auto_145_4)));++i_3)
        {
          ROGUE_GC_CHECK;
          RogueStringBuilder__print__Character( builder_0, ROGUE_ARG(((RogueString__get__Int32( ROGUE_ARG(THIS), i_3 )))) );
        }
      }
      RogueStringBuilder__print__String( builder_0, replace_with_2 );
      {
        RogueInt32 i_5 = (((index_0) + (n_1)));
        RogueInt32 _auto_146_6 = (((RogueString__count( ROGUE_ARG(THIS) ))));
        for (;ROGUE_COND(((i_5) < (_auto_146_6)));++i_5)
        {
          ROGUE_GC_CHECK;
          RogueStringBuilder__print__Character( builder_0, ROGUE_ARG(((RogueString__get__Int32( ROGUE_ARG(THIS), i_5 )))) );
        }
      }
      {
        _auto_402_0 = ((RogueString*)(((RogueString*)(RogueStringBuilder__to_String( builder_0 )))));
        goto _auto_629;
      }
    }
    ROGUE_CATCH( RogueException,_auto_403_0 )
    {
      _auto_401_0 = ((RogueException*)(_auto_403_0));
    }
    ROGUE_END_TRY
    _auto_629:;
    RogueStringBuilderPool__on_end_use__StringBuilder( _auto_400_0, builder_0 );
    if (ROGUE_COND(!!(_auto_401_0)))
    {
      throw ((RogueException*)Rogue_call_ROGUEM0( 16, _auto_401_0 ));
    }
    return (RogueString*)(_auto_402_0);
  }
}

RogueString* RogueString__rightmost__Int32( RogueString* THIS, RogueInt32 n_0 )
{
  ROGUE_GC_CHECK;
  RogueInt32 this_count_1 = (((RogueString__count( ROGUE_ARG(THIS) ))));
  if (ROGUE_COND(((n_0) < (0))))
  {
    return (RogueString*)(((RogueString*)(RogueString__from__Int32_Int32( ROGUE_ARG(THIS), ROGUE_ARG((-(n_0))), ROGUE_ARG(((this_count_1) - (1))) ))));
  }
  else
  {
    return (RogueString*)(((RogueString*)(RogueString__from__Int32_Int32( ROGUE_ARG(THIS), ROGUE_ARG(((this_count_1) - (n_0))), ROGUE_ARG(((this_count_1) - (1))) ))));
  }
}

RogueString_List* RogueString__split__Character_Logical( RogueString* THIS, RogueCharacter separator_0, RogueLogical ignore_case_1 )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueString_List*,result_2,(((RogueString_List*)(RogueString_List__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueString_List*,ROGUE_CREATE_OBJECT(String_List))) )))));
  RogueInt32 i1_3 = (0);
  RogueOptionalInt32 i2_4 = (((RogueString__locate__Character_OptionalInt32_Logical( ROGUE_ARG(THIS), separator_0, RogueOptionalInt32( i1_3, true ), ignore_case_1 ))));
  while (ROGUE_COND(i2_4.exists))
  {
    ROGUE_GC_CHECK;
    RogueString_List__add__String( result_2, ROGUE_ARG(((RogueString*)(RogueString__from__Int32_Int32( ROGUE_ARG(THIS), i1_3, ROGUE_ARG(((i2_4.value) - (1))) )))) );
    i1_3 = ((RogueInt32)(((i2_4.value) + (1))));
    i2_4 = ((RogueOptionalInt32)(((RogueString__locate__Character_OptionalInt32_Logical( ROGUE_ARG(THIS), separator_0, RogueOptionalInt32( i1_3, true ), ignore_case_1 )))));
  }
  RogueString_List__add__String( result_2, ROGUE_ARG(((RogueString*)(RogueString__from__Int32( ROGUE_ARG(THIS), i1_3 )))) );
  return (RogueString_List*)(result_2);
}

RogueString_List* RogueString__split__String_Logical( RogueString* THIS, RogueString* separator_0, RogueLogical ignore_case_1 )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueString_List*,result_2,(((RogueString_List*)(RogueString_List__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueString_List*,ROGUE_CREATE_OBJECT(String_List))) )))));
  RogueInt32 separator_count_3 = (((RogueString__count( separator_0 ))));
  RogueInt32 i1_4 = (0);
  RogueOptionalInt32 i2_5 = (((RogueString__locate__String_OptionalInt32_Logical( ROGUE_ARG(THIS), separator_0, RogueOptionalInt32( i1_4, true ), ignore_case_1 ))));
  while (ROGUE_COND(i2_5.exists))
  {
    ROGUE_GC_CHECK;
    RogueString_List__add__String( result_2, ROGUE_ARG(((RogueString*)(RogueString__from__Int32_Int32( ROGUE_ARG(THIS), i1_4, ROGUE_ARG(((i2_5.value) - (1))) )))) );
    i1_4 = ((RogueInt32)(((i2_5.value) + (separator_count_3))));
    i2_5 = ((RogueOptionalInt32)(((RogueString__locate__String_OptionalInt32_Logical( ROGUE_ARG(THIS), separator_0, RogueOptionalInt32( i1_4, true ), ignore_case_1 )))));
  }
  RogueString_List__add__String( result_2, ROGUE_ARG(((RogueString*)(RogueString__from__Int32( ROGUE_ARG(THIS), i1_4 )))) );
  return (RogueString_List*)(result_2);
}

RogueString* RogueString__substring__Int32( RogueString* THIS, RogueInt32 i1_0 )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(RogueString__from__Int32( ROGUE_ARG(THIS), i1_0 ))));
}

RogueString* RogueString__substring__Int32_Int32( RogueString* THIS, RogueInt32 i1_0, RogueInt32 n_1 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((n_1) < (0))))
  {
    n_1 = ((RogueInt32)(((((((RogueString__count( ROGUE_ARG(THIS) )))) - (i1_0))) + (n_1))));
    if (ROGUE_COND(((n_1) <= (0))))
    {
      return (RogueString*)(Rogue_literal_strings[0]);
    }
  }
  return (RogueString*)(((RogueString*)(RogueString__from__Int32_Int32( ROGUE_ARG(THIS), i1_0, ROGUE_ARG(((i1_0) + (((n_1) - (1))))) ))));
}

RogueString* RogueString__times__Int32( RogueString* THIS, RogueInt32 n_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((n_0) <= (0))))
  {
    return (RogueString*)(Rogue_literal_strings[0]);
  }
  if (ROGUE_COND(((n_0) == (1))))
  {
    return (RogueString*)(THIS);
  }
  ROGUE_DEF_LOCAL_REF(RogueStringBuilder*,builder_1,(((RogueStringBuilder*)(RogueStringBuilder__init__Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))), ROGUE_ARG(((((RogueString__count( ROGUE_ARG(THIS) )))) * (n_0))) )))));
  {
    RogueInt32 _auto_150_2 = (1);
    RogueInt32 _auto_151_3 = (n_0);
    for (;ROGUE_COND(((_auto_150_2) <= (_auto_151_3)));++_auto_150_2)
    {
      ROGUE_GC_CHECK;
      RogueStringBuilder__print__String( builder_1, ROGUE_ARG(THIS) );
    }
  }
  return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( builder_1 ))));
}

RogueInt64 RogueString__to_Int64__Int32( RogueString* THIS, RogueInt32 base_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((((RogueString__count( ROGUE_ARG(THIS) )))) == (0))))
  {
    return (RogueInt64)(0LL);
  }
  if (ROGUE_COND(((RogueString__contains__Character_Logical( ROGUE_ARG(THIS), (RogueCharacter)',', false )))))
  {
    {
      ROGUE_DEF_LOCAL_REF(RogueException*,_auto_530_0,0);
      ROGUE_DEF_LOCAL_REF(RogueClassStringBuilderPool*,_auto_529_0,(RogueStringBuilder_pool));
      RogueInt64 _auto_531_0 = 0;
      ROGUE_DEF_LOCAL_REF(RogueStringBuilder*,builder_0,(((RogueStringBuilder*)(RogueStringBuilderPool__on_use( _auto_529_0 )))));
      Rogue_ignore_unused(builder_0);

      ROGUE_TRY
      {
        {
          ROGUE_DEF_LOCAL_REF(RogueString*,_auto_533_0,(THIS));
          RogueInt32 _auto_534_0 = (0);
          RogueInt32 _auto_535_0 = (((((RogueString__count( _auto_533_0 )))) - (1)));
          for (;ROGUE_COND(((_auto_534_0) <= (_auto_535_0)));++_auto_534_0)
          {
            ROGUE_GC_CHECK;
            RogueCharacter ch_0 = (((RogueString__get__Int32( _auto_533_0, _auto_534_0 ))));
            if (ROGUE_COND(((ch_0) != ((RogueCharacter)','))))
            {
              RogueStringBuilder__print__Character( builder_0, ch_0 );
            }
          }
        }
        {
          _auto_531_0 = ((RogueInt64)(((RogueStringBuilder__to_Int64( builder_0 )))));
          goto _auto_546;
        }
      }
      ROGUE_CATCH( RogueException,_auto_532_0 )
      {
        _auto_530_0 = ((RogueException*)(_auto_532_0));
      }
      ROGUE_END_TRY
      _auto_546:;
      RogueStringBuilderPool__on_end_use__StringBuilder( _auto_529_0, builder_0 );
      if (ROGUE_COND(!!(_auto_530_0)))
      {
        throw ((RogueException*)Rogue_call_ROGUEM0( 16, _auto_530_0 ));
      }
      return (RogueInt64)(_auto_531_0);
    }
  }
  return (RogueInt64)(((RogueInt64)((RogueInt64)strtoll( (char*)THIS->utf8, 0, base_0 ))));
}

RogueReal64 RogueString__to_Real64( RogueString* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((((RogueString__count( ROGUE_ARG(THIS) )))) == (0))))
  {
    return (RogueReal64)(0.0);
  }
  if (ROGUE_COND(((RogueString__contains__Character_Logical( ROGUE_ARG(THIS), (RogueCharacter)',', false )))))
  {
    {
      ROGUE_DEF_LOCAL_REF(RogueException*,_auto_548_0,0);
      ROGUE_DEF_LOCAL_REF(RogueClassStringBuilderPool*,_auto_547_0,(RogueStringBuilder_pool));
      RogueReal64 _auto_549_0 = 0;
      ROGUE_DEF_LOCAL_REF(RogueStringBuilder*,builder_0,(((RogueStringBuilder*)(RogueStringBuilderPool__on_use( _auto_547_0 )))));
      Rogue_ignore_unused(builder_0);

      ROGUE_TRY
      {
        {
          ROGUE_DEF_LOCAL_REF(RogueString*,_auto_551_0,(THIS));
          RogueInt32 _auto_552_0 = (0);
          RogueInt32 _auto_553_0 = (((((RogueString__count( _auto_551_0 )))) - (1)));
          for (;ROGUE_COND(((_auto_552_0) <= (_auto_553_0)));++_auto_552_0)
          {
            ROGUE_GC_CHECK;
            RogueCharacter ch_0 = (((RogueString__get__Int32( _auto_551_0, _auto_552_0 ))));
            if (ROGUE_COND(((ch_0) != ((RogueCharacter)','))))
            {
              RogueStringBuilder__print__Character( builder_0, ch_0 );
            }
          }
        }
        {
          _auto_549_0 = ((RogueReal64)(((RogueStringBuilder__to_Real64( builder_0 )))));
          goto _auto_562;
        }
      }
      ROGUE_CATCH( RogueException,_auto_550_0 )
      {
        _auto_548_0 = ((RogueException*)(_auto_550_0));
      }
      ROGUE_END_TRY
      _auto_562:;
      RogueStringBuilderPool__on_end_use__StringBuilder( _auto_547_0, builder_0 );
      if (ROGUE_COND(!!(_auto_548_0)))
      {
        throw ((RogueException*)Rogue_call_ROGUEM0( 16, _auto_548_0 ));
      }
      return (RogueReal64)(_auto_549_0);
    }
  }
  return (RogueReal64)(((RogueReal64)(strtod( (char*)THIS->utf8, 0 ))));
}

RogueString* RogueString__to_lowercase( RogueString* THIS )
{
  ROGUE_GC_CHECK;
  RogueLogical has_uc_0 = (false);
  {
    ROGUE_DEF_LOCAL_REF(RogueString*,_auto_711_0,(THIS));
    RogueInt32 _auto_712_0 = (0);
    RogueInt32 _auto_713_0 = (((((RogueString__count( _auto_711_0 )))) - (1)));
    for (;ROGUE_COND(((_auto_712_0) <= (_auto_713_0)));++_auto_712_0)
    {
      ROGUE_GC_CHECK;
      RogueCharacter ch_0 = (((RogueString__get__Int32( _auto_711_0, _auto_712_0 ))));
      if (ROGUE_COND(((((ch_0) >= ((RogueCharacter)'A'))) && (((ch_0) <= ((RogueCharacter)'Z'))))))
      {
        has_uc_0 = ((RogueLogical)(true));
        goto _auto_714;
      }
    }
  }
  _auto_714:;
  if (ROGUE_COND(!(has_uc_0)))
  {
    return (RogueString*)(THIS);
  }
  ROGUE_DEF_LOCAL_REF(RogueStringBuilder*,result_1,(((RogueStringBuilder*)(RogueStringBuilder__init__Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))), ROGUE_ARG(((RogueString__count( ROGUE_ARG(THIS) )))) )))));
  {
    ROGUE_DEF_LOCAL_REF(RogueString*,_auto_715_0,(THIS));
    RogueInt32 _auto_716_0 = (0);
    RogueInt32 _auto_717_0 = (((((RogueString__count( _auto_715_0 )))) - (1)));
    for (;ROGUE_COND(((_auto_716_0) <= (_auto_717_0)));++_auto_716_0)
    {
      ROGUE_GC_CHECK;
      RogueCharacter ch_0 = (((RogueString__get__Int32( _auto_715_0, _auto_716_0 ))));
      if (ROGUE_COND(((((ch_0) >= ((RogueCharacter)'A'))) && (((ch_0) <= ((RogueCharacter)'Z'))))))
      {
        RogueStringBuilder__print__Character( result_1, ROGUE_ARG(((((ch_0) - ((RogueCharacter)'A'))) + ((RogueCharacter)'a'))) );
      }
      else
      {
        RogueStringBuilder__print__Character( result_1, ch_0 );
      }
    }
  }
  return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( result_1 ))));
}

RogueString* RogueString__to_escaped_ascii__String( RogueString* THIS, RogueString* additional_characters_to_escape_0 )
{
  ROGUE_GC_CHECK;
  {
    ROGUE_DEF_LOCAL_REF(RogueException*,_auto_720_0,0);
    RogueLogical _auto_719_0 = 0;
    ROGUE_DEF_LOCAL_REF(RogueClassStringBuilderPool*,_auto_718_0,(RogueStringBuilder_pool));
    ROGUE_DEF_LOCAL_REF(RogueString*,_auto_721_0,0);
    ROGUE_DEF_LOCAL_REF(RogueStringBuilder*,builder_0,(((RogueStringBuilder*)(RogueStringBuilderPool__on_use( _auto_718_0 )))));
    Rogue_ignore_unused(builder_0);

    ROGUE_TRY
    {
      {
        ROGUE_DEF_LOCAL_REF(RogueString*,_auto_723_0,(THIS));
        RogueInt32 _auto_724_0 = (0);
        RogueInt32 _auto_725_0 = (((((RogueString__count( _auto_723_0 )))) - (1)));
        for (;ROGUE_COND(((_auto_724_0) <= (_auto_725_0)));++_auto_724_0)
        {
          ROGUE_GC_CHECK;
          RogueCharacter _auto_153_0 = (((RogueString__get__Int32( _auto_723_0, _auto_724_0 ))));
          RogueCharacter__print_escaped_ascii__PrintWriter_String( _auto_153_0, ROGUE_ARG(((((RogueClassPrintWriter*)(builder_0))))), additional_characters_to_escape_0 );
        }
      }
      if (ROGUE_COND(((RogueStringBuilder__operatorEQUALSEQUALS__String( builder_0, ROGUE_ARG(THIS) )))))
      {
        {
          _auto_719_0 = ((RogueLogical)(true));
          _auto_721_0 = ((RogueString*)(THIS));
          goto _auto_726;
        }
      }
      else
      {
        {
          _auto_719_0 = ((RogueLogical)(true));
          _auto_721_0 = ((RogueString*)(((RogueString*)(RogueStringBuilder__to_String( builder_0 )))));
          goto _auto_726;
        }
      }
    }
    ROGUE_CATCH( RogueException,_auto_722_0 )
    {
      _auto_720_0 = ((RogueException*)(_auto_722_0));
    }
    ROGUE_END_TRY
    _auto_726:;
    RogueStringBuilderPool__on_end_use__StringBuilder( _auto_718_0, builder_0 );
    if (ROGUE_COND(!!(_auto_720_0)))
    {
      throw ((RogueException*)Rogue_call_ROGUEM0( 16, _auto_720_0 ));
    }
    if (ROGUE_COND(_auto_719_0))
    {
      return (RogueString*)(_auto_721_0);
    }
  }
  return (RogueString*)(THIS);
}

RogueString* RogueString__trimmed( RogueString* THIS )
{
  ROGUE_GC_CHECK;
  RogueInt32 i1_0 = (0);
  RogueInt32 i2_1 = (((((RogueString__count( ROGUE_ARG(THIS) )))) - (1)));
  while (ROGUE_COND(((i1_0) <= (i2_1))))
  {
    ROGUE_GC_CHECK;
    if (ROGUE_COND(((((RogueString__get__Int32( ROGUE_ARG(THIS), i1_0 )))) <= ((RogueCharacter)' '))))
    {
      ++i1_0;
    }
    else if (ROGUE_COND(((((RogueString__get__Int32( ROGUE_ARG(THIS), i2_1 )))) <= ((RogueCharacter)' '))))
    {
      --i2_1;
    }
    else
    {
      goto _auto_747;
    }
  }
  _auto_747:;
  if (ROGUE_COND(((i1_0) > (i2_1))))
  {
    return (RogueString*)(Rogue_literal_strings[0]);
  }
  if (ROGUE_COND(((((i1_0) == (0))) && (((i2_1) == (((((RogueString__count( ROGUE_ARG(THIS) )))) - (1))))))))
  {
    return (RogueString*)(THIS);
  }
  return (RogueString*)(((RogueString*)(RogueString__from__Int32_Int32( ROGUE_ARG(THIS), i1_0, i2_1 ))));
}

RogueString* RogueString__without_trailing__Character( RogueString* THIS, RogueCharacter ch_0 )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueString*,result_1,(THIS));
  ROGUE_DEF_LOCAL_REF(RogueString*,next_2,(((RogueString*)(RogueString__before_suffix__Character_Logical( result_1, ch_0, false )))));
  while (ROGUE_COND(!((RogueString__operatorEQUALSEQUALS__String_String( result_1, next_2 )))))
  {
    ROGUE_GC_CHECK;
    result_1 = ((RogueString*)(next_2));
    next_2 = ((RogueString*)(((RogueString*)(RogueString__before_suffix__Character_Logical( result_1, ch_0, false )))));
  }
  return (RogueString*)(result_1);
}

RogueString_List* RogueString__word_wrap__Int32_String( RogueString* THIS, RogueInt32 width_0, RogueString* allow_break_after_1 )
{
  ROGUE_GC_CHECK;
  return (RogueString_List*)(((RogueString_List*)(RogueString__split__Character_Logical( ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueString__word_wrap__Int32_StringBuilder_String( ROGUE_ARG(THIS), width_0, ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), allow_break_after_1 )))) )))), (RogueCharacter)10, false ))));
}

RogueStringBuilder* RogueString__word_wrap__Int32_StringBuilder_String( RogueString* THIS, RogueInt32 width_0, RogueStringBuilder* buffer_1, RogueString* allow_break_after_2 )
{
  ROGUE_GC_CHECK;
  RogueInt32 i1_3 = 0;
  RogueInt32 i2_4 = 0;
  RogueInt32 len_5 = (((RogueString__count( ROGUE_ARG(THIS) ))));
  if (ROGUE_COND(((len_5) == (0))))
  {
    return (RogueStringBuilder*)(buffer_1);
  }
  RogueInt32 w_6 = (width_0);
  RogueInt32 initial_indent_7 = (0);
  {
    ROGUE_DEF_LOCAL_REF(RogueString*,_auto_602_0,(THIS));
    RogueInt32 _auto_603_0 = (0);
    RogueInt32 _auto_604_0 = (((((RogueString__count( _auto_602_0 )))) - (1)));
    for (;ROGUE_COND(((_auto_603_0) <= (_auto_604_0)));++_auto_603_0)
    {
      ROGUE_GC_CHECK;
      RogueCharacter ch_0 = (((RogueString__get__Int32( _auto_602_0, _auto_603_0 ))));
      if (ROGUE_COND(((ch_0) != ((RogueCharacter)' '))))
      {
        goto _auto_605;
      }
      ++initial_indent_7;
      --w_6;
      ++i1_3;
    }
  }
  _auto_605:;
  if (ROGUE_COND(((w_6) <= (0))))
  {
    w_6 = ((RogueInt32)(width_0));
    initial_indent_7 = ((RogueInt32)(0));
    RogueStringBuilder__println( buffer_1 );
  }
  else
  {
    {
      RogueInt32 _auto_154_8 = (1);
      RogueInt32 _auto_155_9 = (((width_0) - (w_6)));
      for (;ROGUE_COND(((_auto_154_8) <= (_auto_155_9)));++_auto_154_8)
      {
        ROGUE_GC_CHECK;
        RogueStringBuilder__print__Character( buffer_1, (RogueCharacter)' ' );
      }
    }
  }
  RogueLogical needs_newline_10 = (false);
  while (ROGUE_COND(((i2_4) < (len_5))))
  {
    ROGUE_GC_CHECK;
    while (ROGUE_COND(((((((((i2_4) - (i1_3))) < (w_6))) && (((i2_4) < (len_5))))) && (((((RogueString__get__Int32( ROGUE_ARG(THIS), i2_4 )))) != ((RogueCharacter)10))))))
    {
      ROGUE_GC_CHECK;
      ++i2_4;
    }
    if (ROGUE_COND(((((i2_4) - (i1_3))) == (w_6))))
    {
      if (ROGUE_COND(((i2_4) >= (len_5))))
      {
        i2_4 = ((RogueInt32)(len_5));
      }
      else if (ROGUE_COND(((((RogueString__get__Int32( ROGUE_ARG(THIS), i2_4 )))) != ((RogueCharacter)10))))
      {
        while (ROGUE_COND(((((((RogueString__get__Int32( ROGUE_ARG(THIS), i2_4 )))) != ((RogueCharacter)' '))) && (((i2_4) > (i1_3))))))
        {
          ROGUE_GC_CHECK;
          --i2_4;
        }
        if (ROGUE_COND(((i2_4) == (i1_3))))
        {
          i2_4 = ((RogueInt32)(((i1_3) + (w_6))));
          if (ROGUE_COND(!!(allow_break_after_2)))
          {
            while (ROGUE_COND(((((((i2_4) > (i1_3))) && (!(((RogueString__contains__Character_Logical( allow_break_after_2, ROGUE_ARG(((RogueString__get__Int32( ROGUE_ARG(THIS), ROGUE_ARG(((i2_4) - (1))) )))), false ))))))) && (((i2_4) > (i1_3))))))
            {
              ROGUE_GC_CHECK;
              --i2_4;
            }
            if (ROGUE_COND(((i2_4) == (i1_3))))
            {
              i2_4 = ((RogueInt32)(((i1_3) + (w_6))));
            }
          }
        }
      }
    }
    if (ROGUE_COND(needs_newline_10))
    {
      RogueStringBuilder__println( buffer_1 );
      if (ROGUE_COND(!!(initial_indent_7)))
      {
        {
          RogueInt32 _auto_156_11 = (1);
          RogueInt32 _auto_157_12 = (initial_indent_7);
          for (;ROGUE_COND(((_auto_156_11) <= (_auto_157_12)));++_auto_156_11)
          {
            ROGUE_GC_CHECK;
            RogueStringBuilder__print__Character( buffer_1, (RogueCharacter)' ' );
          }
        }
      }
    }
    {
      RogueInt32 i_13 = (i1_3);
      RogueInt32 _auto_158_14 = (((i2_4) - (1)));
      for (;ROGUE_COND(((i_13) <= (_auto_158_14)));++i_13)
      {
        ROGUE_GC_CHECK;
        RogueStringBuilder__print__Character( buffer_1, ROGUE_ARG(((RogueString__get__Int32( ROGUE_ARG(THIS), i_13 )))) );
      }
    }
    needs_newline_10 = ((RogueLogical)(true));
    if (ROGUE_COND(((i2_4) == (len_5))))
    {
      return (RogueStringBuilder*)(buffer_1);
    }
    else
    {
      switch (((RogueString__get__Int32( ROGUE_ARG(THIS), i2_4 ))))
      {
        case (RogueCharacter)' ':
        {
          while (ROGUE_COND(((((i2_4) < (len_5))) && (((((RogueString__get__Int32( ROGUE_ARG(THIS), i2_4 )))) == ((RogueCharacter)' '))))))
          {
            ROGUE_GC_CHECK;
            ++i2_4;
          }
          if (ROGUE_COND(((((i2_4) < (len_5))) && (((((RogueString__get__Int32( ROGUE_ARG(THIS), i2_4 )))) == ((RogueCharacter)10))))))
          {
            ++i2_4;
          }
          i1_3 = ((RogueInt32)(i2_4));
          break;
        }
        case (RogueCharacter)10:
        {
          ++i2_4;
          w_6 = ((RogueInt32)(width_0));
          initial_indent_7 = ((RogueInt32)(0));
          {
            RogueInt32 i_15 = (i2_4);
            RogueInt32 _auto_159_16 = (len_5);
            for (;ROGUE_COND(((i_15) < (_auto_159_16)));++i_15)
            {
              ROGUE_GC_CHECK;
              if (ROGUE_COND(((((RogueString__get__Int32( ROGUE_ARG(THIS), i_15 )))) != ((RogueCharacter)' '))))
              {
                goto _auto_606;
              }
              ++initial_indent_7;
              --w_6;
              ++i2_4;
            }
          }
          _auto_606:;
          if (ROGUE_COND(((w_6) <= (0))))
          {
            w_6 = ((RogueInt32)(width_0));
            initial_indent_7 = ((RogueInt32)(0));
          }
          else
          {
            {
              RogueInt32 _auto_160_17 = (1);
              RogueInt32 _auto_161_18 = (((width_0) - (w_6)));
              for (;ROGUE_COND(((_auto_160_17) <= (_auto_161_18)));++_auto_160_17)
              {
                ROGUE_GC_CHECK;
                RogueStringBuilder__print__Character( buffer_1, (RogueCharacter)' ' );
              }
            }
          }
          break;
        }
      }
      i1_3 = ((RogueInt32)(i2_4));
    }
  }
  return (RogueStringBuilder*)(buffer_1);
}

RogueClassStackTrace* RogueStackTrace__init_object( RogueClassStackTrace* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  return (RogueClassStackTrace*)(THIS);
}

RogueString* RogueStackTrace__to_String( RogueClassStackTrace* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStackTrace__print__StringBuilder( ROGUE_ARG(THIS), ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))) )))) ))));
}

RogueString* RogueStackTrace__type_name( RogueClassStackTrace* THIS )
{
  return (RogueString*)(Rogue_literal_strings[351]);
}

RogueClassStackTrace* RogueStackTrace__init__Int32( RogueClassStackTrace* THIS, RogueInt32 omit_count_0 )
{
  ROGUE_GC_CHECK;
  ++omit_count_0;
  RogueDebugTrace* current = Rogue_call_stack;
  while (current && omit_count_0 > 0)
  {
    --omit_count_0;
    current = current->previous_trace;
  }
  if (current) THIS->count = current->count();

  THIS->entries = ((RogueString_List*)(RogueString_List__init__Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueString_List*,ROGUE_CREATE_OBJECT(String_List))), ROGUE_ARG(THIS->count) )));
  while (ROGUE_COND(((RogueLogical)(current!=0))))
  {
    ROGUE_GC_CHECK;
    RogueString_List__add__String( ROGUE_ARG(THIS->entries), ROGUE_ARG(((RogueString*)(RogueString_create_from_utf8( current->to_c_string() )))) );
    current = current->previous_trace;

  }
  return (RogueClassStackTrace*)(THIS);
}

void RogueStackTrace__format( RogueClassStackTrace* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(THIS->is_formatted))
  {
    return;
  }
  THIS->is_formatted = true;
  RogueInt32 max_characters_0 = (0);
  {
    ROGUE_DEF_LOCAL_REF(RogueString_List*,_auto_754_0,(THIS->entries));
    RogueInt32 _auto_755_0 = (0);
    RogueInt32 _auto_756_0 = (((_auto_754_0->count) - (1)));
    for (;ROGUE_COND(((_auto_755_0) <= (_auto_756_0)));++_auto_755_0)
    {
      ROGUE_GC_CHECK;
      ROGUE_DEF_LOCAL_REF(RogueString*,entry_0,(((RogueString*)(_auto_754_0->data->as_objects[_auto_755_0]))));
      RogueOptionalInt32 sp_1 = (((RogueString__locate__Character_OptionalInt32_Logical( entry_0, (RogueCharacter)' ', (RogueOptionalInt32__create()), false ))));
      if (ROGUE_COND(sp_1.exists))
      {
        max_characters_0 = ((RogueInt32)((RogueMath__max__Int32_Int32( max_characters_0, ROGUE_ARG(sp_1.value) ))));
      }
    }
  }
  ++max_characters_0;
  {
    ROGUE_DEF_LOCAL_REF(RogueString_List*,_auto_757_0,(THIS->entries));
    RogueInt32 i_0 = (0);
    RogueInt32 _auto_758_0 = (((_auto_757_0->count) - (1)));
    for (;ROGUE_COND(((i_0) <= (_auto_758_0)));++i_0)
    {
      ROGUE_GC_CHECK;
      ROGUE_DEF_LOCAL_REF(RogueString*,entry_0,(((RogueString*)(_auto_757_0->data->as_objects[i_0]))));
      if (ROGUE_COND(((RogueString__contains__Character_Logical( entry_0, (RogueCharacter)' ', false )))))
      {
        RogueString_List__set__Int32_String( ROGUE_ARG(THIS->entries), i_0, ROGUE_ARG((RogueString__operatorPLUS__String_String( ROGUE_ARG(((RogueString*)(RogueString__left_justified__Int32_Character( ROGUE_ARG(((RogueString*)(RogueString__before_first__Character_Logical( entry_0, (RogueCharacter)' ', false )))), max_characters_0, (RogueCharacter)' ' )))), ROGUE_ARG(((RogueString*)(RogueString__from_first__Character_Logical( entry_0, (RogueCharacter)' ', false )))) ))) );
      }
    }
  }
}

void RogueStackTrace__print( RogueClassStackTrace* THIS )
{
  ROGUE_GC_CHECK;
  RogueStackTrace__print__StringBuilder( ROGUE_ARG(THIS), ROGUE_ARG(((RogueClassGlobal*)ROGUE_SINGLETON(Global))->global_output_buffer) );
  RogueGlobal__flush( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)) );
}

RogueStringBuilder* RogueStackTrace__print__StringBuilder( RogueClassStackTrace* THIS, RogueStringBuilder* buffer_0 )
{
  ROGUE_GC_CHECK;
  RogueStackTrace__format( ROGUE_ARG(THIS) );
  {
    ROGUE_DEF_LOCAL_REF(RogueString_List*,_auto_759_0,(THIS->entries));
    RogueInt32 _auto_760_0 = (0);
    RogueInt32 _auto_761_0 = (((_auto_759_0->count) - (1)));
    for (;ROGUE_COND(((_auto_760_0) <= (_auto_761_0)));++_auto_760_0)
    {
      ROGUE_GC_CHECK;
      ROGUE_DEF_LOCAL_REF(RogueString*,entry_0,(((RogueString*)(_auto_759_0->data->as_objects[_auto_760_0]))));
      RogueStringBuilder__println__String( buffer_0, entry_0 );
    }
  }
  return (RogueStringBuilder*)(buffer_0);
}

RogueString_List* RogueString_List__init_object( RogueString_List* THIS )
{
  ROGUE_GC_CHECK;
  RogueGenericList__init_object( ROGUE_ARG(((RogueClassGenericList*)THIS)) );
  return (RogueString_List*)(THIS);
}

RogueString_List* RogueString_List__init( RogueString_List* THIS )
{
  ROGUE_GC_CHECK;
  RogueString_List__init__Int32( ROGUE_ARG(THIS), 0 );
  return (RogueString_List*)(THIS);
}

RogueString* RogueString_List__to_String( RogueString_List* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[293] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(((RogueString*)(RogueString_List__join__String( ROGUE_ARG(THIS), Rogue_literal_strings[15] )))) ))) )))), Rogue_literal_strings[294] )))) ))));
}

RogueString* RogueString_List__type_name( RogueString_List* THIS )
{
  return (RogueString*)(Rogue_literal_strings[377]);
}

RogueString_List* RogueString_List__init__Int32( RogueString_List* THIS, RogueInt32 initial_capacity_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((initial_capacity_0) != (((RogueString_List__capacity( ROGUE_ARG(THIS) )))))))
  {
    THIS->data = RogueType_create_array( initial_capacity_0, sizeof(RogueString*), true, 17 );
  }
  RogueString_List__clear( ROGUE_ARG(THIS) );
  return (RogueString_List*)(THIS);
}

RogueString_List* RogueString_List__cloned( RogueString_List* THIS )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueString_List*,result_0,(((RogueString_List*)(RogueString_List__init__Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueString_List*,ROGUE_CREATE_OBJECT(String_List))), ROGUE_ARG(THIS->count) )))));
  {
    ROGUE_DEF_LOCAL_REF(RogueString_List*,_auto_839_0,(THIS));
    RogueInt32 _auto_840_0 = (0);
    RogueInt32 _auto_841_0 = (((_auto_839_0->count) - (1)));
    for (;ROGUE_COND(((_auto_840_0) <= (_auto_841_0)));++_auto_840_0)
    {
      ROGUE_GC_CHECK;
      ROGUE_DEF_LOCAL_REF(RogueString*,value_0,(((RogueString*)(_auto_839_0->data->as_objects[_auto_840_0]))));
      RogueString_List__add__String( result_0, value_0 );
    }
  }
  return (RogueString_List*)(result_0);
}

void RogueString_List__add__String( RogueString_List* THIS, RogueString* value_0 )
{
  ROGUE_GC_CHECK;
  RogueString_List__set__Int32_String( ROGUE_ARG(((RogueString_List*)(RogueString_List__reserve__Int32( ROGUE_ARG(THIS), 1 )))), ROGUE_ARG(THIS->count), value_0 );
  THIS->count = ((THIS->count) + (1));
}

void RogueString_List__add__String_List( RogueString_List* THIS, RogueString_List* other_0 )
{
  ROGUE_GC_CHECK;
  RogueString_List__reserve__Int32( ROGUE_ARG(THIS), ROGUE_ARG(other_0->count) );
  {
    ROGUE_DEF_LOCAL_REF(RogueString_List*,_auto_902_0,(other_0));
    RogueInt32 _auto_903_0 = (0);
    RogueInt32 _auto_904_0 = (((_auto_902_0->count) - (1)));
    for (;ROGUE_COND(((_auto_903_0) <= (_auto_904_0)));++_auto_903_0)
    {
      ROGUE_GC_CHECK;
      ROGUE_DEF_LOCAL_REF(RogueString*,value_0,(((RogueString*)(_auto_902_0->data->as_objects[_auto_903_0]))));
      RogueString_List__add__String( ROGUE_ARG(THIS), value_0 );
    }
  }
}

RogueInt32 RogueString_List__capacity( RogueString_List* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(!!(THIS->data))))
  {
    return (RogueInt32)(0);
  }
  return (RogueInt32)(THIS->data->count);
}

void RogueString_List__clear( RogueString_List* THIS )
{
  ROGUE_GC_CHECK;
  RogueString_List__discard_from__Int32( ROGUE_ARG(THIS), 0 );
}

RogueLogical RogueString_List__contains__String( RogueString_List* THIS, RogueString* value_0 )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(((RogueString_List__locate__String_Int32( ROGUE_ARG(THIS), value_0, 0 ))).exists);
}

void RogueString_List__discard_from__Int32( RogueString_List* THIS, RogueInt32 index_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!!(THIS->data)))
  {
    RogueInt32 n_1 = (((THIS->count) - (index_0)));
    if (ROGUE_COND(((n_1) > (0))))
    {
      RogueArray__zero__Int32_Int32( ROGUE_ARG(((RogueArray*)THIS->data)), index_0, n_1 );
      THIS->count = index_0;
    }
  }
}

void RogueString_List__ensure_capacity__Int32( RogueString_List* THIS, RogueInt32 desired_capacity_0 )
{
  ROGUE_GC_CHECK;
  RogueString_List__reserve__Int32( ROGUE_ARG(THIS), ROGUE_ARG(((desired_capacity_0) - (THIS->count))) );
}

void RogueString_List__expand_to_count__Int32( RogueString_List* THIS, RogueInt32 minimum_count_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((THIS->count) < (minimum_count_0))))
  {
    RogueString_List__ensure_capacity__Int32( ROGUE_ARG(THIS), minimum_count_0 );
    THIS->count = minimum_count_0;
  }
}

void RogueString_List__expand_to_include__Int32( RogueString_List* THIS, RogueInt32 index_0 )
{
  ROGUE_GC_CHECK;
  RogueString_List__expand_to_count__Int32( ROGUE_ARG(THIS), ROGUE_ARG(((index_0) + (1))) );
}

RogueOptionalString RogueString_List__find___Function_String_RETURNSLogical_( RogueString_List* THIS, RogueClass_Function_String_RETURNSLogical_* query_0 )
{
  ROGUE_GC_CHECK;
  {
    ROGUE_DEF_LOCAL_REF(RogueString_List*,_auto_864_0,(THIS));
    RogueInt32 _auto_865_0 = (0);
    RogueInt32 _auto_866_0 = (((_auto_864_0->count) - (1)));
    for (;ROGUE_COND(((_auto_865_0) <= (_auto_866_0)));++_auto_865_0)
    {
      ROGUE_GC_CHECK;
      ROGUE_DEF_LOCAL_REF(RogueString*,value_0,(((RogueString*)(_auto_864_0->data->as_objects[_auto_865_0]))));
      if (ROGUE_COND((Rogue_call_ROGUEM9( 13, query_0, value_0 ))))
      {
        return (RogueOptionalString)(RogueOptionalString( value_0, true ));
      }
    }
  }
  return (RogueOptionalString)((RogueOptionalString__create()));
}

RogueString* RogueString_List__first( RogueString_List* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(THIS->data->as_objects[0])));
}

RogueString* RogueString_List__get__Int32( RogueString_List* THIS, RogueInt32 index_0 )
{
  return (RogueString*)(((RogueString*)(THIS->data->as_objects[index_0])));
}

void RogueString_List__insert__String_Int32( RogueString_List* THIS, RogueString* value_0, RogueInt32 before_index_1 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((before_index_1) < (0))))
  {
    before_index_1 = ((RogueInt32)(0));
  }
  if (ROGUE_COND(((before_index_1) >= (THIS->count))))
  {
    RogueString_List__add__String( ROGUE_ARG(THIS), value_0 );
    return;
  }
  else
  {
    RogueString_List__reserve__Int32( ROGUE_ARG(THIS), 1 );
    RogueString_List__shift__Int32_OptionalInt32_Int32_OptionalString( ROGUE_ARG(THIS), before_index_1, (RogueOptionalInt32__create()), 1, (RogueOptionalString__create()) );
    THIS->data->as_objects[before_index_1] = value_0;
  }
}

RogueLogical RogueString_List__is_empty( RogueString_List* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(((THIS->count) == (0)));
}

RogueString* RogueString_List__join__String( RogueString_List* THIS, RogueString* separator_0 )
{
  ROGUE_GC_CHECK;
  RogueInt32 total_count_1 = (0);
  {
    ROGUE_DEF_LOCAL_REF(RogueString_List*,_auto_748_0,(THIS));
    RogueInt32 _auto_749_0 = (0);
    RogueInt32 _auto_750_0 = (((_auto_748_0->count) - (1)));
    for (;ROGUE_COND(((_auto_749_0) <= (_auto_750_0)));++_auto_749_0)
    {
      ROGUE_GC_CHECK;
      ROGUE_DEF_LOCAL_REF(RogueString*,line_0,(((RogueString*)(_auto_748_0->data->as_objects[_auto_749_0]))));
      total_count_1 += ((RogueString__count( line_0 )));
    }
  }
  if (ROGUE_COND(!!(THIS->count)))
  {
    total_count_1 += ((((RogueString__count( separator_0 )))) * (((THIS->count) - (1))));
  }
  ROGUE_DEF_LOCAL_REF(RogueStringBuilder*,builder_2,(((RogueStringBuilder*)(RogueStringBuilder__init__Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))), total_count_1 )))));
  RogueLogical first_3 = (true);
  {
    ROGUE_DEF_LOCAL_REF(RogueString_List*,_auto_751_0,(THIS));
    RogueInt32 _auto_752_0 = (0);
    RogueInt32 _auto_753_0 = (((_auto_751_0->count) - (1)));
    for (;ROGUE_COND(((_auto_752_0) <= (_auto_753_0)));++_auto_752_0)
    {
      ROGUE_GC_CHECK;
      ROGUE_DEF_LOCAL_REF(RogueString*,line_0,(((RogueString*)(_auto_751_0->data->as_objects[_auto_752_0]))));
      if (ROGUE_COND(first_3))
      {
        first_3 = ((RogueLogical)(false));
      }
      else
      {
        RogueStringBuilder__print__String( builder_2, separator_0 );
      }
      RogueStringBuilder__print__String( builder_2, line_0 );
    }
  }
  return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( builder_2 ))));
}

void RogueString_List__keep___Function_String_RETURNSLogical_( RogueString_List* THIS, RogueClass_Function_String_RETURNSLogical_* keep_if_0 )
{
  ROGUE_GC_CHECK;
  {
    ROGUE_DEF_LOCAL_REF(RogueException*,_auto_1036_0,0);
    RogueLogical _auto_1035_0 = 0;
    ROGUE_DEF_LOCAL_REF(RogueClassListRewriter_String_*,_auto_1034_0,(((RogueClassListRewriter_String_*)(RogueString_List__rewriter( ROGUE_ARG(THIS) )))));
    ROGUE_DEF_LOCAL_REF(RogueClassListRewriter_String_*,rewriter_0,(((RogueClassListRewriter_String_*)(RogueListRewriter_String___on_use( _auto_1034_0 )))));
    Rogue_ignore_unused(rewriter_0);

    ROGUE_TRY
    {
      {
        ROGUE_DEF_LOCAL_REF(RogueClassListRewriter_String_*,_auto_1038_0,(rewriter_0));
        while (ROGUE_COND(((RogueListRewriter_String___has_another( _auto_1038_0 )))))
        {
          ROGUE_GC_CHECK;
          ROGUE_DEF_LOCAL_REF(RogueString*,value_0,(((RogueString*)(RogueListRewriter_String___read( _auto_1038_0 )))));
          if (ROGUE_COND((Rogue_call_ROGUEM9( 13, keep_if_0, value_0 ))))
          {
            RogueListRewriter_String___write__String( rewriter_0, value_0 );
          }
        }
      }
    }
    ROGUE_CATCH( RogueException,_auto_1037_0 )
    {
      _auto_1036_0 = ((RogueException*)(_auto_1037_0));
    }
    ROGUE_END_TRY
    _auto_1036_0 = ((RogueException*)(((RogueException*)(RogueListRewriter_String___on_end_use__Exception( _auto_1034_0, _auto_1036_0 )))));
    if (ROGUE_COND(!!(_auto_1036_0)))
    {
      throw ((RogueException*)Rogue_call_ROGUEM0( 16, _auto_1036_0 ));
    }
    if (ROGUE_COND(_auto_1035_0))
    {
      return;
    }
  }
}

RogueString* RogueString_List__last( RogueString_List* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(THIS->data->as_objects[((THIS->count) - (1))])));
}

RogueOptionalInt32 RogueString_List__locate__String_Int32( RogueString_List* THIS, RogueString* value_0, RogueInt32 i1_1 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(true))
  {
    if (ROGUE_COND(((void*)value_0) == ((void*)NULL)))
    {
      {
        ROGUE_DEF_LOCAL_REF(RogueString_List*,_auto_1084_0,(THIS));
        RogueInt32 i_0 = (i1_1);
        RogueInt32 _auto_1085_0 = (((_auto_1084_0->count) - (1)));
        for (;ROGUE_COND(((i_0) <= (_auto_1085_0)));++i_0)
        {
          ROGUE_GC_CHECK;
          if (ROGUE_COND(((void*)value_0) == ((void*)((RogueString*)(THIS->data->as_objects[i_0])))))
          {
            return (RogueOptionalInt32)(RogueOptionalInt32( i_0, true ));
          }
        }
      }
      return (RogueOptionalInt32)((RogueOptionalInt32__create()));
    }
  }
  {
    ROGUE_DEF_LOCAL_REF(RogueString_List*,_auto_1086_0,(THIS));
    RogueInt32 i_0 = (i1_1);
    RogueInt32 _auto_1087_0 = (((_auto_1086_0->count) - (1)));
    for (;ROGUE_COND(((i_0) <= (_auto_1087_0)));++i_0)
    {
      ROGUE_GC_CHECK;
      if (ROGUE_COND((RogueString__operatorEQUALSEQUALS__String_String( value_0, ROGUE_ARG(((RogueString*)(THIS->data->as_objects[i_0]))) ))))
      {
        return (RogueOptionalInt32)(RogueOptionalInt32( i_0, true ));
      }
    }
  }
  return (RogueOptionalInt32)((RogueOptionalInt32__create()));
}

RogueOptionalInt32 RogueString_List__locate___Function_String_RETURNSLogical__Int32( RogueString_List* THIS, RogueClass_Function_String_RETURNSLogical_* query_0, RogueInt32 i1_1 )
{
  ROGUE_GC_CHECK;
  {
    ROGUE_DEF_LOCAL_REF(RogueString_List*,_auto_897_0,(THIS));
    RogueInt32 index_0 = (i1_1);
    RogueInt32 _auto_898_0 = (((_auto_897_0->count) - (1)));
    for (;ROGUE_COND(((index_0) <= (_auto_898_0)));++index_0)
    {
      ROGUE_GC_CHECK;
      ROGUE_DEF_LOCAL_REF(RogueString*,value_0,(((RogueString*)(_auto_897_0->data->as_objects[index_0]))));
      if (ROGUE_COND((Rogue_call_ROGUEM9( 13, query_0, value_0 ))))
      {
        return (RogueOptionalInt32)(RogueOptionalInt32( index_0, true ));
      }
    }
  }
  return (RogueOptionalInt32)((RogueOptionalInt32__create()));
}

void RogueString_List__quicksort___Function_String_String_RETURNSLogical_( RogueString_List* THIS, RogueClass_Function_String_String_RETURNSLogical_* compare_fn_0 )
{
  ROGUE_GC_CHECK;
  RogueQuicksort_String___sort__String_List__Function_String_String_RETURNSLogical_( ROGUE_ARG(THIS), compare_fn_0 );
}

RogueString_List* RogueString_List__reserve__Int32( RogueString_List* THIS, RogueInt32 additional_elements_0 )
{
  ROGUE_GC_CHECK;
  RogueInt32 required_capacity_1 = (((THIS->count) + (additional_elements_0)));
  if (ROGUE_COND(((required_capacity_1) == (0))))
  {
    return (RogueString_List*)(THIS);
  }
  if (ROGUE_COND(!(!!(THIS->data))))
  {
    if (ROGUE_COND(((required_capacity_1) == (1))))
    {
      required_capacity_1 = ((RogueInt32)(10));
    }
    THIS->data = RogueType_create_array( required_capacity_1, sizeof(RogueString*), true, 17 );
  }
  else if (ROGUE_COND(((required_capacity_1) > (THIS->data->count))))
  {
    RogueInt32 cap_2 = (((RogueString_List__capacity( ROGUE_ARG(THIS) ))));
    if (ROGUE_COND(((required_capacity_1) < (((cap_2) + (cap_2))))))
    {
      required_capacity_1 = ((RogueInt32)(((cap_2) + (cap_2))));
    }
    ROGUE_DEF_LOCAL_REF(RogueArray*,new_data_3,(RogueType_create_array( required_capacity_1, sizeof(RogueString*), true, 17 )));
    RogueArray_set(new_data_3,0,((RogueArray*)(THIS->data)),0,-1);
    THIS->data = new_data_3;
  }
  return (RogueString_List*)(THIS);
}

RogueString* RogueString_List__remove_at__Int32( RogueString_List* THIS, RogueInt32 index_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((RogueLogical)((((unsigned int)index_0) >= (unsigned int)THIS->count)))))
  {
    throw ((RogueException*)(RogueOutOfBoundsError___throw( ROGUE_ARG(((RogueClassOutOfBoundsError*)(RogueOutOfBoundsError__init__Int32_Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassOutOfBoundsError*,ROGUE_CREATE_OBJECT(OutOfBoundsError))), index_0, ROGUE_ARG(THIS->count) )))) )));
  }
  ROGUE_DEF_LOCAL_REF(RogueString*,result_1,(((RogueString*)(THIS->data->as_objects[index_0]))));
  RogueArray_set(THIS->data,index_0,((RogueArray*)(THIS->data)),((index_0) + (1)),-1);
  ROGUE_DEF_LOCAL_REF(RogueString*,zero_value_2,0);
  THIS->count = ((THIS->count) + (-1));
  THIS->data->as_objects[THIS->count] = zero_value_2;
  return (RogueString*)(result_1);
}

RogueString* RogueString_List__remove_first( RogueString_List* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(RogueString_List__remove_at__Int32( ROGUE_ARG(THIS), 0 ))));
}

RogueString* RogueString_List__remove_last( RogueString_List* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(RogueString_List__remove_at__Int32( ROGUE_ARG(THIS), ROGUE_ARG(((THIS->count) - (1))) ))));
}

RogueClassListRewriter_String_* RogueString_List__rewriter( RogueString_List* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueClassListRewriter_String_*)((RogueListRewriter_String___acquire__String_List( ROGUE_ARG(THIS) )));
}

void RogueString_List__set__Int32_String( RogueString_List* THIS, RogueInt32 index_0, RogueString* new_value_1 )
{
  ROGUE_GC_CHECK;
  THIS->data->as_objects[index_0] = new_value_1;
}

void RogueString_List__shift__Int32_OptionalInt32_Int32_OptionalString( RogueString_List* THIS, RogueInt32 i1_0, RogueOptionalInt32 element_count_1, RogueInt32 delta_2, RogueOptionalString fill_3 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((delta_2) == (0))))
  {
    return;
  }
  RogueInt32 n_4 = 0;
  if (ROGUE_COND(element_count_1.exists))
  {
    n_4 = ((RogueInt32)(element_count_1.value));
  }
  else
  {
    n_4 = ((RogueInt32)(((THIS->count) - (i1_0))));
  }
  RogueInt32 dest_i2_5 = (((((((i1_0) + (delta_2))) + (n_4))) - (1)));
  RogueString_List__expand_to_include__Int32( ROGUE_ARG(THIS), dest_i2_5 );
  RogueArray_set(THIS->data,((i1_0) + (delta_2)),((RogueArray*)(THIS->data)),i1_0,n_4);
  if (ROGUE_COND(fill_3.exists))
  {
    ROGUE_DEF_LOCAL_REF(RogueString*,value_6,(fill_3.value));
    if (ROGUE_COND(((delta_2) > (0))))
    {
      {
        RogueInt32 i_7 = (i1_0);
        RogueInt32 _auto_187_8 = (((((i1_0) + (delta_2))) - (1)));
        for (;ROGUE_COND(((i_7) <= (_auto_187_8)));++i_7)
        {
          ROGUE_GC_CHECK;
          THIS->data->as_objects[i_7] = value_6;
        }
      }
    }
    else
    {
      {
        RogueInt32 i_9 = (((((i1_0) + (delta_2))) + (n_4)));
        RogueInt32 _auto_188_10 = (((((i1_0) + (n_4))) - (1)));
        for (;ROGUE_COND(((i_9) <= (_auto_188_10)));++i_9)
        {
          ROGUE_GC_CHECK;
          THIS->data->as_objects[i_9] = value_6;
        }
      }
    }
  }
}

void RogueString_List__sort___Function_String_String_RETURNSLogical_( RogueString_List* THIS, RogueClass_Function_String_String_RETURNSLogical_* compare_fn_0 )
{
  ROGUE_GC_CHECK;
  RogueString_List__quicksort___Function_String_String_RETURNSLogical_( ROGUE_ARG(THIS), compare_fn_0 );
}

void RogueString_List__swap__Int32_Int32( RogueString_List* THIS, RogueInt32 i1_0, RogueInt32 i2_1 )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueString*,temp_2,(((RogueString*)(THIS->data->as_objects[i1_0]))));
  THIS->data->as_objects[i1_0] = ((RogueString*)(THIS->data->as_objects[i2_1]));
  THIS->data->as_objects[i2_1] = temp_2;
}

RogueString* RogueArray_String___type_name( RogueArray* THIS )
{
  return (RogueString*)(Rogue_literal_strings[386]);
}

RogueInt32 RogueReal64__decimal_digit_count( RogueReal64 THIS )
{
  ROGUE_GC_CHECK;
  {
    ROGUE_DEF_LOCAL_REF(RogueException*,_auto_763_0,0);
    ROGUE_DEF_LOCAL_REF(RogueClassStringBuilderPool*,_auto_762_0,(RogueStringBuilder_pool));
    RogueInt32 _auto_764_0 = 0;
    ROGUE_DEF_LOCAL_REF(RogueStringBuilder*,buffer_0,(((RogueStringBuilder*)(RogueStringBuilderPool__on_use( _auto_762_0 )))));
    Rogue_ignore_unused(buffer_0);

    ROGUE_TRY
    {
      RogueStringBuilder__print__Real64( buffer_0, ROGUE_ARG(THIS) );
      RogueOptionalInt32 dot_0 = (((RogueStringBuilder__locate__Character( buffer_0, (RogueCharacter)'.' ))));
      if (ROGUE_COND(!(dot_0.exists)))
      {
        {
          _auto_764_0 = ((RogueInt32)(0));
          goto _auto_766;
        }
      }
      RogueInt32 start_1 = (((dot_0.value) + (1)));
      RogueInt32 count_2 = (buffer_0->count);
      if (ROGUE_COND(((((count_2) == (((start_1) + (1))))) && (((((RogueStringBuilder__get__Int32( buffer_0, start_1 )))) == ((RogueCharacter)'0'))))))
      {
        {
          _auto_764_0 = ((RogueInt32)(0));
          goto _auto_766;
        }
      }
      {
        _auto_764_0 = ((RogueInt32)(((count_2) - (start_1))));
        goto _auto_766;
      }
    }
    ROGUE_CATCH( RogueException,_auto_765_0 )
    {
      _auto_763_0 = ((RogueException*)(_auto_765_0));
    }
    ROGUE_END_TRY
    _auto_766:;
    RogueStringBuilderPool__on_end_use__StringBuilder( _auto_762_0, buffer_0 );
    if (ROGUE_COND(!!(_auto_763_0)))
    {
      throw ((RogueException*)Rogue_call_ROGUEM0( 16, _auto_763_0 ));
    }
    return (RogueInt32)(_auto_764_0);
  }
}

RogueString* RogueReal64__format__OptionalInt32( RogueReal64 THIS, RogueOptionalInt32 decimal_digits_0 )
{
  ROGUE_GC_CHECK;
  RogueOptionalInt32 _auto_192_1 = (decimal_digits_0);
  RogueInt32 digits_2 = (((((_auto_192_1.exists))) ? (_auto_192_1.value) : ((RogueReal64__decimal_digit_count( ROGUE_ARG(THIS) )))));
  return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__Real64_Int32( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), ROGUE_ARG(THIS), digits_2 )))) ))));
}

RogueReal64 RogueReal64__fractional_part( RogueReal64 THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((THIS) >= (0.0))))
  {
    return (RogueReal64)(((THIS) - (((RogueReal64__whole_part( ROGUE_ARG(THIS) ))))));
  }
  else
  {
    return (RogueReal64)(((((RogueReal64__whole_part( ROGUE_ARG(THIS) )))) - (THIS)));
  }
}

RogueLogical RogueReal64__is_infinite( RogueReal64 THIS )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(((((THIS) == (THIS))) && (((((THIS) - (THIS))) != (0.0)))));
}

RogueLogical RogueReal64__is_not_a_number( RogueReal64 THIS )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(((THIS) != (THIS)));
}

RogueReal64 RogueReal64__whole_part( RogueReal64 THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((THIS) >= (0.0))))
  {
    return (RogueReal64)(floor((double)THIS));
  }
  else
  {
    return (RogueReal64)(ceil((double)THIS));
  }
}

RogueClassPrintWriter* RogueInt64__print_in_power2_base__Int32_Int32_PrintWriter( RogueInt64 THIS, RogueInt32 base_0, RogueInt32 digits_1, RogueClassPrintWriter* buffer_2 )
{
  ROGUE_GC_CHECK;
  RogueInt32 bits_3 = (0);
  RogueInt32 temp_4 = (base_0);
  while (ROGUE_COND(((temp_4) > (1))))
  {
    ROGUE_GC_CHECK;
    ++bits_3;
    temp_4 = ((RogueInt32)(((temp_4) >> (1))));
  }
  RogueInt64 remaining_5 = ((RogueMath__shift_right__Int64_Int64( ROGUE_ARG(THIS), ROGUE_ARG(((RogueInt64)(bits_3))) )));
  if (ROGUE_COND(((((digits_1) > (1))) || (!!(remaining_5)))))
  {
    RogueInt64__print_in_power2_base__Int32_Int32_PrintWriter( remaining_5, base_0, ROGUE_ARG(((digits_1) - (1))), ((buffer_2)) );
  }
  RoguePrintWriter__print__Character( ((((RogueObject*)buffer_2))), ROGUE_ARG(((RogueInt32__to_digit__Logical( ROGUE_ARG(((RogueInt32)(((THIS) & (((RogueInt64)(((base_0) - (1))))))))), false )))) );
  return (RogueClassPrintWriter*)(buffer_2);
}

RogueString* RogueInt64__to_hex_string__Int32( RogueInt64 THIS, RogueInt32 digits_0 )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)Rogue_call_ROGUEM0( 8, ROGUE_ARG(((((RogueObject*)((RogueClassPrintWriter*)(RogueInt64__print_in_power2_base__Int32_Int32_PrintWriter( ROGUE_ARG(THIS), 16, digits_0, ROGUE_ARG(((((RogueClassPrintWriter*)(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))))))) ))))))) )));
}

RogueLogical RogueCharacter__is_alphanumeric( RogueCharacter THIS )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(((((((((THIS) >= ((RogueCharacter)'0'))) && (((THIS) <= ((RogueCharacter)'9'))))) || (((((THIS) >= ((RogueCharacter)'a'))) && (((THIS) <= ((RogueCharacter)'z'))))))) || (((((THIS) >= ((RogueCharacter)'A'))) && (((THIS) <= ((RogueCharacter)'Z')))))));
}

RogueLogical RogueCharacter__is_identifier__Logical_Logical( RogueCharacter THIS, RogueLogical start_0, RogueLogical allow_dollar_1 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((((((((THIS) >= ((RogueCharacter)'a'))) && (((THIS) <= ((RogueCharacter)'z'))))) || (((((THIS) >= ((RogueCharacter)'A'))) && (((THIS) <= ((RogueCharacter)'Z'))))))) || (((THIS) == ((RogueCharacter)'_'))))))
  {
    return (RogueLogical)(true);
  }
  if (ROGUE_COND(((!(start_0)) && (((((THIS) >= ((RogueCharacter)'0'))) && (((THIS) <= ((RogueCharacter)'9'))))))))
  {
    return (RogueLogical)(true);
  }
  if (ROGUE_COND(((allow_dollar_1) && (((THIS) == ((RogueCharacter)'$'))))))
  {
    return (RogueLogical)(true);
  }
  return (RogueLogical)(false);
}

RogueLogical RogueCharacter__is_letter( RogueCharacter THIS )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(((((((THIS) >= ((RogueCharacter)'a'))) && (((THIS) <= ((RogueCharacter)'z'))))) || (((((THIS) >= ((RogueCharacter)'A'))) && (((THIS) <= ((RogueCharacter)'Z')))))));
}

RogueLogical RogueCharacter__is_number__Int32( RogueCharacter THIS, RogueInt32 base_0 )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(((((RogueCharacter__to_number__Int32( ROGUE_ARG(THIS), base_0 )))) != (-1)));
}

RogueLogical RogueCharacter__is_uppercase( RogueCharacter THIS )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(((((THIS) >= ((RogueCharacter)'A'))) && (((THIS) <= ((RogueCharacter)'Z')))));
}

void RogueCharacter__print_escaped_ascii__PrintWriter_String( RogueCharacter THIS, RogueClassPrintWriter* writer_0, RogueString* additional_characters_to_escape_1 )
{
  ROGUE_GC_CHECK;
  switch (THIS)
  {
    case (RogueCharacter)'\\':
    {
      RoguePrintWriter__print__String( ((((RogueObject*)writer_0))), Rogue_literal_strings[248] );
      break;
    }
    case (RogueCharacter)0:
    {
      RoguePrintWriter__print__String( ((((RogueObject*)writer_0))), Rogue_literal_strings[249] );
      break;
    }
    case (RogueCharacter)8:
    {
      RoguePrintWriter__print__String( ((((RogueObject*)writer_0))), Rogue_literal_strings[250] );
      break;
    }
    case (RogueCharacter)27:
    {
      RoguePrintWriter__print__String( ((((RogueObject*)writer_0))), Rogue_literal_strings[251] );
      break;
    }
    case (RogueCharacter)12:
    {
      RoguePrintWriter__print__String( ((((RogueObject*)writer_0))), Rogue_literal_strings[252] );
      break;
    }
    case (RogueCharacter)10:
    {
      RoguePrintWriter__print__String( ((((RogueObject*)writer_0))), Rogue_literal_strings[253] );
      break;
    }
    case (RogueCharacter)13:
    {
      RoguePrintWriter__print__String( ((((RogueObject*)writer_0))), Rogue_literal_strings[254] );
      break;
    }
    case (RogueCharacter)11:
    {
      RoguePrintWriter__print__String( ((((RogueObject*)writer_0))), Rogue_literal_strings[255] );
      break;
    }
    default:
    {
      if (ROGUE_COND(((RogueString__contains__Character_Logical( additional_characters_to_escape_1, ROGUE_ARG(THIS), false )))))
      {
        RoguePrintWriter__print__Character( ROGUE_ARG(((((RogueObject*)(RoguePrintWriter__print__String( ((((RogueObject*)writer_0))), Rogue_literal_strings[256] )))))), ROGUE_ARG(THIS) );
      }
      else if (ROGUE_COND(((((RogueInt32)(THIS))) >= (256))))
      {
        RoguePrintWriter__print__String( ((((RogueObject*)writer_0))), Rogue_literal_strings[257] );
        RogueInt64__print_in_power2_base__Int32_Int32_PrintWriter( ROGUE_ARG(((RogueInt64)(THIS))), 16, 3, ((writer_0)) );
        RoguePrintWriter__print__Character( ((((RogueObject*)writer_0))), (RogueCharacter)']' );
      }
      else if (ROGUE_COND(((((((RogueInt32)(THIS))) < (32))) || (((((RogueInt32)(THIS))) >= (127))))))
      {
        RoguePrintWriter__print__String( ((((RogueObject*)writer_0))), Rogue_literal_strings[258] );
        RogueInt64__print_in_power2_base__Int32_Int32_PrintWriter( ROGUE_ARG(((RogueInt64)(THIS))), 16, 2, ((writer_0)) );
      }
      else
      {
        RoguePrintWriter__print__Character( ((((RogueObject*)writer_0))), ROGUE_ARG(THIS) );
      }
    }
  }
}

RogueString* RogueCharacter__to_String( RogueCharacter THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(RogueString__operatorPLUS__Character( ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], Rogue_literal_strings[0] ))), ROGUE_ARG(THIS) ))));
}

RogueInt32 RogueCharacter__to_number__Int32( RogueCharacter THIS, RogueInt32 base_0 )
{
  ROGUE_GC_CHECK;
  RogueInt32 value_1 = 0;
  if (ROGUE_COND(((((THIS) >= ((RogueCharacter)'0'))) && (((THIS) <= ((RogueCharacter)'9'))))))
  {
    value_1 = ((RogueInt32)(((RogueInt32)(((THIS) - ((RogueCharacter)'0'))))));
  }
  else if (ROGUE_COND(((((THIS) >= ((RogueCharacter)'A'))) && (((THIS) <= ((RogueCharacter)'Z'))))))
  {
    value_1 = ((RogueInt32)(((10) + (((RogueInt32)(((THIS) - ((RogueCharacter)'A'))))))));
  }
  else if (ROGUE_COND(((((THIS) >= ((RogueCharacter)'a'))) && (((THIS) <= ((RogueCharacter)'z'))))))
  {
    value_1 = ((RogueInt32)(((10) + (((RogueInt32)(((THIS) - ((RogueCharacter)'a'))))))));
  }
  else
  {
    return (RogueInt32)(-1);
  }
  if (ROGUE_COND(((value_1) < (base_0))))
  {
    return (RogueInt32)(value_1);
  }
  else
  {
    return (RogueInt32)(-1);
  }
}

RogueCharacter RogueCharacter__to_lowercase( RogueCharacter THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(((RogueCharacter__is_uppercase( ROGUE_ARG(THIS) ))))))
  {
    return (RogueCharacter)(THIS);
  }
  return (RogueCharacter)((RogueCharacter__create__Character( ROGUE_ARG(((THIS) + ((((RogueCharacter)'a') - ((RogueCharacter)'A'))))) )));
}

RogueClassValue* RogueValue__init_object( RogueClassValue* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  return (RogueClassValue*)(THIS);
}

RogueString* RogueValue__type_name( RogueClassValue* THIS )
{
  return (RogueString*)(Rogue_literal_strings[325]);
}

RogueClassValue* RogueValue__at__Int32( RogueClassValue* THIS, RogueInt32 index_0 )
{
  ROGUE_GC_CHECK;
  return (RogueClassValue*)(((RogueClassValue*)Rogue_call_ROGUEM3( 40, ROGUE_ARG(THIS), index_0 )));
}

RogueInt32 RogueValue__count( RogueClassValue* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueInt32)(0);
}

RogueClassValue* RogueValue__first___Function_Value_RETURNSLogical_( RogueClassValue* THIS, RogueClass_Function_Value_RETURNSLogical_* query_0 )
{
  ROGUE_GC_CHECK;
  return (RogueClassValue*)(((RogueClassValue*)(((RogueClassUndefinedValue*)ROGUE_SINGLETON(UndefinedValue)))));
}

RogueClassValue* RogueValue__get__Int32( RogueClassValue* THIS, RogueInt32 index_0 )
{
  ROGUE_GC_CHECK;
  return (RogueClassValue*)(((RogueClassValue*)(((RogueClassUndefinedValue*)ROGUE_SINGLETON(UndefinedValue)))));
}

RogueClassValue* RogueValue__get__String( RogueClassValue* THIS, RogueString* key_0 )
{
  ROGUE_GC_CHECK;
  return (RogueClassValue*)(((RogueClassValue*)(((RogueClassUndefinedValue*)ROGUE_SINGLETON(UndefinedValue)))));
}

RogueLogical RogueValue__is_collection( RogueClassValue* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(false);
}

RogueLogical RogueValue__is_complex( RogueClassValue* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!((Rogue_call_ROGUEM5( 52, ROGUE_ARG(THIS) )))))
  {
    return (RogueLogical)(false);
  }
  if (ROGUE_COND((((Rogue_call_ROGUEM2( 29, ROGUE_ARG(THIS) ))) > (1))))
  {
    return (RogueLogical)(true);
  }
  {
    ROGUE_DEF_LOCAL_REF(RogueClassValue*,_auto_501_0,(THIS));
    RogueInt32 _auto_502_0 = (0);
    RogueInt32 _auto_503_0 = ((((Rogue_call_ROGUEM2( 29, _auto_501_0 ))) - (1)));
    for (;ROGUE_COND(((_auto_502_0) <= (_auto_503_0)));++_auto_502_0)
    {
      ROGUE_GC_CHECK;
      ROGUE_DEF_LOCAL_REF(RogueClassValue*,value_0,(((RogueClassValue*)Rogue_call_ROGUEM3( 22, _auto_501_0, _auto_502_0 ))));
      if (ROGUE_COND((((RogueOptionalValue__operator__Value( value_0 ))) && (((RogueValue__is_complex( value_0 )))))))
      {
        return (RogueLogical)(true);
      }
    }
  }
  return (RogueLogical)(false);
}

RogueLogical RogueValue__is_list( RogueClassValue* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(false);
}

RogueLogical RogueValue__is_logical( RogueClassValue* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(false);
}

RogueLogical RogueValue__is_non_null( RogueClassValue* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(true);
}

RogueClassValue* RogueValue__set__String_Value( RogueClassValue* THIS, RogueString* key_0, RogueClassValue* value_1 )
{
  ROGUE_GC_CHECK;
  return (RogueClassValue*)(THIS);
}

RogueInt64 RogueValue__to_Int64( RogueClassValue* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueInt64)(0LL);
}

RogueInt32 RogueValue__to_Int32( RogueClassValue* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueInt32)(((RogueInt32)((Rogue_call_ROGUEM7( 149, ROGUE_ARG(THIS) )))));
}

RogueLogical RogueValue__to_Logical( RogueClassValue* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)((RogueLogical__create__Int32( ROGUE_ARG((Rogue_call_ROGUEM2( 150, ROGUE_ARG(THIS) ))) )));
}

RogueString* RogueValue__to_json__Logical_Logical( RogueClassValue* THIS, RogueLogical formatted_0, RogueLogical omit_commas_1 )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueValue__to_json__StringBuilder_Logical_Logical( ROGUE_ARG(THIS), ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), formatted_0, omit_commas_1 )))) ))));
}

RogueStringBuilder* RogueValue__to_json__StringBuilder_Logical_Logical( RogueClassValue* THIS, RogueStringBuilder* buffer_0, RogueLogical formatted_1, RogueLogical omit_commas_2 )
{
  ROGUE_GC_CHECK;
  RogueInt32 flags_3 = (0);
  if (ROGUE_COND(formatted_1))
  {
    flags_3 |= 1;
  }
  if (ROGUE_COND(omit_commas_2))
  {
    flags_3 |= 3;
  }
  return (RogueStringBuilder*)(((RogueStringBuilder*)Rogue_call_ROGUEM8( 159, ROGUE_ARG(THIS), buffer_0, flags_3 )));
}

RogueStringBuilder* RogueValue__to_json__StringBuilder_Int32( RogueClassValue* THIS, RogueStringBuilder* buffer_0, RogueInt32 flags_1 )
{
  ROGUE_GC_CHECK;
  return (RogueStringBuilder*)(buffer_0);
}

RogueString_List* RogueValue__to_list_String_( RogueClassValue* THIS )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueString_List*,result_0,(((RogueString_List*)(RogueString_List__init__Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueString_List*,ROGUE_CREATE_OBJECT(String_List))), ROGUE_ARG((Rogue_call_ROGUEM2( 29, ROGUE_ARG(THIS) ))) )))));
  {
    ROGUE_DEF_LOCAL_REF(RogueClassValue*,_auto_996_0,(THIS));
    RogueInt32 _auto_997_0 = (0);
    RogueInt32 _auto_998_0 = ((((Rogue_call_ROGUEM2( 29, _auto_996_0 ))) - (1)));
    for (;ROGUE_COND(((_auto_997_0) <= (_auto_998_0)));++_auto_997_0)
    {
      ROGUE_GC_CHECK;
      ROGUE_DEF_LOCAL_REF(RogueClassValue*,value_0,(((RogueClassValue*)Rogue_call_ROGUEM3( 22, _auto_996_0, _auto_997_0 ))));
      RogueString_List__add__String( result_0, ROGUE_ARG(((RogueString*)Rogue_call_ROGUEM0( 8, ((RogueObject*)value_0) ))) );
    }
  }
  return (RogueString_List*)(result_0);
}

RogueCharacter_List* RogueCharacter_List__init_object( RogueCharacter_List* THIS )
{
  ROGUE_GC_CHECK;
  RogueGenericList__init_object( ROGUE_ARG(((RogueClassGenericList*)THIS)) );
  return (RogueCharacter_List*)(THIS);
}

RogueCharacter_List* RogueCharacter_List__init( RogueCharacter_List* THIS )
{
  ROGUE_GC_CHECK;
  RogueCharacter_List__init__Int32( ROGUE_ARG(THIS), 0 );
  return (RogueCharacter_List*)(THIS);
}

RogueString* RogueCharacter_List__to_String( RogueCharacter_List* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[293] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(((RogueString*)(RogueCharacter_List__join__String( ROGUE_ARG(THIS), Rogue_literal_strings[15] )))) ))) )))), Rogue_literal_strings[294] )))) ))));
}

RogueString* RogueCharacter_List__type_name( RogueCharacter_List* THIS )
{
  return (RogueString*)(Rogue_literal_strings[375]);
}

RogueCharacter_List* RogueCharacter_List__init__Int32( RogueCharacter_List* THIS, RogueInt32 initial_capacity_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((initial_capacity_0) != (((RogueCharacter_List__capacity( ROGUE_ARG(THIS) )))))))
  {
    THIS->data = RogueType_create_array( initial_capacity_0, sizeof(RogueCharacter), false, 23 );
  }
  RogueCharacter_List__clear( ROGUE_ARG(THIS) );
  return (RogueCharacter_List*)(THIS);
}

void RogueCharacter_List__add__Character( RogueCharacter_List* THIS, RogueCharacter value_0 )
{
  ROGUE_GC_CHECK;
  RogueCharacter_List__set__Int32_Character( ROGUE_ARG(((RogueCharacter_List*)(RogueCharacter_List__reserve__Int32( ROGUE_ARG(THIS), 1 )))), ROGUE_ARG(THIS->count), value_0 );
  THIS->count = ((THIS->count) + (1));
}

RogueInt32 RogueCharacter_List__capacity( RogueCharacter_List* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(!!(THIS->data))))
  {
    return (RogueInt32)(0);
  }
  return (RogueInt32)(THIS->data->count);
}

void RogueCharacter_List__clear( RogueCharacter_List* THIS )
{
  ROGUE_GC_CHECK;
  RogueCharacter_List__discard_from__Int32( ROGUE_ARG(THIS), 0 );
}

void RogueCharacter_List__discard_from__Int32( RogueCharacter_List* THIS, RogueInt32 index_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!!(THIS->data)))
  {
    RogueInt32 n_1 = (((THIS->count) - (index_0)));
    if (ROGUE_COND(((n_1) > (0))))
    {
      RogueArray__zero__Int32_Int32( ROGUE_ARG(((RogueArray*)THIS->data)), index_0, n_1 );
      THIS->count = index_0;
    }
  }
}

RogueCharacter RogueCharacter_List__get__Int32( RogueCharacter_List* THIS, RogueInt32 index_0 )
{
  return (RogueCharacter)(THIS->data->as_characters[index_0]);
}

RogueString* RogueCharacter_List__join__String( RogueCharacter_List* THIS, RogueString* separator_0 )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueStringBuilder*,builder_1,(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))));
  RogueLogical first_2 = (true);
  {
    ROGUE_DEF_LOCAL_REF(RogueCharacter_List*,_auto_1851_0,(THIS));
    RogueInt32 _auto_1852_0 = (0);
    RogueInt32 _auto_1853_0 = (((_auto_1851_0->count) - (1)));
    for (;ROGUE_COND(((_auto_1852_0) <= (_auto_1853_0)));++_auto_1852_0)
    {
      ROGUE_GC_CHECK;
      RogueCharacter item_0 = (_auto_1851_0->data->as_characters[_auto_1852_0]);
      if (ROGUE_COND(first_2))
      {
        first_2 = ((RogueLogical)(false));
      }
      else
      {
        RogueStringBuilder__print__String( builder_1, separator_0 );
      }
      RogueStringBuilder__print__Character( builder_1, item_0 );
    }
  }
  return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( builder_1 ))));
}

RogueCharacter_List* RogueCharacter_List__reserve__Int32( RogueCharacter_List* THIS, RogueInt32 additional_elements_0 )
{
  ROGUE_GC_CHECK;
  RogueInt32 required_capacity_1 = (((THIS->count) + (additional_elements_0)));
  if (ROGUE_COND(((required_capacity_1) == (0))))
  {
    return (RogueCharacter_List*)(THIS);
  }
  if (ROGUE_COND(!(!!(THIS->data))))
  {
    if (ROGUE_COND(((required_capacity_1) == (1))))
    {
      required_capacity_1 = ((RogueInt32)(10));
    }
    THIS->data = RogueType_create_array( required_capacity_1, sizeof(RogueCharacter), false, 23 );
  }
  else if (ROGUE_COND(((required_capacity_1) > (THIS->data->count))))
  {
    RogueInt32 cap_2 = (((RogueCharacter_List__capacity( ROGUE_ARG(THIS) ))));
    if (ROGUE_COND(((required_capacity_1) < (((cap_2) + (cap_2))))))
    {
      required_capacity_1 = ((RogueInt32)(((cap_2) + (cap_2))));
    }
    ROGUE_DEF_LOCAL_REF(RogueArray*,new_data_3,(RogueType_create_array( required_capacity_1, sizeof(RogueCharacter), false, 23 )));
    RogueArray_set(new_data_3,0,((RogueArray*)(THIS->data)),0,-1);
    THIS->data = new_data_3;
  }
  return (RogueCharacter_List*)(THIS);
}

void RogueCharacter_List__set__Int32_Character( RogueCharacter_List* THIS, RogueInt32 index_0, RogueCharacter new_value_1 )
{
  ROGUE_GC_CHECK;
  THIS->data->as_characters[index_0] = (new_value_1);
}

RogueString* RogueArray_Character___type_name( RogueArray* THIS )
{
  return (RogueString*)(Rogue_literal_strings[389]);
}

RogueClass_Function_Value_RETURNSLogical_* Rogue_Function_Value_RETURNSLogical___init_object( RogueClass_Function_Value_RETURNSLogical_* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  return (RogueClass_Function_Value_RETURNSLogical_*)(THIS);
}

RogueString* Rogue_Function_Value_RETURNSLogical___type_name( RogueClass_Function_Value_RETURNSLogical_* THIS )
{
  return (RogueString*)(Rogue_literal_strings[330]);
}

RogueLogical Rogue_Function_Value_RETURNSLogical___call__Value( RogueClass_Function_Value_RETURNSLogical_* THIS, RogueClassValue* param1_0 )
{
  return (RogueLogical)(false);
}

RogueClassFile* RogueFile__init_object( RogueClassFile* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  return (RogueClassFile*)(THIS);
}

RogueString* RogueFile__to_String( RogueClassFile* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(THIS->filepath);
}

RogueString* RogueFile__type_name( RogueClassFile* THIS )
{
  return (RogueString*)(Rogue_literal_strings[331]);
}

RogueClassFile* RogueFile__init__String( RogueClassFile* THIS, RogueString* filepath_0 )
{
  ROGUE_GC_CHECK;
  THIS->filepath = (RogueFile__fix_slashes__String( filepath_0 ));
  THIS->filepath = (RogueFile__expand_path__String( ROGUE_ARG(THIS->filepath) ));
  return (RogueClassFile*)(THIS);
}

RogueString* RogueFile__absolute_filepath( RogueClassFile* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)((RogueFile__absolute_filepath__String( ROGUE_ARG(THIS->filepath) )));
}

RogueLogical RogueFile__create_folder( RogueClassFile* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)((RogueFile__create_folder__String( ROGUE_ARG(THIS->filepath) )));
}

RogueString* RogueFile__expand_path( RogueClassFile* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)((RogueFile__expand_path__String( ROGUE_ARG(THIS->filepath) )));
}

RogueString* RogueFile__folder( RogueClassFile* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)((RogueFile__folder__String( ROGUE_ARG(THIS->filepath) )));
}

RogueLogical RogueFile__is_folder( RogueClassFile* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)((RogueFile__is_folder__String( ROGUE_ARG(THIS->filepath) )));
}

RogueString* RogueFile__load_as_string__StringEncoding( RogueClassFile* THIS, RogueClassStringEncoding encoding_0 )
{
  ROGUE_GC_CHECK;
  return (RogueString*)((RogueFile__load_as_string__String_StringEncoding( ROGUE_ARG(THIS->filepath), encoding_0 )));
}

RogueClassFile* RogueFile__operatorSLASH__String( RogueClassFile* THIS, RogueString* path_segment_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND((RogueString__operatorEQUALSEQUALS__String_String( path_segment_0, Rogue_literal_strings[31] ))))
  {
    if (ROGUE_COND(((RogueString__contains__Character_Logical( ROGUE_ARG(THIS->filepath), (RogueCharacter)'/', false )))))
    {
      return (RogueClassFile*)(((RogueClassFile*)(RogueFile__init__String( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassFile*,ROGUE_CREATE_OBJECT(File))), ROGUE_ARG((RogueFile__path__String( ROGUE_ARG(THIS->filepath) ))) ))));
    }
    else
    {
      return (RogueClassFile*)(((RogueClassFile*)(RogueFile__init__String( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassFile*,ROGUE_CREATE_OBJECT(File))), ROGUE_ARG((RogueFile__path__String( ROGUE_ARG(((RogueString*)(RogueFile__absolute_filepath( ROGUE_ARG(THIS) )))) ))) ))));
    }
  }
  return (RogueClassFile*)(((RogueClassFile*)(RogueFile__init__String( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassFile*,ROGUE_CREATE_OBJECT(File))), ROGUE_ARG((RogueString__operatorSLASH__String_String( ROGUE_ARG(((RogueString*)(RogueFile__expand_path( ROGUE_ARG(THIS) )))), path_segment_0 ))) ))));
}

RogueClassFileReader* RogueFile__reader( RogueClassFile* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueClassFileReader*)((RogueFile__reader__String( ROGUE_ARG(THIS->filepath) )));
}

RogueLogical RogueFile__save__Byte_List( RogueClassFile* THIS, RogueByte_List* data_0 )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)((RogueFile__save__String_Byte_List( ROGUE_ARG(THIS->filepath), data_0 )));
}

RogueInt32_List* RogueInt32_List__init_object( RogueInt32_List* THIS )
{
  ROGUE_GC_CHECK;
  RogueGenericList__init_object( ROGUE_ARG(((RogueClassGenericList*)THIS)) );
  return (RogueInt32_List*)(THIS);
}

RogueInt32_List* RogueInt32_List__init( RogueInt32_List* THIS )
{
  ROGUE_GC_CHECK;
  RogueInt32_List__init__Int32( ROGUE_ARG(THIS), 0 );
  return (RogueInt32_List*)(THIS);
}

RogueString* RogueInt32_List__to_String( RogueInt32_List* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[293] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(((RogueString*)(RogueInt32_List__join__String( ROGUE_ARG(THIS), Rogue_literal_strings[15] )))) ))) )))), Rogue_literal_strings[294] )))) ))));
}

RogueString* RogueInt32_List__type_name( RogueInt32_List* THIS )
{
  return (RogueString*)(Rogue_literal_strings[382]);
}

RogueInt32_List* RogueInt32_List__init__Int32( RogueInt32_List* THIS, RogueInt32 initial_capacity_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((initial_capacity_0) != (((RogueInt32_List__capacity( ROGUE_ARG(THIS) )))))))
  {
    THIS->data = RogueType_create_array( initial_capacity_0, sizeof(RogueInt32), false, 9 );
  }
  RogueInt32_List__clear( ROGUE_ARG(THIS) );
  return (RogueInt32_List*)(THIS);
}

void RogueInt32_List__add__Int32( RogueInt32_List* THIS, RogueInt32 value_0 )
{
  ROGUE_GC_CHECK;
  RogueInt32_List__set__Int32_Int32( ROGUE_ARG(((RogueInt32_List*)(RogueInt32_List__reserve__Int32( ROGUE_ARG(THIS), 1 )))), ROGUE_ARG(THIS->count), value_0 );
  THIS->count = ((THIS->count) + (1));
}

RogueInt32 RogueInt32_List__capacity( RogueInt32_List* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(!!(THIS->data))))
  {
    return (RogueInt32)(0);
  }
  return (RogueInt32)(THIS->data->count);
}

void RogueInt32_List__clear( RogueInt32_List* THIS )
{
  ROGUE_GC_CHECK;
  RogueInt32_List__discard_from__Int32( ROGUE_ARG(THIS), 0 );
}

void RogueInt32_List__discard_from__Int32( RogueInt32_List* THIS, RogueInt32 index_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!!(THIS->data)))
  {
    RogueInt32 n_1 = (((THIS->count) - (index_0)));
    if (ROGUE_COND(((n_1) > (0))))
    {
      RogueArray__zero__Int32_Int32( ROGUE_ARG(((RogueArray*)THIS->data)), index_0, n_1 );
      THIS->count = index_0;
    }
  }
}

RogueInt32 RogueInt32_List__get__Int32( RogueInt32_List* THIS, RogueInt32 index_0 )
{
  return (RogueInt32)(THIS->data->as_int32s[index_0]);
}

RogueString* RogueInt32_List__join__String( RogueInt32_List* THIS, RogueString* separator_0 )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueStringBuilder*,builder_1,(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))));
  RogueLogical first_2 = (true);
  {
    ROGUE_DEF_LOCAL_REF(RogueInt32_List*,_auto_1996_0,(THIS));
    RogueInt32 _auto_1997_0 = (0);
    RogueInt32 _auto_1998_0 = (((_auto_1996_0->count) - (1)));
    for (;ROGUE_COND(((_auto_1997_0) <= (_auto_1998_0)));++_auto_1997_0)
    {
      ROGUE_GC_CHECK;
      RogueInt32 item_0 = (_auto_1996_0->data->as_int32s[_auto_1997_0]);
      if (ROGUE_COND(first_2))
      {
        first_2 = ((RogueLogical)(false));
      }
      else
      {
        RogueStringBuilder__print__String( builder_1, separator_0 );
      }
      RogueStringBuilder__print__Int32( builder_1, item_0 );
    }
  }
  return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( builder_1 ))));
}

RogueInt32_List* RogueInt32_List__reserve__Int32( RogueInt32_List* THIS, RogueInt32 additional_elements_0 )
{
  ROGUE_GC_CHECK;
  RogueInt32 required_capacity_1 = (((THIS->count) + (additional_elements_0)));
  if (ROGUE_COND(((required_capacity_1) == (0))))
  {
    return (RogueInt32_List*)(THIS);
  }
  if (ROGUE_COND(!(!!(THIS->data))))
  {
    if (ROGUE_COND(((required_capacity_1) == (1))))
    {
      required_capacity_1 = ((RogueInt32)(10));
    }
    THIS->data = RogueType_create_array( required_capacity_1, sizeof(RogueInt32), false, 9 );
  }
  else if (ROGUE_COND(((required_capacity_1) > (THIS->data->count))))
  {
    RogueInt32 cap_2 = (((RogueInt32_List__capacity( ROGUE_ARG(THIS) ))));
    if (ROGUE_COND(((required_capacity_1) < (((cap_2) + (cap_2))))))
    {
      required_capacity_1 = ((RogueInt32)(((cap_2) + (cap_2))));
    }
    ROGUE_DEF_LOCAL_REF(RogueArray*,new_data_3,(RogueType_create_array( required_capacity_1, sizeof(RogueInt32), false, 9 )));
    RogueArray_set(new_data_3,0,((RogueArray*)(THIS->data)),0,-1);
    THIS->data = new_data_3;
  }
  return (RogueInt32_List*)(THIS);
}

void RogueInt32_List__set__Int32_Int32( RogueInt32_List* THIS, RogueInt32 index_0, RogueInt32 new_value_1 )
{
  ROGUE_GC_CHECK;
  THIS->data->as_int32s[index_0] = (new_value_1);
}

RogueString* RogueArray_Int32___type_name( RogueArray* THIS )
{
  return (RogueString*)(Rogue_literal_strings[393]);
}

RogueClassRuntime* RogueRuntime__init_object( RogueClassRuntime* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  return (RogueClassRuntime*)(THIS);
}

RogueString* RogueRuntime__type_name( RogueClassRuntime* THIS )
{
  return (RogueString*)(Rogue_literal_strings[332]);
}

RogueClassUndefinedValue* RogueUndefinedValue__init_object( RogueClassUndefinedValue* THIS )
{
  ROGUE_GC_CHECK;
  RogueNullValue__init_object( ROGUE_ARG(((RogueClassNullValue*)THIS)) );
  return (RogueClassUndefinedValue*)(THIS);
}

RogueString* RogueUndefinedValue__to_String( RogueClassUndefinedValue* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(Rogue_literal_strings[0]);
}

RogueString* RogueUndefinedValue__type_name( RogueClassUndefinedValue* THIS )
{
  return (RogueString*)(Rogue_literal_strings[424]);
}

RogueClassNullValue* RogueNullValue__init_object( RogueClassNullValue* THIS )
{
  ROGUE_GC_CHECK;
  RogueValue__init_object( ROGUE_ARG(((RogueClassValue*)THIS)) );
  return (RogueClassNullValue*)(THIS);
}

RogueString* RogueNullValue__to_String( RogueClassNullValue* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(Rogue_literal_strings[6]);
}

RogueString* RogueNullValue__type_name( RogueClassNullValue* THIS )
{
  return (RogueString*)(Rogue_literal_strings[403]);
}

RogueLogical RogueNullValue__is_non_null( RogueClassNullValue* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(false);
}

RogueInt64 RogueNullValue__to_Int64( RogueClassNullValue* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueInt64)(0LL);
}

RogueStringBuilder* RogueNullValue__to_json__StringBuilder_Int32( RogueClassNullValue* THIS, RogueStringBuilder* buffer_0, RogueInt32 flags_1 )
{
  ROGUE_GC_CHECK;
  return (RogueStringBuilder*)(((RogueStringBuilder*)(RogueStringBuilder__print__String( buffer_0, Rogue_literal_strings[6] ))));
}

RogueClassValueTable* RogueValueTable__init_object( RogueClassValueTable* THIS )
{
  ROGUE_GC_CHECK;
  RogueValue__init_object( ROGUE_ARG(((RogueClassValue*)THIS)) );
  return (RogueClassValueTable*)(THIS);
}

RogueClassValueTable* RogueValueTable__init( RogueClassValueTable* THIS )
{
  ROGUE_GC_CHECK;
  THIS->data = ((RogueClassTable_String_Value_*)(RogueTable_String_Value___init( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassTable_String_Value_*,ROGUE_CREATE_OBJECT(Table_String_Value_))) )));
  return (RogueClassValueTable*)(THIS);
}

RogueString* RogueValueTable__to_String( RogueClassValueTable* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(RogueTable_String_Value___to_String( ROGUE_ARG(THIS->data) ))));
}

RogueString* RogueValueTable__type_name( RogueClassValueTable* THIS )
{
  return (RogueString*)(Rogue_literal_strings[404]);
}

RogueClassValue* RogueValueTable__at__Int32( RogueClassValueTable* THIS, RogueInt32 index_0 )
{
  ROGUE_GC_CHECK;
  return (RogueClassValue*)(((RogueClassValue*)(RogueTable_String_Value___at__Int32( ROGUE_ARG(THIS->data), index_0 ))));
}

RogueInt32 RogueValueTable__count( RogueClassValueTable* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueInt32)(THIS->data->count);
}

RogueClassValue* RogueValueTable__first___Function_Value_RETURNSLogical_( RogueClassValueTable* THIS, RogueClass_Function_Value_RETURNSLogical_* query_0 )
{
  ROGUE_GC_CHECK;
  {
    RogueClassTableKeysIterator_String_Value_ _auto_2109_0 = (((RogueValueTable__keys( ROGUE_ARG(THIS) ))));
    RogueOptionalString _auto_2110_0 = (((RogueTableKeysIterator_String_Value___read_another( _auto_2109_0 ))));
    while (ROGUE_COND(_auto_2110_0.exists))
    {
      ROGUE_GC_CHECK;
      ROGUE_DEF_LOCAL_REF(RogueString*,key_0,(_auto_2110_0.value));
      _auto_2110_0 = ((RogueOptionalString)(((RogueTableKeysIterator_String_Value___read_another( _auto_2109_0 )))));
      ROGUE_DEF_LOCAL_REF(RogueClassValue*,v_1,(((RogueClassValue*)(RogueValueTable__get__String( ROGUE_ARG(THIS), key_0 )))));
      if (ROGUE_COND((Rogue_call_ROGUEM9( 13, query_0, v_1 ))))
      {
        return (RogueClassValue*)(v_1);
      }
    }
  }
  return (RogueClassValue*)(((RogueClassValue*)(((RogueClassUndefinedValue*)ROGUE_SINGLETON(UndefinedValue)))));
}

RogueClassValue* RogueValueTable__get__Int32( RogueClassValueTable* THIS, RogueInt32 index_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((((index_0) < (0))) || (((index_0) >= (((RogueValueTable__count( ROGUE_ARG(THIS) )))))))))
  {
    return (RogueClassValue*)(((RogueClassValue*)(((RogueClassUndefinedValue*)ROGUE_SINGLETON(UndefinedValue)))));
  }
  ROGUE_DEF_LOCAL_REF(RogueClassValue*,result_1,(((RogueClassValue*)(RogueTable_String_Value___get__String( ROGUE_ARG(THIS->data), ROGUE_ARG(((RogueString*)(RogueInt32__to_String( index_0 )))) )))));
  if (ROGUE_COND(((void*)result_1) == ((void*)NULL)))
  {
    return (RogueClassValue*)(((RogueClassValue*)(((RogueClassUndefinedValue*)ROGUE_SINGLETON(UndefinedValue)))));
  }
  return (RogueClassValue*)(result_1);
}

RogueClassValue* RogueValueTable__get__String( RogueClassValueTable* THIS, RogueString* key_0 )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueClassValue*,result_1,(((RogueClassValue*)(RogueTable_String_Value___get__String( ROGUE_ARG(THIS->data), key_0 )))));
  if (ROGUE_COND(((void*)result_1) == ((void*)NULL)))
  {
    return (RogueClassValue*)(((RogueClassValue*)(((RogueClassUndefinedValue*)ROGUE_SINGLETON(UndefinedValue)))));
  }
  return (RogueClassValue*)(result_1);
}

RogueLogical RogueValueTable__is_collection( RogueClassValueTable* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(true);
}

RogueClassTableKeysIterator_String_Value_ RogueValueTable__keys( RogueClassValueTable* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueClassTableKeysIterator_String_Value_)(((RogueTable_String_Value___keys( ROGUE_ARG(THIS->data) ))));
}

RogueClassValueTable* RogueValueTable__set__String_Value( RogueClassValueTable* THIS, RogueString* key_0, RogueClassValue* _auto_4019 )
{
  ROGUE_DEF_LOCAL_REF(RogueClassValue*,new_value_1,_auto_4019);
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((void*)new_value_1) == ((void*)NULL)))
  {
    new_value_1 = ((RogueClassValue*)(((RogueClassValue*)(((RogueClassNullValue*)ROGUE_SINGLETON(NullValue))))));
  }
  RogueTable_String_Value___set__String_Value( ROGUE_ARG(THIS->data), key_0, new_value_1 );
  return (RogueClassValueTable*)(THIS);
}

RogueLogical RogueValueTable__to_Logical( RogueClassValueTable* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(true);
}

RogueStringBuilder* RogueValueTable__to_json__StringBuilder_Int32( RogueClassValueTable* THIS, RogueStringBuilder* buffer_0, RogueInt32 flags_1 )
{
  ROGUE_GC_CHECK;
  RogueLogical pretty_print_2 = (((!!(((flags_1) & (1)))) && (((((RogueValue__is_complex( ROGUE_ARG(((RogueClassValue*)THIS)) )))) || (!!(((flags_1) & (2))))))));
  RogueStringBuilder__print__Character( buffer_0, (RogueCharacter)'{' );
  if (ROGUE_COND(pretty_print_2))
  {
    RogueStringBuilder__println( buffer_0 );
    buffer_0->indent += 2;
  }
  RogueLogical first_3 = (true);
  {
    RogueClassTableKeysIterator_String_Value_ _auto_2161_0 = (((RogueTable_String_Value___keys( ROGUE_ARG(THIS->data) ))));
    RogueOptionalString _auto_2162_0 = (((RogueTableKeysIterator_String_Value___read_another( _auto_2161_0 ))));
    while (ROGUE_COND(_auto_2162_0.exists))
    {
      ROGUE_GC_CHECK;
      ROGUE_DEF_LOCAL_REF(RogueString*,key_0,(_auto_2162_0.value));
      _auto_2162_0 = ((RogueOptionalString)(((RogueTableKeysIterator_String_Value___read_another( _auto_2161_0 )))));
      if (ROGUE_COND(first_3))
      {
        first_3 = ((RogueLogical)(false));
      }
      else
      {
        if (ROGUE_COND(!(!!(((flags_1) & (2))))))
        {
          RogueStringBuilder__print__Character( buffer_0, (RogueCharacter)',' );
        }
        if (ROGUE_COND(pretty_print_2))
        {
          RogueStringBuilder__println( buffer_0 );
        }
      }
      RogueStringValue__to_json__String_StringBuilder_Int32( key_0, buffer_0, flags_1 );
      RogueStringBuilder__print__Character( buffer_0, (RogueCharacter)':' );
      RogueLogical indent_4 = (false);
      ROGUE_DEF_LOCAL_REF(RogueClassValue*,value_5,(((RogueClassValue*)(RogueTable_String_Value___get__String( ROGUE_ARG(THIS->data), key_0 )))));
      if (ROGUE_COND(((pretty_print_2) && ((((((RogueOptionalValue__operator__Value( value_5 ))) && (((RogueValue__is_complex( value_5 )))))) || (!!(((flags_1) & (2)))))))))
      {
        RogueStringBuilder__println( buffer_0 );
        indent_4 = ((RogueLogical)(((((void*)value_5) == ((void*)NULL)) || (!((Rogue_call_ROGUEM5( 52, value_5 )))))));
        if (ROGUE_COND(indent_4))
        {
          buffer_0->indent += 2;
        }
      }
      if (ROGUE_COND(((((void*)value_5) != ((void*)NULL)) && ((((RogueOptionalValue__operator__Value( value_5 ))) || ((Rogue_call_ROGUEM5( 59, value_5 ))))))))
      {
        Rogue_call_ROGUEM8( 159, value_5, buffer_0, flags_1 );
      }
      else
      {
        RogueStringBuilder__print__String( buffer_0, Rogue_literal_strings[6] );
      }
      if (ROGUE_COND(indent_4))
      {
        buffer_0->indent -= 2;
      }
    }
  }
  if (ROGUE_COND(pretty_print_2))
  {
    RogueStringBuilder__println( buffer_0 );
    buffer_0->indent -= 2;
  }
  RogueStringBuilder__print__Character( buffer_0, (RogueCharacter)'}' );
  return (RogueStringBuilder*)(buffer_0);
}

RogueClassTable_String_Value_* RogueTable_String_Value___init_object( RogueClassTable_String_Value_* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  return (RogueClassTable_String_Value_*)(THIS);
}

RogueClassTable_String_Value_* RogueTable_String_Value___init( RogueClassTable_String_Value_* THIS )
{
  ROGUE_GC_CHECK;
  RogueTable_String_Value___init__Int32( ROGUE_ARG(THIS), 16 );
  return (RogueClassTable_String_Value_*)(THIS);
}

RogueString* RogueTable_String_Value___to_String( RogueClassTable_String_Value_* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueTable_String_Value___print_to__StringBuilder( ROGUE_ARG(THIS), ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))) )))) ))));
}

RogueString* RogueTable_String_Value___type_name( RogueClassTable_String_Value_* THIS )
{
  return (RogueString*)(Rogue_literal_strings[333]);
}

RogueClassTable_String_Value_* RogueTable_String_Value___init__Int32( RogueClassTable_String_Value_* THIS, RogueInt32 bin_count_0 )
{
  ROGUE_GC_CHECK;
  RogueInt32 bins_power_of_2_1 = (1);
  while (ROGUE_COND(((bins_power_of_2_1) < (bin_count_0))))
  {
    ROGUE_GC_CHECK;
    bins_power_of_2_1 = ((RogueInt32)(((bins_power_of_2_1) << (1))));
  }
  bin_count_0 = ((RogueInt32)(bins_power_of_2_1));
  THIS->bin_mask = ((bin_count_0) - (1));
  THIS->bins = RogueType_create_array( bin_count_0, sizeof(RogueClassTableEntry_String_Value_*), true, 37 );
  return (RogueClassTable_String_Value_*)(THIS);
}

RogueClassValue* RogueTable_String_Value___at__Int32( RogueClassTable_String_Value_* THIS, RogueInt32 index_0 )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueClassTableEntry_String_Value_*,entry_1,(((RogueClassTableEntry_String_Value_*)(RogueTable_String_Value___entry_at__Int32( ROGUE_ARG(THIS), index_0 )))));
  if (ROGUE_COND(!!(entry_1)))
  {
    return (RogueClassValue*)(entry_1->value);
  }
  ROGUE_DEF_LOCAL_REF(RogueClassValue*,default_value_2,0);
  return (RogueClassValue*)(default_value_2);
}

RogueClassTableEntry_String_Value_* RogueTable_String_Value___entry_at__Int32( RogueClassTable_String_Value_* THIS, RogueInt32 index_0 )
{
  ROGUE_GC_CHECK;
  {
    {
      {
        if ( !(THIS->count) ) goto _auto_2097;
        if (ROGUE_COND(!(!!(THIS->cur_entry))))
        {
          if (ROGUE_COND(((index_0) <= (((THIS->count) / (2))))))
          {
            THIS->cur_entry = THIS->first_entry;
            THIS->cur_entry_index = 0;
          }
          else
          {
            THIS->cur_entry = THIS->last_entry;
            THIS->cur_entry_index = ((THIS->count) - (1));
          }
        }
        while (ROGUE_COND(((THIS->cur_entry_index) < (index_0))))
        {
          ROGUE_GC_CHECK;
          ++THIS->cur_entry_index;
          THIS->cur_entry = THIS->cur_entry->next_entry;
          if ( !(THIS->cur_entry) ) goto _auto_2097;
        }
        while (ROGUE_COND(((THIS->cur_entry_index) > (index_0))))
        {
          ROGUE_GC_CHECK;
          --THIS->cur_entry_index;
          THIS->cur_entry = THIS->cur_entry->previous_entry;
          if ( !(THIS->cur_entry) ) goto _auto_2097;
        }
        return (RogueClassTableEntry_String_Value_*)(THIS->cur_entry);
        }
    }
    _auto_2097:;
    {
      return (RogueClassTableEntry_String_Value_*)(((RogueClassTableEntry_String_Value_*)(NULL)));
      }
  }
}

RogueClassTableEntry_String_Value_* RogueTable_String_Value___find__String( RogueClassTable_String_Value_* THIS, RogueString* key_0 )
{
  ROGUE_GC_CHECK;
  RogueInt32 hash_1 = (((RogueString__hash_code( key_0 ))));
  ROGUE_DEF_LOCAL_REF(RogueClassTableEntry_String_Value_*,entry_2,(((RogueClassTableEntry_String_Value_*)(THIS->bins->as_objects[((hash_1) & (THIS->bin_mask))]))));
  while (ROGUE_COND(!!(entry_2)))
  {
    ROGUE_GC_CHECK;
    if (ROGUE_COND(((((entry_2->hash) == (hash_1))) && ((RogueString__operatorEQUALSEQUALS__String_String( ROGUE_ARG(entry_2->key), key_0 ))))))
    {
      return (RogueClassTableEntry_String_Value_*)(entry_2);
    }
    entry_2 = ((RogueClassTableEntry_String_Value_*)(entry_2->adjacent_entry));
  }
  return (RogueClassTableEntry_String_Value_*)(((RogueClassTableEntry_String_Value_*)(NULL)));
}

RogueClassValue* RogueTable_String_Value___get__String( RogueClassTable_String_Value_* THIS, RogueString* key_0 )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueClassTableEntry_String_Value_*,entry_1,(((RogueClassTableEntry_String_Value_*)(RogueTable_String_Value___find__String( ROGUE_ARG(THIS), key_0 )))));
  if (ROGUE_COND(!!(entry_1)))
  {
    return (RogueClassValue*)(entry_1->value);
  }
  else
  {
    ROGUE_DEF_LOCAL_REF(RogueClassValue*,default_value_2,0);
    return (RogueClassValue*)(default_value_2);
  }
}

RogueClassTableKeysIterator_String_Value_ RogueTable_String_Value___keys( RogueClassTable_String_Value_* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueClassTableKeysIterator_String_Value_)(RogueClassTableKeysIterator_String_Value_( THIS->first_entry ));
}

RogueStringBuilder* RogueTable_String_Value___print_to__StringBuilder( RogueClassTable_String_Value_* THIS, RogueStringBuilder* buffer_0 )
{
  ROGUE_GC_CHECK;
  RogueStringBuilder__print__Character( buffer_0, (RogueCharacter)'{' );
  ROGUE_DEF_LOCAL_REF(RogueClassTableEntry_String_Value_*,cur_1,(THIS->first_entry));
  RogueInt32 i_2 = (0);
  while (ROGUE_COND(!!(cur_1)))
  {
    ROGUE_GC_CHECK;
    if (ROGUE_COND(((i_2) > (0))))
    {
      RogueStringBuilder__print__Character( buffer_0, (RogueCharacter)',' );
    }
    RogueStringBuilder__print__String( buffer_0, ROGUE_ARG(cur_1->key) );
    RogueStringBuilder__print__Character( buffer_0, (RogueCharacter)':' );
    RogueStringBuilder__print__Object( buffer_0, ROGUE_ARG(((RogueObject*)(cur_1->value))) );
    cur_1 = ((RogueClassTableEntry_String_Value_*)(cur_1->next_entry));
    ++i_2;
  }
  RogueStringBuilder__print__Character( buffer_0, (RogueCharacter)'}' );
  return (RogueStringBuilder*)(buffer_0);
}

RogueClassTable_String_Value_* RogueTable_String_Value___set__String_Value( RogueClassTable_String_Value_* THIS, RogueString* key_0, RogueClassValue* value_1 )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueClassTableEntry_String_Value_*,entry_2,(((RogueClassTableEntry_String_Value_*)(RogueTable_String_Value___find__String( ROGUE_ARG(THIS), key_0 )))));
  if (ROGUE_COND(!!(entry_2)))
  {
    entry_2->value = value_1;
    if (ROGUE_COND(!!(THIS->sort_function)))
    {
      RogueTable_String_Value____adjust_entry_order__TableEntry_String_Value_( ROGUE_ARG(THIS), entry_2 );
    }
    return (RogueClassTable_String_Value_*)(THIS);
  }
  if (ROGUE_COND(((THIS->count) >= (THIS->bins->count))))
  {
    RogueTable_String_Value____grow( ROGUE_ARG(THIS) );
  }
  RogueInt32 hash_3 = (((RogueString__hash_code( key_0 ))));
  RogueInt32 index_4 = (((hash_3) & (THIS->bin_mask)));
  if (ROGUE_COND(!(!!(entry_2))))
  {
    entry_2 = ((RogueClassTableEntry_String_Value_*)(((RogueClassTableEntry_String_Value_*)(RogueTableEntry_String_Value___init__String_Value_Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassTableEntry_String_Value_*,ROGUE_CREATE_OBJECT(TableEntry_String_Value_))), key_0, value_1, hash_3 )))));
  }
  entry_2->adjacent_entry = ((RogueClassTableEntry_String_Value_*)(THIS->bins->as_objects[index_4]));
  THIS->bins->as_objects[index_4] = entry_2;
  RogueTable_String_Value____place_entry_in_order__TableEntry_String_Value_( ROGUE_ARG(THIS), entry_2 );
  THIS->count = ((THIS->count) + (1));
  return (RogueClassTable_String_Value_*)(THIS);
}

void RogueTable_String_Value____adjust_entry_order__TableEntry_String_Value_( RogueClassTable_String_Value_* THIS, RogueClassTableEntry_String_Value_* entry_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((void*)THIS->first_entry) == ((void*)THIS->last_entry)))
  {
    return;
  }
  if (ROGUE_COND(((void*)entry_0) == ((void*)THIS->first_entry)))
  {
    if (ROGUE_COND((Rogue_call_ROGUEM11( 13, ROGUE_ARG(THIS->sort_function), entry_0, ROGUE_ARG(entry_0->next_entry) ))))
    {
      return;
    }
  }
  else if (ROGUE_COND(((void*)entry_0) == ((void*)THIS->last_entry)))
  {
    if (ROGUE_COND((Rogue_call_ROGUEM11( 13, ROGUE_ARG(THIS->sort_function), ROGUE_ARG(entry_0->previous_entry), entry_0 ))))
    {
      return;
    }
  }
  else if (ROGUE_COND((((Rogue_call_ROGUEM11( 13, ROGUE_ARG(THIS->sort_function), ROGUE_ARG(entry_0->previous_entry), entry_0 ))) && ((Rogue_call_ROGUEM11( 13, ROGUE_ARG(THIS->sort_function), entry_0, ROGUE_ARG(entry_0->next_entry) ))))))
  {
    return;
  }
  RogueTable_String_Value____unlink__TableEntry_String_Value_( ROGUE_ARG(THIS), entry_0 );
  RogueTable_String_Value____place_entry_in_order__TableEntry_String_Value_( ROGUE_ARG(THIS), entry_0 );
}

void RogueTable_String_Value____place_entry_in_order__TableEntry_String_Value_( RogueClassTable_String_Value_* THIS, RogueClassTableEntry_String_Value_* entry_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!!(THIS->first_entry)))
  {
    if (ROGUE_COND(!!(THIS->sort_function)))
    {
      if (ROGUE_COND((Rogue_call_ROGUEM11( 13, ROGUE_ARG(THIS->sort_function), entry_0, ROGUE_ARG(THIS->first_entry) ))))
      {
        entry_0->next_entry = THIS->first_entry;
        THIS->first_entry->previous_entry = entry_0;
        THIS->first_entry = entry_0;
      }
      else if (ROGUE_COND((Rogue_call_ROGUEM11( 13, ROGUE_ARG(THIS->sort_function), ROGUE_ARG(THIS->last_entry), entry_0 ))))
      {
        THIS->last_entry->next_entry = entry_0;
        entry_0->previous_entry = THIS->last_entry;
        THIS->last_entry = entry_0;
      }
      else
      {
        ROGUE_DEF_LOCAL_REF(RogueClassTableEntry_String_Value_*,cur_1,(THIS->first_entry));
        while (ROGUE_COND(!!(cur_1->next_entry)))
        {
          ROGUE_GC_CHECK;
          if (ROGUE_COND((Rogue_call_ROGUEM11( 13, ROGUE_ARG(THIS->sort_function), entry_0, ROGUE_ARG(cur_1->next_entry) ))))
          {
            entry_0->previous_entry = cur_1;
            entry_0->next_entry = cur_1->next_entry;
            entry_0->next_entry->previous_entry = entry_0;
            cur_1->next_entry = entry_0;
            goto _auto_481;
          }
          cur_1 = ((RogueClassTableEntry_String_Value_*)(cur_1->next_entry));
        }
        _auto_481:;
      }
    }
    else
    {
      THIS->last_entry->next_entry = entry_0;
      entry_0->previous_entry = THIS->last_entry;
      THIS->last_entry = entry_0;
    }
  }
  else
  {
    THIS->first_entry = entry_0;
    THIS->last_entry = entry_0;
  }
}

void RogueTable_String_Value____unlink__TableEntry_String_Value_( RogueClassTable_String_Value_* THIS, RogueClassTableEntry_String_Value_* entry_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((void*)entry_0) == ((void*)THIS->first_entry)))
  {
    if (ROGUE_COND(((void*)entry_0) == ((void*)THIS->last_entry)))
    {
      THIS->first_entry = ((RogueClassTableEntry_String_Value_*)(NULL));
      THIS->last_entry = ((RogueClassTableEntry_String_Value_*)(NULL));
    }
    else
    {
      THIS->first_entry = entry_0->next_entry;
      THIS->first_entry->previous_entry = ((RogueClassTableEntry_String_Value_*)(NULL));
    }
  }
  else if (ROGUE_COND(((void*)entry_0) == ((void*)THIS->last_entry)))
  {
    THIS->last_entry = entry_0->previous_entry;
    THIS->last_entry->next_entry = ((RogueClassTableEntry_String_Value_*)(NULL));
  }
  else
  {
    entry_0->previous_entry->next_entry = entry_0->next_entry;
    entry_0->next_entry->previous_entry = entry_0->previous_entry;
  }
}

void RogueTable_String_Value____grow( RogueClassTable_String_Value_* THIS )
{
  ROGUE_GC_CHECK;
  THIS->bins = RogueType_create_array( ((THIS->bins->count) * (2)), sizeof(RogueClassTableEntry_String_Value_*), true, 37 );
  THIS->bin_mask = ((((THIS->bin_mask) << (1))) | (1));
  ROGUE_DEF_LOCAL_REF(RogueClassTableEntry_String_Value_*,cur_0,(THIS->first_entry));
  while (ROGUE_COND(!!(cur_0)))
  {
    ROGUE_GC_CHECK;
    RogueInt32 index_1 = (((cur_0->hash) & (THIS->bin_mask)));
    cur_0->adjacent_entry = ((RogueClassTableEntry_String_Value_*)(THIS->bins->as_objects[index_1]));
    THIS->bins->as_objects[index_1] = cur_0;
    cur_0 = ((RogueClassTableEntry_String_Value_*)(cur_0->next_entry));
  }
}

RogueString* RogueArray_TableEntry_String_Value____type_name( RogueArray* THIS )
{
  return (RogueString*)(Rogue_literal_strings[387]);
}

RogueClassTableEntry_String_Value_* RogueTableEntry_String_Value___init_object( RogueClassTableEntry_String_Value_* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  return (RogueClassTableEntry_String_Value_*)(THIS);
}

RogueString* RogueTableEntry_String_Value___to_String( RogueClassTableEntry_String_Value_* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[113] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(THIS->key) ))) )))), Rogue_literal_strings[295] )))), ROGUE_ARG(((RogueString*)(RogueString__operatorPLUS__Object( ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], Rogue_literal_strings[0] ))), ROGUE_ARG(((RogueObject*)(THIS->value))) )))) )))), Rogue_literal_strings[116] )))) ))));
}

RogueString* RogueTableEntry_String_Value___type_name( RogueClassTableEntry_String_Value_* THIS )
{
  return (RogueString*)(Rogue_literal_strings[334]);
}

RogueClassTableEntry_String_Value_* RogueTableEntry_String_Value___init__String_Value_Int32( RogueClassTableEntry_String_Value_* THIS, RogueString* _key_0, RogueClassValue* _value_1, RogueInt32 _hash_2 )
{
  ROGUE_GC_CHECK;
  THIS->key = _key_0;
  THIS->value = _value_1;
  THIS->hash = _hash_2;
  return (RogueClassTableEntry_String_Value_*)(THIS);
}

RogueClass_Function_TableEntry_String_Value__TableEntry_String_Value__RETURNSLogical_* Rogue_Function_TableEntry_String_Value__TableEntry_String_Value__RETURNSLogical___init_object( RogueClass_Function_TableEntry_String_Value__TableEntry_String_Value__RETURNSLogical_* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  return (RogueClass_Function_TableEntry_String_Value__TableEntry_String_Value__RETURNSLogical_*)(THIS);
}

RogueString* Rogue_Function_TableEntry_String_Value__TableEntry_String_Value__RETURNSLogical___type_name( RogueClass_Function_TableEntry_String_Value__TableEntry_String_Value__RETURNSLogical_* THIS )
{
  return (RogueString*)(Rogue_literal_strings[346]);
}

RogueLogical Rogue_Function_TableEntry_String_Value__TableEntry_String_Value__RETURNSLogical___call__TableEntry_String_Value__TableEntry_String_Value_( RogueClass_Function_TableEntry_String_Value__TableEntry_String_Value__RETURNSLogical_* THIS, RogueClassTableEntry_String_Value_* param1_0, RogueClassTableEntry_String_Value_* param2_1 )
{
  return (RogueLogical)(false);
}

RogueClassSystem* RogueSystem__init_object( RogueClassSystem* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  return (RogueClassSystem*)(THIS);
}

RogueString* RogueSystem__type_name( RogueClassSystem* THIS )
{
  return (RogueString*)(Rogue_literal_strings[335]);
}

RogueClassSystemEnvironment* RogueSystemEnvironment__init_object( RogueClassSystemEnvironment* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  return (RogueClassSystemEnvironment*)(THIS);
}

RogueString* RogueSystemEnvironment__type_name( RogueClassSystemEnvironment* THIS )
{
  return (RogueString*)(Rogue_literal_strings[336]);
}

RogueClassStringTable_String_* RogueSystemEnvironment__definitions( RogueClassSystemEnvironment* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!!(THIS->definitions)))
  {
    return (RogueClassStringTable_String_*)(THIS->definitions);
  }
  THIS->definitions = ((RogueClassStringTable_String_*)(((RogueClassTable_String_String_*)(RogueTable_String_String___init( ROGUE_ARG(((RogueClassTable_String_String_*)ROGUE_CREATE_REF(RogueClassStringTable_String_*,ROGUE_CREATE_OBJECT(StringTable_String_)))) )))));
  char** env = environ;

  while (ROGUE_COND(((RogueLogical)(*env))))
  {
    ROGUE_GC_CHECK;
    ROGUE_DEF_LOCAL_REF(RogueString_List*,parts_0,(((RogueString_List*)(RogueString__split__Character_Logical( ROGUE_ARG(((RogueString*)(RogueString_create_from_utf8( *(env++) )))), (RogueCharacter)'=', false )))));
    ROGUE_DEF_LOCAL_REF(RogueString*,name_1,(((RogueString*)(RogueString_List__first( parts_0 )))));
    ROGUE_DEF_LOCAL_REF(RogueString*,value_2,(((((((parts_0->count) == (2))))) ? (ROGUE_ARG((RogueString*)((RogueString*)(RogueString_List__last( parts_0 ))))) : ROGUE_ARG(Rogue_literal_strings[0]))));
    RogueTable_String_String___set__String_String( ROGUE_ARG(((RogueClassTable_String_String_*)THIS->definitions)), name_1, value_2 );
  }
  return (RogueClassStringTable_String_*)(THIS->definitions);
}

RogueString* RogueSystemEnvironment__get__String( RogueClassSystemEnvironment* THIS, RogueString* name_0 )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)Rogue_call_ROGUEM4( 29, ROGUE_ARG(((RogueClassTable_String_String_*)((RogueClassStringTable_String_*)(RogueSystemEnvironment__definitions( ROGUE_ARG(THIS) ))))), name_0 )));
}

RogueString_List* RogueSystemEnvironment__names( RogueClassSystemEnvironment* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!!(THIS->names)))
  {
    return (RogueString_List*)(THIS->names);
  }
  THIS->names = ((RogueString_List*)(RogueTableKeysIterator_String_String___to_list__String_List( ((RogueTable_String_String___keys( ROGUE_ARG(((RogueClassTable_String_String_*)((RogueClassStringTable_String_*)(RogueSystemEnvironment__definitions( ROGUE_ARG(THIS) ))))) ))), ROGUE_ARG(((RogueString_List*)(NULL))) )));
  return (RogueString_List*)(THIS->names);
}

RogueClassStringTable_String_* RogueStringTable_String___init_object( RogueClassStringTable_String_* THIS )
{
  ROGUE_GC_CHECK;
  RogueTable_String_String___init_object( ROGUE_ARG(((RogueClassTable_String_String_*)THIS)) );
  return (RogueClassStringTable_String_*)(THIS);
}

RogueString* RogueStringTable_String___type_name( RogueClassStringTable_String_* THIS )
{
  return (RogueString*)(Rogue_literal_strings[425]);
}

RogueClassTable_String_String_* RogueTable_String_String___init_object( RogueClassTable_String_String_* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  return (RogueClassTable_String_String_*)(THIS);
}

RogueClassTable_String_String_* RogueTable_String_String___init( RogueClassTable_String_String_* THIS )
{
  ROGUE_GC_CHECK;
  RogueTable_String_String___init__Int32( ROGUE_ARG(THIS), 16 );
  return (RogueClassTable_String_String_*)(THIS);
}

RogueString* RogueTable_String_String___to_String( RogueClassTable_String_String_* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueTable_String_String___print_to__StringBuilder( ROGUE_ARG(THIS), ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))) )))) ))));
}

RogueString* RogueTable_String_String___type_name( RogueClassTable_String_String_* THIS )
{
  return (RogueString*)(Rogue_literal_strings[337]);
}

RogueClassTable_String_String_* RogueTable_String_String___init__Int32( RogueClassTable_String_String_* THIS, RogueInt32 bin_count_0 )
{
  ROGUE_GC_CHECK;
  RogueInt32 bins_power_of_2_1 = (1);
  while (ROGUE_COND(((bins_power_of_2_1) < (bin_count_0))))
  {
    ROGUE_GC_CHECK;
    bins_power_of_2_1 = ((RogueInt32)(((bins_power_of_2_1) << (1))));
  }
  bin_count_0 = ((RogueInt32)(bins_power_of_2_1));
  THIS->bin_mask = ((bin_count_0) - (1));
  THIS->bins = RogueType_create_array( bin_count_0, sizeof(RogueClassTableEntry_String_String_*), true, 44 );
  return (RogueClassTable_String_String_*)(THIS);
}

RogueClassTableEntry_String_String_* RogueTable_String_String___find__String( RogueClassTable_String_String_* THIS, RogueString* key_0 )
{
  ROGUE_GC_CHECK;
  RogueInt32 hash_1 = (((RogueString__hash_code( key_0 ))));
  ROGUE_DEF_LOCAL_REF(RogueClassTableEntry_String_String_*,entry_2,(((RogueClassTableEntry_String_String_*)(THIS->bins->as_objects[((hash_1) & (THIS->bin_mask))]))));
  while (ROGUE_COND(!!(entry_2)))
  {
    ROGUE_GC_CHECK;
    if (ROGUE_COND(((((entry_2->hash) == (hash_1))) && ((RogueString__operatorEQUALSEQUALS__String_String( ROGUE_ARG(entry_2->key), key_0 ))))))
    {
      return (RogueClassTableEntry_String_String_*)(entry_2);
    }
    entry_2 = ((RogueClassTableEntry_String_String_*)(entry_2->adjacent_entry));
  }
  return (RogueClassTableEntry_String_String_*)(((RogueClassTableEntry_String_String_*)(NULL)));
}

RogueString* RogueTable_String_String___get__String( RogueClassTable_String_String_* THIS, RogueString* key_0 )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueClassTableEntry_String_String_*,entry_1,(((RogueClassTableEntry_String_String_*)(RogueTable_String_String___find__String( ROGUE_ARG(THIS), key_0 )))));
  if (ROGUE_COND(!!(entry_1)))
  {
    return (RogueString*)(entry_1->value);
  }
  else
  {
    ROGUE_DEF_LOCAL_REF(RogueString*,default_value_2,0);
    return (RogueString*)(default_value_2);
  }
}

RogueClassTableKeysIterator_String_String_ RogueTable_String_String___keys( RogueClassTable_String_String_* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueClassTableKeysIterator_String_String_)(RogueClassTableKeysIterator_String_String_( THIS->first_entry ));
}

RogueStringBuilder* RogueTable_String_String___print_to__StringBuilder( RogueClassTable_String_String_* THIS, RogueStringBuilder* buffer_0 )
{
  ROGUE_GC_CHECK;
  RogueStringBuilder__print__Character( buffer_0, (RogueCharacter)'{' );
  ROGUE_DEF_LOCAL_REF(RogueClassTableEntry_String_String_*,cur_1,(THIS->first_entry));
  RogueInt32 i_2 = (0);
  while (ROGUE_COND(!!(cur_1)))
  {
    ROGUE_GC_CHECK;
    if (ROGUE_COND(((i_2) > (0))))
    {
      RogueStringBuilder__print__Character( buffer_0, (RogueCharacter)',' );
    }
    RogueStringBuilder__print__String( buffer_0, ROGUE_ARG(cur_1->key) );
    RogueStringBuilder__print__Character( buffer_0, (RogueCharacter)':' );
    RogueStringBuilder__print__String( buffer_0, ROGUE_ARG(cur_1->value) );
    cur_1 = ((RogueClassTableEntry_String_String_*)(cur_1->next_entry));
    ++i_2;
  }
  RogueStringBuilder__print__Character( buffer_0, (RogueCharacter)'}' );
  return (RogueStringBuilder*)(buffer_0);
}

RogueClassTable_String_String_* RogueTable_String_String___set__String_String( RogueClassTable_String_String_* THIS, RogueString* key_0, RogueString* value_1 )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueClassTableEntry_String_String_*,entry_2,(((RogueClassTableEntry_String_String_*)(RogueTable_String_String___find__String( ROGUE_ARG(THIS), key_0 )))));
  if (ROGUE_COND(!!(entry_2)))
  {
    entry_2->value = value_1;
    if (ROGUE_COND(!!(THIS->sort_function)))
    {
      RogueTable_String_String____adjust_entry_order__TableEntry_String_String_( ROGUE_ARG(THIS), entry_2 );
    }
    return (RogueClassTable_String_String_*)(THIS);
  }
  if (ROGUE_COND(((THIS->count) >= (THIS->bins->count))))
  {
    RogueTable_String_String____grow( ROGUE_ARG(THIS) );
  }
  RogueInt32 hash_3 = (((RogueString__hash_code( key_0 ))));
  RogueInt32 index_4 = (((hash_3) & (THIS->bin_mask)));
  if (ROGUE_COND(!(!!(entry_2))))
  {
    entry_2 = ((RogueClassTableEntry_String_String_*)(((RogueClassTableEntry_String_String_*)(RogueTableEntry_String_String___init__String_String_Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassTableEntry_String_String_*,ROGUE_CREATE_OBJECT(TableEntry_String_String_))), key_0, value_1, hash_3 )))));
  }
  entry_2->adjacent_entry = ((RogueClassTableEntry_String_String_*)(THIS->bins->as_objects[index_4]));
  THIS->bins->as_objects[index_4] = entry_2;
  RogueTable_String_String____place_entry_in_order__TableEntry_String_String_( ROGUE_ARG(THIS), entry_2 );
  THIS->count = ((THIS->count) + (1));
  return (RogueClassTable_String_String_*)(THIS);
}

void RogueTable_String_String____adjust_entry_order__TableEntry_String_String_( RogueClassTable_String_String_* THIS, RogueClassTableEntry_String_String_* entry_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((void*)THIS->first_entry) == ((void*)THIS->last_entry)))
  {
    return;
  }
  if (ROGUE_COND(((void*)entry_0) == ((void*)THIS->first_entry)))
  {
    if (ROGUE_COND(((Rogue_Function_TableEntry_String_String__TableEntry_String_String__RETURNSLogical___call__TableEntry_String_String__TableEntry_String_String_( ROGUE_ARG(THIS->sort_function), entry_0, ROGUE_ARG(entry_0->next_entry) )))))
    {
      return;
    }
  }
  else if (ROGUE_COND(((void*)entry_0) == ((void*)THIS->last_entry)))
  {
    if (ROGUE_COND(((Rogue_Function_TableEntry_String_String__TableEntry_String_String__RETURNSLogical___call__TableEntry_String_String__TableEntry_String_String_( ROGUE_ARG(THIS->sort_function), ROGUE_ARG(entry_0->previous_entry), entry_0 )))))
    {
      return;
    }
  }
  else if (ROGUE_COND(((((Rogue_Function_TableEntry_String_String__TableEntry_String_String__RETURNSLogical___call__TableEntry_String_String__TableEntry_String_String_( ROGUE_ARG(THIS->sort_function), ROGUE_ARG(entry_0->previous_entry), entry_0 )))) && (((Rogue_Function_TableEntry_String_String__TableEntry_String_String__RETURNSLogical___call__TableEntry_String_String__TableEntry_String_String_( ROGUE_ARG(THIS->sort_function), entry_0, ROGUE_ARG(entry_0->next_entry) )))))))
  {
    return;
  }
  RogueTable_String_String____unlink__TableEntry_String_String_( ROGUE_ARG(THIS), entry_0 );
  RogueTable_String_String____place_entry_in_order__TableEntry_String_String_( ROGUE_ARG(THIS), entry_0 );
}

void RogueTable_String_String____place_entry_in_order__TableEntry_String_String_( RogueClassTable_String_String_* THIS, RogueClassTableEntry_String_String_* entry_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!!(THIS->first_entry)))
  {
    if (ROGUE_COND(!!(THIS->sort_function)))
    {
      if (ROGUE_COND(((Rogue_Function_TableEntry_String_String__TableEntry_String_String__RETURNSLogical___call__TableEntry_String_String__TableEntry_String_String_( ROGUE_ARG(THIS->sort_function), entry_0, ROGUE_ARG(THIS->first_entry) )))))
      {
        entry_0->next_entry = THIS->first_entry;
        THIS->first_entry->previous_entry = entry_0;
        THIS->first_entry = entry_0;
      }
      else if (ROGUE_COND(((Rogue_Function_TableEntry_String_String__TableEntry_String_String__RETURNSLogical___call__TableEntry_String_String__TableEntry_String_String_( ROGUE_ARG(THIS->sort_function), ROGUE_ARG(THIS->last_entry), entry_0 )))))
      {
        THIS->last_entry->next_entry = entry_0;
        entry_0->previous_entry = THIS->last_entry;
        THIS->last_entry = entry_0;
      }
      else
      {
        ROGUE_DEF_LOCAL_REF(RogueClassTableEntry_String_String_*,cur_1,(THIS->first_entry));
        while (ROGUE_COND(!!(cur_1->next_entry)))
        {
          ROGUE_GC_CHECK;
          if (ROGUE_COND(((Rogue_Function_TableEntry_String_String__TableEntry_String_String__RETURNSLogical___call__TableEntry_String_String__TableEntry_String_String_( ROGUE_ARG(THIS->sort_function), entry_0, ROGUE_ARG(cur_1->next_entry) )))))
          {
            entry_0->previous_entry = cur_1;
            entry_0->next_entry = cur_1->next_entry;
            entry_0->next_entry->previous_entry = entry_0;
            cur_1->next_entry = entry_0;
            goto _auto_399;
          }
          cur_1 = ((RogueClassTableEntry_String_String_*)(cur_1->next_entry));
        }
        _auto_399:;
      }
    }
    else
    {
      THIS->last_entry->next_entry = entry_0;
      entry_0->previous_entry = THIS->last_entry;
      THIS->last_entry = entry_0;
    }
  }
  else
  {
    THIS->first_entry = entry_0;
    THIS->last_entry = entry_0;
  }
}

void RogueTable_String_String____unlink__TableEntry_String_String_( RogueClassTable_String_String_* THIS, RogueClassTableEntry_String_String_* entry_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((void*)entry_0) == ((void*)THIS->first_entry)))
  {
    if (ROGUE_COND(((void*)entry_0) == ((void*)THIS->last_entry)))
    {
      THIS->first_entry = ((RogueClassTableEntry_String_String_*)(NULL));
      THIS->last_entry = ((RogueClassTableEntry_String_String_*)(NULL));
    }
    else
    {
      THIS->first_entry = entry_0->next_entry;
      THIS->first_entry->previous_entry = ((RogueClassTableEntry_String_String_*)(NULL));
    }
  }
  else if (ROGUE_COND(((void*)entry_0) == ((void*)THIS->last_entry)))
  {
    THIS->last_entry = entry_0->previous_entry;
    THIS->last_entry->next_entry = ((RogueClassTableEntry_String_String_*)(NULL));
  }
  else
  {
    entry_0->previous_entry->next_entry = entry_0->next_entry;
    entry_0->next_entry->previous_entry = entry_0->previous_entry;
  }
}

void RogueTable_String_String____grow( RogueClassTable_String_String_* THIS )
{
  ROGUE_GC_CHECK;
  THIS->bins = RogueType_create_array( ((THIS->bins->count) * (2)), sizeof(RogueClassTableEntry_String_String_*), true, 44 );
  THIS->bin_mask = ((((THIS->bin_mask) << (1))) | (1));
  ROGUE_DEF_LOCAL_REF(RogueClassTableEntry_String_String_*,cur_0,(THIS->first_entry));
  while (ROGUE_COND(!!(cur_0)))
  {
    ROGUE_GC_CHECK;
    RogueInt32 index_1 = (((cur_0->hash) & (THIS->bin_mask)));
    cur_0->adjacent_entry = ((RogueClassTableEntry_String_String_*)(THIS->bins->as_objects[index_1]));
    THIS->bins->as_objects[index_1] = cur_0;
    cur_0 = ((RogueClassTableEntry_String_String_*)(cur_0->next_entry));
  }
}

RogueString* RogueArray_TableEntry_String_String____type_name( RogueArray* THIS )
{
  return (RogueString*)(Rogue_literal_strings[385]);
}

RogueClassTableEntry_String_String_* RogueTableEntry_String_String___init_object( RogueClassTableEntry_String_String_* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  return (RogueClassTableEntry_String_String_*)(THIS);
}

RogueString* RogueTableEntry_String_String___to_String( RogueClassTableEntry_String_String_* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[113] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(THIS->key) ))) )))), Rogue_literal_strings[295] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(THIS->value) ))) )))), Rogue_literal_strings[116] )))) ))));
}

RogueString* RogueTableEntry_String_String___type_name( RogueClassTableEntry_String_String_* THIS )
{
  return (RogueString*)(Rogue_literal_strings[338]);
}

RogueClassTableEntry_String_String_* RogueTableEntry_String_String___init__String_String_Int32( RogueClassTableEntry_String_String_* THIS, RogueString* _key_0, RogueString* _value_1, RogueInt32 _hash_2 )
{
  ROGUE_GC_CHECK;
  THIS->key = _key_0;
  THIS->value = _value_1;
  THIS->hash = _hash_2;
  return (RogueClassTableEntry_String_String_*)(THIS);
}

RogueClass_Function_TableEntry_String_String__TableEntry_String_String__RETURNSLogical_* Rogue_Function_TableEntry_String_String__TableEntry_String_String__RETURNSLogical___init_object( RogueClass_Function_TableEntry_String_String__TableEntry_String_String__RETURNSLogical_* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  return (RogueClass_Function_TableEntry_String_String__TableEntry_String_String__RETURNSLogical_*)(THIS);
}

RogueString* Rogue_Function_TableEntry_String_String__TableEntry_String_String__RETURNSLogical___type_name( RogueClass_Function_TableEntry_String_String__TableEntry_String_String__RETURNSLogical_* THIS )
{
  return (RogueString*)(Rogue_literal_strings[340]);
}

RogueLogical Rogue_Function_TableEntry_String_String__TableEntry_String_String__RETURNSLogical___call__TableEntry_String_String__TableEntry_String_String_( RogueClass_Function_TableEntry_String_String__TableEntry_String_String__RETURNSLogical_* THIS, RogueClassTableEntry_String_String_* param1_0, RogueClassTableEntry_String_String_* param2_1 )
{
  return (RogueLogical)(false);
}

RogueClass_Function_String_RETURNSLogical_* Rogue_Function_String_RETURNSLogical___init_object( RogueClass_Function_String_RETURNSLogical_* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  return (RogueClass_Function_String_RETURNSLogical_*)(THIS);
}

RogueString* Rogue_Function_String_RETURNSLogical___type_name( RogueClass_Function_String_RETURNSLogical_* THIS )
{
  return (RogueString*)(Rogue_literal_strings[339]);
}

RogueLogical Rogue_Function_String_RETURNSLogical___call__String( RogueClass_Function_String_RETURNSLogical_* THIS, RogueString* param1_0 )
{
  return (RogueLogical)(false);
}

RogueClassMath* RogueMath__init_object( RogueClassMath* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  return (RogueClassMath*)(THIS);
}

RogueString* RogueMath__type_name( RogueClassMath* THIS )
{
  return (RogueString*)(Rogue_literal_strings[342]);
}

RogueClassStringValue* RogueStringValue__init_object( RogueClassStringValue* THIS )
{
  ROGUE_GC_CHECK;
  RogueValue__init_object( ROGUE_ARG(((RogueClassValue*)THIS)) );
  return (RogueClassStringValue*)(THIS);
}

RogueString* RogueStringValue__to_String( RogueClassStringValue* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(THIS->value);
}

RogueString* RogueStringValue__type_name( RogueClassStringValue* THIS )
{
  return (RogueString*)(Rogue_literal_strings[405]);
}

RogueInt32 RogueStringValue__count( RogueClassStringValue* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueInt32)(((RogueString__count( ROGUE_ARG(THIS->value) ))));
}

RogueClassValue* RogueStringValue__get__Int32( RogueClassStringValue* THIS, RogueInt32 index_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((((index_0) < (0))) || (((index_0) >= (((RogueString__count( ROGUE_ARG(THIS->value) )))))))))
  {
    return (RogueClassValue*)(((RogueClassValue*)(((RogueClassUndefinedValue*)ROGUE_SINGLETON(UndefinedValue)))));
  }
  return (RogueClassValue*)((RogueValue__create__String( ROGUE_ARG(((RogueString*)(RogueString__operatorPLUS__Character( ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], Rogue_literal_strings[0] ))), ROGUE_ARG(((RogueString__get__Int32( ROGUE_ARG(THIS->value), index_0 )))) )))) )));
}

RogueInt64 RogueStringValue__to_Int64( RogueClassStringValue* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueInt64)(((RogueString__to_Int64__Int32( ROGUE_ARG(THIS->value), 10 ))));
}

RogueLogical RogueStringValue__to_Logical( RogueClassStringValue* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(!(((RogueString__is_false( ROGUE_ARG(THIS->value) )))));
}

RogueStringBuilder* RogueStringValue__to_json__StringBuilder_Int32( RogueClassStringValue* THIS, RogueStringBuilder* buffer_0, RogueInt32 flags_1 )
{
  ROGUE_GC_CHECK;
  return (RogueStringBuilder*)((RogueStringValue__to_json__String_StringBuilder_Int32( ROGUE_ARG(THIS->value), buffer_0, flags_1 )));
}

RogueClassStringValue* RogueStringValue__init__String( RogueClassStringValue* THIS, RogueString* _auto_412_0 )
{
  ROGUE_GC_CHECK;
  THIS->value = _auto_412_0;
  return (RogueClassStringValue*)(THIS);
}

RogueClassReal64Value* RogueReal64Value__init_object( RogueClassReal64Value* THIS )
{
  ROGUE_GC_CHECK;
  RogueValue__init_object( ROGUE_ARG(((RogueClassValue*)THIS)) );
  return (RogueClassReal64Value*)(THIS);
}

RogueString* RogueReal64Value__to_String( RogueClassReal64Value* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(RogueReal64__format__OptionalInt32( ROGUE_ARG(THIS->value), (RogueOptionalInt32__create()) ))));
}

RogueString* RogueReal64Value__type_name( RogueClassReal64Value* THIS )
{
  return (RogueString*)(Rogue_literal_strings[406]);
}

RogueInt64 RogueReal64Value__to_Int64( RogueClassReal64Value* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueInt64)(((RogueInt64)(THIS->value)));
}

RogueStringBuilder* RogueReal64Value__to_json__StringBuilder_Int32( RogueClassReal64Value* THIS, RogueStringBuilder* buffer_0, RogueInt32 flags_1 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!!(((RogueReal64__fractional_part( ROGUE_ARG(THIS->value) ))))))
  {
    RogueStringBuilder__print__Real64( buffer_0, ROGUE_ARG(THIS->value) );
  }
  else
  {
    RogueStringBuilder__print__Real64_Int32( buffer_0, ROGUE_ARG(THIS->value), 0 );
  }
  return (RogueStringBuilder*)(buffer_0);
}

RogueClassReal64Value* RogueReal64Value__init__Real64( RogueClassReal64Value* THIS, RogueReal64 _auto_413_0 )
{
  ROGUE_GC_CHECK;
  THIS->value = _auto_413_0;
  return (RogueClassReal64Value*)(THIS);
}

RogueClassLogicalValue* RogueLogicalValue__init_object( RogueClassLogicalValue* THIS )
{
  ROGUE_GC_CHECK;
  RogueValue__init_object( ROGUE_ARG(((RogueClassValue*)THIS)) );
  return (RogueClassLogicalValue*)(THIS);
}

RogueString* RogueLogicalValue__to_String( RogueClassLogicalValue* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(RogueLogical__to_String( ROGUE_ARG(THIS->value) ))));
}

RogueString* RogueLogicalValue__type_name( RogueClassLogicalValue* THIS )
{
  return (RogueString*)(Rogue_literal_strings[407]);
}

RogueLogical RogueLogicalValue__is_logical( RogueClassLogicalValue* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(true);
}

RogueInt64 RogueLogicalValue__to_Int64( RogueClassLogicalValue* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(THIS->value))
  {
    return (RogueInt64)(1LL);
  }
  else
  {
    return (RogueInt64)(0LL);
  }
}

RogueLogical RogueLogicalValue__to_Logical( RogueClassLogicalValue* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(THIS->value);
}

RogueStringBuilder* RogueLogicalValue__to_json__StringBuilder_Int32( RogueClassLogicalValue* THIS, RogueStringBuilder* buffer_0, RogueInt32 flags_1 )
{
  ROGUE_GC_CHECK;
  RogueStringBuilder__print__Logical( buffer_0, ROGUE_ARG(THIS->value) );
  return (RogueStringBuilder*)(buffer_0);
}

RogueClassLogicalValue* RogueLogicalValue__init__Logical( RogueClassLogicalValue* THIS, RogueLogical _auto_416_0 )
{
  ROGUE_GC_CHECK;
  THIS->value = _auto_416_0;
  return (RogueClassLogicalValue*)(THIS);
}

RogueClassValueList* RogueValueList__init_object( RogueClassValueList* THIS )
{
  ROGUE_GC_CHECK;
  RogueValue__init_object( ROGUE_ARG(((RogueClassValue*)THIS)) );
  return (RogueClassValueList*)(THIS);
}

RogueClassValueList* RogueValueList__init( RogueClassValueList* THIS )
{
  ROGUE_GC_CHECK;
  RogueValueList__init__Int32( ROGUE_ARG(THIS), 0 );
  return (RogueClassValueList*)(THIS);
}

RogueString* RogueValueList__to_String( RogueClassValueList* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(RogueValue_List__to_String( ROGUE_ARG(THIS->data) ))));
}

RogueString* RogueValueList__type_name( RogueClassValueList* THIS )
{
  return (RogueString*)(Rogue_literal_strings[408]);
}

RogueClassValueList* RogueValueList__add__Value( RogueClassValueList* THIS, RogueClassValue* value_0 )
{
  ROGUE_GC_CHECK;
  RogueValue_List__add__Value( ROGUE_ARG(THIS->data), value_0 );
  return (RogueClassValueList*)(THIS);
}

RogueInt32 RogueValueList__count( RogueClassValueList* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueInt32)(THIS->data->count);
}

RogueClassValue* RogueValueList__first___Function_Value_RETURNSLogical_( RogueClassValueList* THIS, RogueClass_Function_Value_RETURNSLogical_* query_0 )
{
  ROGUE_GC_CHECK;
  {
    ROGUE_DEF_LOCAL_REF(RogueValue_List*,_auto_2284_0,(THIS->data));
    RogueInt32 _auto_2285_0 = (0);
    RogueInt32 _auto_2286_0 = (((_auto_2284_0->count) - (1)));
    for (;ROGUE_COND(((_auto_2285_0) <= (_auto_2286_0)));++_auto_2285_0)
    {
      ROGUE_GC_CHECK;
      ROGUE_DEF_LOCAL_REF(RogueClassValue*,v_0,(((RogueClassValue*)(_auto_2284_0->data->as_objects[_auto_2285_0]))));
      if (ROGUE_COND((Rogue_call_ROGUEM9( 13, query_0, v_0 ))))
      {
        return (RogueClassValue*)(v_0);
      }
    }
  }
  return (RogueClassValue*)(((RogueClassValue*)(((RogueClassUndefinedValue*)ROGUE_SINGLETON(UndefinedValue)))));
}

RogueClassValue* RogueValueList__get__Int32( RogueClassValueList* THIS, RogueInt32 index_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((((index_0) < (0))) || (((index_0) >= (THIS->data->count))))))
  {
    return (RogueClassValue*)(((RogueClassValue*)(((RogueClassUndefinedValue*)ROGUE_SINGLETON(UndefinedValue)))));
  }
  ROGUE_DEF_LOCAL_REF(RogueClassValue*,result_1,(((RogueClassValue*)(THIS->data->data->as_objects[index_0]))));
  if (ROGUE_COND(((void*)result_1) == ((void*)NULL)))
  {
    return (RogueClassValue*)(((RogueClassValue*)(((RogueClassUndefinedValue*)ROGUE_SINGLETON(UndefinedValue)))));
  }
  return (RogueClassValue*)(result_1);
}

RogueLogical RogueValueList__is_collection( RogueClassValueList* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(true);
}

RogueLogical RogueValueList__is_empty( RogueClassValueList* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(((THIS->data->count) == (0)));
}

RogueLogical RogueValueList__is_list( RogueClassValueList* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(true);
}

RogueLogical RogueValueList__to_Logical( RogueClassValueList* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(true);
}

RogueStringBuilder* RogueValueList__to_json__StringBuilder_Int32( RogueClassValueList* THIS, RogueStringBuilder* buffer_0, RogueInt32 flags_1 )
{
  ROGUE_GC_CHECK;
  RogueLogical pretty_print_2 = (((!!(((flags_1) & (1)))) && (((((RogueValue__is_complex( ROGUE_ARG(((RogueClassValue*)THIS)) )))) || (!!(((flags_1) & (2))))))));
  RogueStringBuilder__print__Character( buffer_0, (RogueCharacter)'[' );
  if (ROGUE_COND(pretty_print_2))
  {
    RogueStringBuilder__println( buffer_0 );
    buffer_0->indent += 2;
  }
  RogueLogical first_3 = (true);
  {
    ROGUE_DEF_LOCAL_REF(RogueValue_List*,_auto_2312_0,(THIS->data));
    RogueInt32 _auto_2313_0 = (0);
    RogueInt32 _auto_2314_0 = (((_auto_2312_0->count) - (1)));
    for (;ROGUE_COND(((_auto_2313_0) <= (_auto_2314_0)));++_auto_2313_0)
    {
      ROGUE_GC_CHECK;
      ROGUE_DEF_LOCAL_REF(RogueClassValue*,value_0,(((RogueClassValue*)(_auto_2312_0->data->as_objects[_auto_2313_0]))));
      if (ROGUE_COND(first_3))
      {
        first_3 = ((RogueLogical)(false));
      }
      else
      {
        if (ROGUE_COND(!(!!(((flags_1) & (2))))))
        {
          RogueStringBuilder__print__Character( buffer_0, (RogueCharacter)',' );
        }
        if (ROGUE_COND(pretty_print_2))
        {
          RogueStringBuilder__println( buffer_0 );
        }
      }
      if (ROGUE_COND(((((void*)value_0) != ((void*)NULL)) && ((((RogueOptionalValue__operator__Value( value_0 ))) || ((Rogue_call_ROGUEM5( 59, value_0 ))))))))
      {
        Rogue_call_ROGUEM8( 159, value_0, buffer_0, flags_1 );
      }
      else
      {
        RogueStringBuilder__print__String( buffer_0, Rogue_literal_strings[6] );
      }
    }
  }
  if (ROGUE_COND(pretty_print_2))
  {
    RogueStringBuilder__println( buffer_0 );
    buffer_0->indent -= 2;
  }
  RogueStringBuilder__print__Character( buffer_0, (RogueCharacter)']' );
  return (RogueStringBuilder*)(buffer_0);
}

RogueClassValueList* RogueValueList__init__Int32( RogueClassValueList* THIS, RogueInt32 initial_capacity_0 )
{
  ROGUE_GC_CHECK;
  THIS->data = ((RogueValue_List*)(RogueValue_List__init__Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueValue_List*,ROGUE_CREATE_OBJECT(Value_List))), initial_capacity_0 )));
  return (RogueClassValueList*)(THIS);
}

RogueValue_List* RogueValue_List__init_object( RogueValue_List* THIS )
{
  ROGUE_GC_CHECK;
  RogueGenericList__init_object( ROGUE_ARG(((RogueClassGenericList*)THIS)) );
  return (RogueValue_List*)(THIS);
}

RogueValue_List* RogueValue_List__init( RogueValue_List* THIS )
{
  ROGUE_GC_CHECK;
  RogueValue_List__init__Int32( ROGUE_ARG(THIS), 0 );
  return (RogueValue_List*)(THIS);
}

RogueString* RogueValue_List__to_String( RogueValue_List* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[293] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(((RogueString*)(RogueValue_List__join__String( ROGUE_ARG(THIS), Rogue_literal_strings[15] )))) ))) )))), Rogue_literal_strings[294] )))) ))));
}

RogueString* RogueValue_List__type_name( RogueValue_List* THIS )
{
  return (RogueString*)(Rogue_literal_strings[378]);
}

RogueValue_List* RogueValue_List__init__Int32( RogueValue_List* THIS, RogueInt32 initial_capacity_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((initial_capacity_0) != (((RogueValue_List__capacity( ROGUE_ARG(THIS) )))))))
  {
    THIS->data = RogueType_create_array( initial_capacity_0, sizeof(RogueClassValue*), true, 24 );
  }
  RogueValue_List__clear( ROGUE_ARG(THIS) );
  return (RogueValue_List*)(THIS);
}

void RogueValue_List__add__Value( RogueValue_List* THIS, RogueClassValue* value_0 )
{
  ROGUE_GC_CHECK;
  RogueValue_List__set__Int32_Value( ROGUE_ARG(((RogueValue_List*)(RogueValue_List__reserve__Int32( ROGUE_ARG(THIS), 1 )))), ROGUE_ARG(THIS->count), value_0 );
  THIS->count = ((THIS->count) + (1));
}

RogueInt32 RogueValue_List__capacity( RogueValue_List* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(!!(THIS->data))))
  {
    return (RogueInt32)(0);
  }
  return (RogueInt32)(THIS->data->count);
}

void RogueValue_List__clear( RogueValue_List* THIS )
{
  ROGUE_GC_CHECK;
  RogueValue_List__discard_from__Int32( ROGUE_ARG(THIS), 0 );
}

void RogueValue_List__discard_from__Int32( RogueValue_List* THIS, RogueInt32 index_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!!(THIS->data)))
  {
    RogueInt32 n_1 = (((THIS->count) - (index_0)));
    if (ROGUE_COND(((n_1) > (0))))
    {
      RogueArray__zero__Int32_Int32( ROGUE_ARG(((RogueArray*)THIS->data)), index_0, n_1 );
      THIS->count = index_0;
    }
  }
}

RogueClassValue* RogueValue_List__get__Int32( RogueValue_List* THIS, RogueInt32 index_0 )
{
  return (RogueClassValue*)(((RogueClassValue*)(THIS->data->as_objects[index_0])));
}

RogueString* RogueValue_List__join__String( RogueValue_List* THIS, RogueString* separator_0 )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueStringBuilder*,builder_1,(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))));
  RogueLogical first_2 = (true);
  {
    ROGUE_DEF_LOCAL_REF(RogueValue_List*,_auto_2258_0,(THIS));
    RogueInt32 _auto_2259_0 = (0);
    RogueInt32 _auto_2260_0 = (((_auto_2258_0->count) - (1)));
    for (;ROGUE_COND(((_auto_2259_0) <= (_auto_2260_0)));++_auto_2259_0)
    {
      ROGUE_GC_CHECK;
      ROGUE_DEF_LOCAL_REF(RogueClassValue*,item_0,(((RogueClassValue*)(_auto_2258_0->data->as_objects[_auto_2259_0]))));
      if (ROGUE_COND(first_2))
      {
        first_2 = ((RogueLogical)(false));
      }
      else
      {
        RogueStringBuilder__print__String( builder_1, separator_0 );
      }
      RogueStringBuilder__print__Object( builder_1, ROGUE_ARG(((RogueObject*)(item_0))) );
    }
  }
  return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( builder_1 ))));
}

RogueValue_List* RogueValue_List__reserve__Int32( RogueValue_List* THIS, RogueInt32 additional_elements_0 )
{
  ROGUE_GC_CHECK;
  RogueInt32 required_capacity_1 = (((THIS->count) + (additional_elements_0)));
  if (ROGUE_COND(((required_capacity_1) == (0))))
  {
    return (RogueValue_List*)(THIS);
  }
  if (ROGUE_COND(!(!!(THIS->data))))
  {
    if (ROGUE_COND(((required_capacity_1) == (1))))
    {
      required_capacity_1 = ((RogueInt32)(10));
    }
    THIS->data = RogueType_create_array( required_capacity_1, sizeof(RogueClassValue*), true, 24 );
  }
  else if (ROGUE_COND(((required_capacity_1) > (THIS->data->count))))
  {
    RogueInt32 cap_2 = (((RogueValue_List__capacity( ROGUE_ARG(THIS) ))));
    if (ROGUE_COND(((required_capacity_1) < (((cap_2) + (cap_2))))))
    {
      required_capacity_1 = ((RogueInt32)(((cap_2) + (cap_2))));
    }
    ROGUE_DEF_LOCAL_REF(RogueArray*,new_data_3,(RogueType_create_array( required_capacity_1, sizeof(RogueClassValue*), true, 24 )));
    RogueArray_set(new_data_3,0,((RogueArray*)(THIS->data)),0,-1);
    THIS->data = new_data_3;
  }
  return (RogueValue_List*)(THIS);
}

void RogueValue_List__set__Int32_Value( RogueValue_List* THIS, RogueInt32 index_0, RogueClassValue* new_value_1 )
{
  ROGUE_GC_CHECK;
  THIS->data->as_objects[index_0] = new_value_1;
}

RogueString* RogueArray_Value___type_name( RogueArray* THIS )
{
  return (RogueString*)(Rogue_literal_strings[388]);
}

RogueClassJSON* RogueJSON__init_object( RogueClassJSON* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  return (RogueClassJSON*)(THIS);
}

RogueString* RogueJSON__type_name( RogueClassJSON* THIS )
{
  return (RogueString*)(Rogue_literal_strings[343]);
}

RogueClassJSONParser* RogueJSONParser__init_object( RogueClassJSONParser* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  return (RogueClassJSONParser*)(THIS);
}

RogueString* RogueJSONParser__type_name( RogueClassJSONParser* THIS )
{
  return (RogueString*)(Rogue_literal_strings[344]);
}

RogueClassJSONParser* RogueJSONParser__init__String( RogueClassJSONParser* THIS, RogueString* json_0 )
{
  ROGUE_GC_CHECK;
  THIS->reader = ((RogueClassScanner*)(RogueScanner__init__String_Int32_Logical( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassScanner*,ROGUE_CREATE_OBJECT(Scanner))), json_0, 0, false )));
  return (RogueClassJSONParser*)(THIS);
}

RogueClassValue* RogueJSONParser__parse_value( RogueClassJSONParser* THIS )
{
  ROGUE_GC_CHECK;
  RogueJSONParser__consume_whitespace_and_eols( ROGUE_ARG(THIS) );
  if (ROGUE_COND(!(((RogueScanner__has_another( ROGUE_ARG(THIS->reader) ))))))
  {
    return (RogueClassValue*)(((RogueClassValue*)(((RogueClassUndefinedValue*)ROGUE_SINGLETON(UndefinedValue)))));
  }
  RogueCharacter ch_0 = (((RogueScanner__peek( ROGUE_ARG(THIS->reader) ))));
  if (ROGUE_COND(((ch_0) == ((RogueCharacter)'{'))))
  {
    return (RogueClassValue*)(((RogueClassValue*)(RogueJSONParser__parse_table__Character_Character( ROGUE_ARG(THIS), (RogueCharacter)'{', (RogueCharacter)'}' ))));
  }
  if (ROGUE_COND(((ch_0) == ((RogueCharacter)'['))))
  {
    return (RogueClassValue*)(((RogueClassValue*)(RogueJSONParser__parse_list__Character_Character( ROGUE_ARG(THIS), (RogueCharacter)'[', (RogueCharacter)']' ))));
  }
  if (ROGUE_COND(((ch_0) == ((RogueCharacter)'-'))))
  {
    return (RogueClassValue*)(((RogueClassValue*)(RogueJSONParser__parse_number( ROGUE_ARG(THIS) ))));
  }
  if (ROGUE_COND(((((ch_0) >= ((RogueCharacter)'0'))) && (((ch_0) <= ((RogueCharacter)'9'))))))
  {
    return (RogueClassValue*)(((RogueClassValue*)(RogueJSONParser__parse_number( ROGUE_ARG(THIS) ))));
  }
  if (ROGUE_COND(((((ch_0) == ((RogueCharacter)'"'))) || (((ch_0) == ((RogueCharacter)'\''))))))
  {
    ROGUE_DEF_LOCAL_REF(RogueString*,result_1,(((RogueString*)(RogueJSONParser__parse_string( ROGUE_ARG(THIS) )))));
    if (ROGUE_COND(((((RogueString__count( result_1 )))) == (0))))
    {
      return (RogueClassValue*)(((RogueClassValue*)(RogueStringValue_empty_string)));
    }
    RogueCharacter first_ch_2 = (((RogueString__get__Int32( result_1, 0 ))));
    if (ROGUE_COND(((((first_ch_2) == ((RogueCharacter)'t'))) && ((RogueString__operatorEQUALSEQUALS__String_String( result_1, Rogue_literal_strings[130] ))))))
    {
      return (RogueClassValue*)(((RogueClassValue*)(RogueLogicalValue_true_value)));
    }
    if (ROGUE_COND(((((first_ch_2) == ((RogueCharacter)'f'))) && ((RogueString__operatorEQUALSEQUALS__String_String( result_1, Rogue_literal_strings[131] ))))))
    {
      return (RogueClassValue*)(((RogueClassValue*)(RogueLogicalValue_false_value)));
    }
    if (ROGUE_COND(((((first_ch_2) == ((RogueCharacter)'n'))) && ((RogueString__operatorEQUALSEQUALS__String_String( result_1, Rogue_literal_strings[6] ))))))
    {
      return (RogueClassValue*)(((RogueClassValue*)(((RogueClassNullValue*)ROGUE_SINGLETON(NullValue)))));
    }
    return (RogueClassValue*)(((RogueClassValue*)(((RogueClassStringValue*)(RogueStringValue__init__String( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassStringValue*,ROGUE_CREATE_OBJECT(StringValue))), result_1 ))))));
  }
  else if (ROGUE_COND(((RogueJSONParser__next_is_identifier( ROGUE_ARG(THIS) )))))
  {
    ROGUE_DEF_LOCAL_REF(RogueString*,result_3,(((RogueString*)(RogueJSONParser__parse_identifier( ROGUE_ARG(THIS) )))));
    if (ROGUE_COND(((((RogueString__count( result_3 )))) == (0))))
    {
      return (RogueClassValue*)(((RogueClassValue*)(RogueStringValue_empty_string)));
    }
    RogueCharacter first_ch_4 = (((RogueString__get__Int32( result_3, 0 ))));
    if (ROGUE_COND(((((first_ch_4) == ((RogueCharacter)'t'))) && ((RogueString__operatorEQUALSEQUALS__String_String( result_3, Rogue_literal_strings[130] ))))))
    {
      return (RogueClassValue*)(((RogueClassValue*)(RogueLogicalValue_true_value)));
    }
    if (ROGUE_COND(((((first_ch_4) == ((RogueCharacter)'f'))) && ((RogueString__operatorEQUALSEQUALS__String_String( result_3, Rogue_literal_strings[131] ))))))
    {
      return (RogueClassValue*)(((RogueClassValue*)(RogueLogicalValue_false_value)));
    }
    if (ROGUE_COND(((((first_ch_4) == ((RogueCharacter)'n'))) && ((RogueString__operatorEQUALSEQUALS__String_String( result_3, Rogue_literal_strings[6] ))))))
    {
      return (RogueClassValue*)(((RogueClassValue*)(((RogueClassNullValue*)ROGUE_SINGLETON(NullValue)))));
    }
    return (RogueClassValue*)(((RogueClassValue*)(((RogueClassStringValue*)(RogueStringValue__init__String( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassStringValue*,ROGUE_CREATE_OBJECT(StringValue))), result_3 ))))));
  }
  else
  {
    return (RogueClassValue*)(((RogueClassValue*)(((RogueClassUndefinedValue*)ROGUE_SINGLETON(UndefinedValue)))));
  }
}

RogueClassValue* RogueJSONParser__parse_table__Character_Character( RogueClassJSONParser* THIS, RogueCharacter open_ch_0, RogueCharacter close_ch_1 )
{
  ROGUE_GC_CHECK;
  RogueJSONParser__consume_whitespace_and_eols( ROGUE_ARG(THIS) );
  if (ROGUE_COND(!(((RogueScanner__consume__Character( ROGUE_ARG(THIS->reader), open_ch_0 ))))))
  {
    return (RogueClassValue*)(((RogueClassValue*)(((RogueClassUndefinedValue*)ROGUE_SINGLETON(UndefinedValue)))));
  }
  RogueJSONParser__consume_whitespace_and_eols( ROGUE_ARG(THIS) );
  ROGUE_DEF_LOCAL_REF(RogueClassValueTable*,table_2,(((RogueClassValueTable*)(RogueValueTable__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassValueTable*,ROGUE_CREATE_OBJECT(ValueTable))) )))));
  if (ROGUE_COND(((RogueScanner__consume__Character( ROGUE_ARG(THIS->reader), close_ch_1 )))))
  {
    return (RogueClassValue*)(((RogueClassValue*)(table_2)));
  }
  RogueInt32 prev_pos_3 = (THIS->reader->position);
  RogueLogical first_4 = (true);
  while (ROGUE_COND(((((first_4) || (((RogueScanner__consume__Character( ROGUE_ARG(THIS->reader), (RogueCharacter)',' )))))) || (((((((RogueScanner__has_another( ROGUE_ARG(THIS->reader) )))) && (((((RogueScanner__peek( ROGUE_ARG(THIS->reader) )))) != (close_ch_1))))) && (((THIS->reader->position) > (prev_pos_3))))))))
  {
    ROGUE_GC_CHECK;
    first_4 = ((RogueLogical)(false));
    prev_pos_3 = ((RogueInt32)(THIS->reader->position));
    RogueJSONParser__consume_whitespace_and_eols( ROGUE_ARG(THIS) );
    if (ROGUE_COND(((RogueJSONParser__next_is_identifier( ROGUE_ARG(THIS) )))))
    {
      ROGUE_DEF_LOCAL_REF(RogueString*,key_5,(((RogueString*)(RogueJSONParser__parse_identifier( ROGUE_ARG(THIS) )))));
      RogueJSONParser__consume_whitespace_and_eols( ROGUE_ARG(THIS) );
      if (ROGUE_COND(!!(((RogueString__count( key_5 ))))))
      {
        if (ROGUE_COND(((RogueScanner__consume__Character( ROGUE_ARG(THIS->reader), (RogueCharacter)':' )))))
        {
          RogueJSONParser__consume_whitespace_and_eols( ROGUE_ARG(THIS) );
          ROGUE_DEF_LOCAL_REF(RogueClassValue*,value_6,(((RogueClassValue*)(RogueJSONParser__parse_value( ROGUE_ARG(THIS) )))));
          RogueValueTable__set__String_Value( table_2, key_5, value_6 );
        }
        else
        {
          RogueValueTable__set__String_Value( table_2, key_5, ROGUE_ARG((RogueValue__create__Logical( true ))) );
        }
        RogueJSONParser__consume_whitespace_and_eols( ROGUE_ARG(THIS) );
      }
    }
  }
  if (ROGUE_COND(!(((RogueScanner__consume__Character( ROGUE_ARG(THIS->reader), close_ch_1 ))))))
  {
    throw ((RogueException*)(RogueJSONParseError___throw( ROGUE_ARG(((RogueClassJSONParseError*)(RogueJSONParseError__init__String( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassJSONParseError*,ROGUE_CREATE_OBJECT(JSONParseError))), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[128] )))), ROGUE_ARG(((RogueString*)(RogueString__operatorPLUS__Character( ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], Rogue_literal_strings[0] ))), close_ch_1 )))) )))), Rogue_literal_strings[129] )))) )))) )))) )));
  }
  return (RogueClassValue*)(((RogueClassValue*)(table_2)));
}

RogueClassValue* RogueJSONParser__parse_list__Character_Character( RogueClassJSONParser* THIS, RogueCharacter open_ch_0, RogueCharacter close_ch_1 )
{
  ROGUE_GC_CHECK;
  RogueJSONParser__consume_whitespace_and_eols( ROGUE_ARG(THIS) );
  if (ROGUE_COND(!(((RogueScanner__consume__Character( ROGUE_ARG(THIS->reader), open_ch_0 ))))))
  {
    return (RogueClassValue*)(((RogueClassValue*)(((RogueClassUndefinedValue*)ROGUE_SINGLETON(UndefinedValue)))));
  }
  RogueJSONParser__consume_whitespace_and_eols( ROGUE_ARG(THIS) );
  ROGUE_DEF_LOCAL_REF(RogueClassValueList*,list_2,(((RogueClassValueList*)(RogueValueList__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassValueList*,ROGUE_CREATE_OBJECT(ValueList))) )))));
  if (ROGUE_COND(((RogueScanner__consume__Character( ROGUE_ARG(THIS->reader), close_ch_1 )))))
  {
    return (RogueClassValue*)(((RogueClassValue*)(list_2)));
  }
  RogueInt32 prev_pos_3 = (THIS->reader->position);
  RogueLogical first_4 = (true);
  while (ROGUE_COND(((((first_4) || (((RogueScanner__consume__Character( ROGUE_ARG(THIS->reader), (RogueCharacter)',' )))))) || (((((((RogueScanner__has_another( ROGUE_ARG(THIS->reader) )))) && (((((RogueScanner__peek( ROGUE_ARG(THIS->reader) )))) != (close_ch_1))))) && (((THIS->reader->position) > (prev_pos_3))))))))
  {
    ROGUE_GC_CHECK;
    first_4 = ((RogueLogical)(false));
    prev_pos_3 = ((RogueInt32)(THIS->reader->position));
    RogueJSONParser__consume_whitespace_and_eols( ROGUE_ARG(THIS) );
    if (ROGUE_COND(((((RogueScanner__peek( ROGUE_ARG(THIS->reader) )))) == (close_ch_1))))
    {
      goto _auto_482;
    }
    Rogue_call_ROGUEM4( 19, list_2, ROGUE_ARG(((RogueClassValue*)(RogueJSONParser__parse_value( ROGUE_ARG(THIS) )))) );
    RogueJSONParser__consume_whitespace_and_eols( ROGUE_ARG(THIS) );
  }
  _auto_482:;
  if (ROGUE_COND(!(((RogueScanner__consume__Character( ROGUE_ARG(THIS->reader), close_ch_1 ))))))
  {
    throw ((RogueException*)(RogueJSONParseError___throw( ROGUE_ARG(((RogueClassJSONParseError*)(RogueJSONParseError__init__String( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassJSONParseError*,ROGUE_CREATE_OBJECT(JSONParseError))), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[128] )))), ROGUE_ARG(((RogueString*)(RogueString__operatorPLUS__Character( ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], Rogue_literal_strings[0] ))), close_ch_1 )))) )))), Rogue_literal_strings[129] )))) )))) )))) )));
  }
  return (RogueClassValue*)(((RogueClassValue*)(list_2)));
}

RogueString* RogueJSONParser__parse_string( RogueClassJSONParser* THIS )
{
  ROGUE_GC_CHECK;
  RogueJSONParser__consume_whitespace_and_eols( ROGUE_ARG(THIS) );
  RogueCharacter terminator_0 = ((RogueCharacter)'"');
  if (ROGUE_COND(((RogueScanner__consume__Character( ROGUE_ARG(THIS->reader), (RogueCharacter)'"' )))))
  {
    terminator_0 = ((RogueCharacter)((RogueCharacter)'"'));
  }
  else if (ROGUE_COND(((RogueScanner__consume__Character( ROGUE_ARG(THIS->reader), (RogueCharacter)'\'' )))))
  {
    terminator_0 = ((RogueCharacter)((RogueCharacter)'\''));
  }
  if (ROGUE_COND(!(((RogueScanner__has_another( ROGUE_ARG(THIS->reader) ))))))
  {
    return (RogueString*)(Rogue_literal_strings[0]);
  }
  {
    ROGUE_DEF_LOCAL_REF(RogueException*,_auto_466_0,0);
    ROGUE_DEF_LOCAL_REF(RogueClassStringBuilderPool*,_auto_465_0,(RogueStringBuilder_pool));
    ROGUE_DEF_LOCAL_REF(RogueString*,_auto_467_0,0);
    ROGUE_DEF_LOCAL_REF(RogueStringBuilder*,buffer_0,(((RogueStringBuilder*)(RogueStringBuilderPool__on_use( _auto_465_0 )))));
    Rogue_ignore_unused(buffer_0);

    ROGUE_TRY
    {
      RogueCharacter ch_1 = (((RogueScanner__read( ROGUE_ARG(THIS->reader) ))));
      while (ROGUE_COND(((((RogueScanner__has_another( ROGUE_ARG(THIS->reader) )))) && (((ch_1) != (terminator_0))))))
      {
        ROGUE_GC_CHECK;
        if (ROGUE_COND(((ch_1) == ((RogueCharacter)'\\'))))
        {
          ch_1 = ((RogueCharacter)(((RogueScanner__read( ROGUE_ARG(THIS->reader) )))));
          if (ROGUE_COND(((ch_1) == ((RogueCharacter)'b'))))
          {
            RogueStringBuilder__print__Character( buffer_0, (RogueCharacter)8 );
          }
          else if (ROGUE_COND(((ch_1) == ((RogueCharacter)'f'))))
          {
            RogueStringBuilder__print__Character( buffer_0, (RogueCharacter)12 );
          }
          else if (ROGUE_COND(((ch_1) == ((RogueCharacter)'n'))))
          {
            RogueStringBuilder__print__Character( buffer_0, (RogueCharacter)10 );
          }
          else if (ROGUE_COND(((ch_1) == ((RogueCharacter)'r'))))
          {
            RogueStringBuilder__print__Character( buffer_0, (RogueCharacter)13 );
          }
          else if (ROGUE_COND(((ch_1) == ((RogueCharacter)'t'))))
          {
            RogueStringBuilder__print__Character( buffer_0, (RogueCharacter)9 );
          }
          else if (ROGUE_COND(((ch_1) == ((RogueCharacter)'u'))))
          {
            RogueStringBuilder__print__Character( buffer_0, ROGUE_ARG(((RogueJSONParser__parse_hex_quad( ROGUE_ARG(THIS) )))) );
          }
          else
          {
            RogueStringBuilder__print__Character( buffer_0, ch_1 );
          }
        }
        else
        {
          RogueStringBuilder__print__Character( buffer_0, ch_1 );
        }
        ch_1 = ((RogueCharacter)(((RogueScanner__read( ROGUE_ARG(THIS->reader) )))));
      }
      {
        _auto_467_0 = ((RogueString*)(((RogueString*)(RogueString__consolidated( ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( buffer_0 )))) )))));
        goto _auto_474;
      }
    }
    ROGUE_CATCH( RogueException,_auto_468_0 )
    {
      _auto_466_0 = ((RogueException*)(_auto_468_0));
    }
    ROGUE_END_TRY
    _auto_474:;
    RogueStringBuilderPool__on_end_use__StringBuilder( _auto_465_0, buffer_0 );
    if (ROGUE_COND(!!(_auto_466_0)))
    {
      throw ((RogueException*)Rogue_call_ROGUEM0( 16, _auto_466_0 ));
    }
    return (RogueString*)(_auto_467_0);
  }
}

RogueCharacter RogueJSONParser__parse_hex_quad( RogueClassJSONParser* THIS )
{
  ROGUE_GC_CHECK;
  RogueInt32 code_0 = (0);
  {
    RogueInt32 i_1 = (1);
    RogueInt32 _auto_451_2 = (4);
    for (;ROGUE_COND(((i_1) <= (_auto_451_2)));++i_1)
    {
      ROGUE_GC_CHECK;
      if (ROGUE_COND(((RogueScanner__has_another( ROGUE_ARG(THIS->reader) )))))
      {
        code_0 = ((RogueInt32)(((((code_0) << (4))) | (((RogueCharacter__to_number__Int32( ROGUE_ARG(((RogueScanner__read( ROGUE_ARG(THIS->reader) )))), 16 )))))));
      }
    }
  }
  return (RogueCharacter)(((RogueCharacter)(code_0)));
}

RogueString* RogueJSONParser__parse_identifier( RogueClassJSONParser* THIS )
{
  ROGUE_GC_CHECK;
  RogueJSONParser__consume_whitespace_and_eols( ROGUE_ARG(THIS) );
  RogueCharacter ch_0 = (((RogueScanner__peek( ROGUE_ARG(THIS->reader) ))));
  if (ROGUE_COND(((((ch_0) == ((RogueCharacter)'"'))) || (((ch_0) == ((RogueCharacter)'\''))))))
  {
    return (RogueString*)(((RogueString*)(RogueJSONParser__parse_string( ROGUE_ARG(THIS) ))));
  }
  else
  {
    {
      ROGUE_DEF_LOCAL_REF(RogueException*,_auto_476_0,0);
      ROGUE_DEF_LOCAL_REF(RogueClassStringBuilderPool*,_auto_475_0,(RogueStringBuilder_pool));
      ROGUE_DEF_LOCAL_REF(RogueString*,_auto_477_0,0);
      ROGUE_DEF_LOCAL_REF(RogueStringBuilder*,buffer_0,(((RogueStringBuilder*)(RogueStringBuilderPool__on_use( _auto_475_0 )))));
      Rogue_ignore_unused(buffer_0);

      ROGUE_TRY
      {
        RogueLogical finished_1 = (false);
        while (ROGUE_COND(((!(finished_1)) && (((RogueScanner__has_another( ROGUE_ARG(THIS->reader) )))))))
        {
          ROGUE_GC_CHECK;
          if (ROGUE_COND(((RogueCharacter__is_identifier__Logical_Logical( ch_0, false, true )))))
          {
            RogueScanner__read( ROGUE_ARG(THIS->reader) );
            RogueStringBuilder__print__Character( buffer_0, ch_0 );
            ch_0 = ((RogueCharacter)(((RogueScanner__peek( ROGUE_ARG(THIS->reader) )))));
          }
          else
          {
            finished_1 = ((RogueLogical)(true));
          }
        }
        if (ROGUE_COND(((buffer_0->count) == (0))))
        {
          throw ((RogueException*)(RogueJSONParseError___throw( ROGUE_ARG(((RogueClassJSONParseError*)(RogueJSONParseError__init__String( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassJSONParseError*,ROGUE_CREATE_OBJECT(JSONParseError))), Rogue_literal_strings[127] )))) )));
        }
        {
          _auto_477_0 = ((RogueString*)(((RogueString*)(RogueString__consolidated( ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( buffer_0 )))) )))));
          goto _auto_480;
        }
      }
      ROGUE_CATCH( RogueException,_auto_478_0 )
      {
        _auto_476_0 = ((RogueException*)(_auto_478_0));
      }
      ROGUE_END_TRY
      _auto_480:;
      RogueStringBuilderPool__on_end_use__StringBuilder( _auto_475_0, buffer_0 );
      if (ROGUE_COND(!!(_auto_476_0)))
      {
        throw ((RogueException*)Rogue_call_ROGUEM0( 16, _auto_476_0 ));
      }
      return (RogueString*)(_auto_477_0);
    }
  }
}

RogueLogical RogueJSONParser__next_is_identifier( RogueClassJSONParser* THIS )
{
  ROGUE_GC_CHECK;
  RogueCharacter ch_0 = (((RogueScanner__peek( ROGUE_ARG(THIS->reader) ))));
  if (ROGUE_COND(((RogueCharacter__is_identifier__Logical_Logical( ch_0, true, true )))))
  {
    return (RogueLogical)(true);
  }
  return (RogueLogical)(((((ch_0) == ((RogueCharacter)'"'))) || (((ch_0) == ((RogueCharacter)'\'')))));
}

RogueClassValue* RogueJSONParser__parse_number( RogueClassJSONParser* THIS )
{
  ROGUE_GC_CHECK;
  RogueJSONParser__consume_whitespace_and_eols( ROGUE_ARG(THIS) );
  RogueReal64 sign_0 = (1.0);
  if (ROGUE_COND(((RogueScanner__consume__Character( ROGUE_ARG(THIS->reader), (RogueCharacter)'-' )))))
  {
    sign_0 = ((RogueReal64)(-1.0));
    RogueJSONParser__consume_whitespace_and_eols( ROGUE_ARG(THIS) );
  }
  RogueReal64 n_1 = (0.0);
  RogueCharacter ch_2 = (((RogueScanner__peek( ROGUE_ARG(THIS->reader) ))));
  while (ROGUE_COND(((((((RogueScanner__has_another( ROGUE_ARG(THIS->reader) )))) && (((ch_2) >= ((RogueCharacter)'0'))))) && (((ch_2) <= ((RogueCharacter)'9'))))))
  {
    ROGUE_GC_CHECK;
    RogueScanner__read( ROGUE_ARG(THIS->reader) );
    n_1 = ((RogueReal64)(((((n_1) * (10.0))) + (((RogueReal64)(((ch_2) - ((RogueCharacter)'0'))))))));
    ch_2 = ((RogueCharacter)(((RogueScanner__peek( ROGUE_ARG(THIS->reader) )))));
  }
  if (ROGUE_COND(((RogueScanner__consume__Character( ROGUE_ARG(THIS->reader), (RogueCharacter)'.' )))))
  {
    RogueReal64 decimal_3 = (0.0);
    RogueReal64 power_4 = (0.0);
    ch_2 = ((RogueCharacter)(((RogueScanner__peek( ROGUE_ARG(THIS->reader) )))));
    while (ROGUE_COND(((((((RogueScanner__has_another( ROGUE_ARG(THIS->reader) )))) && (((ch_2) >= ((RogueCharacter)'0'))))) && (((ch_2) <= ((RogueCharacter)'9'))))))
    {
      ROGUE_GC_CHECK;
      RogueScanner__read( ROGUE_ARG(THIS->reader) );
      decimal_3 = ((RogueReal64)(((((decimal_3) * (10.0))) + (((RogueReal64)(((ch_2) - ((RogueCharacter)'0'))))))));
      power_4 += 1.0;
      ch_2 = ((RogueCharacter)(((RogueScanner__peek( ROGUE_ARG(THIS->reader) )))));
    }
    n_1 += ((decimal_3) / (((RogueReal64) pow((double)10.0, (double)power_4))));
  }
  if (ROGUE_COND(((((RogueScanner__consume__Character( ROGUE_ARG(THIS->reader), (RogueCharacter)'e' )))) || (((RogueScanner__consume__Character( ROGUE_ARG(THIS->reader), (RogueCharacter)'E' )))))))
  {
    RogueLogical negexp_5 = (false);
    if (ROGUE_COND(((!(((RogueScanner__consume__Character( ROGUE_ARG(THIS->reader), (RogueCharacter)'+' ))))) && (((RogueScanner__consume__Character( ROGUE_ARG(THIS->reader), (RogueCharacter)'-' )))))))
    {
      negexp_5 = ((RogueLogical)(true));
    }
    RogueReal64 power_6 = (0.0);
    ch_2 = ((RogueCharacter)(((RogueScanner__peek( ROGUE_ARG(THIS->reader) )))));
    while (ROGUE_COND(((((((RogueScanner__has_another( ROGUE_ARG(THIS->reader) )))) && (((ch_2) >= ((RogueCharacter)'0'))))) && (((ch_2) <= ((RogueCharacter)'9'))))))
    {
      ROGUE_GC_CHECK;
      RogueScanner__read( ROGUE_ARG(THIS->reader) );
      power_6 = ((RogueReal64)(((((power_6) * (10.0))) + (((RogueReal64)(((ch_2) - ((RogueCharacter)'0'))))))));
      ch_2 = ((RogueCharacter)(((RogueScanner__peek( ROGUE_ARG(THIS->reader) )))));
    }
    if (ROGUE_COND(negexp_5))
    {
      n_1 /= ((RogueReal64) pow((double)10.0, (double)power_6));
    }
    else
    {
      n_1 *= ((RogueReal64) pow((double)10.0, (double)power_6));
    }
  }
  n_1 = ((RogueReal64)(((n_1) * (sign_0))));
  return (RogueClassValue*)(((RogueClassValue*)(((RogueClassReal64Value*)(RogueReal64Value__init__Real64( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassReal64Value*,ROGUE_CREATE_OBJECT(Real64Value))), n_1 ))))));
}

void RogueJSONParser__consume_whitespace_and_eols( RogueClassJSONParser* THIS )
{
  ROGUE_GC_CHECK;
  while (ROGUE_COND(((((RogueScanner__consume_whitespace( ROGUE_ARG(THIS->reader) )))) || (((RogueScanner__consume_eols( ROGUE_ARG(THIS->reader) )))))))
  {
    ROGUE_GC_CHECK;
  }
}

RogueClassScanner* RogueScanner__init_object( RogueClassScanner* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  return (RogueClassScanner*)(THIS);
}

RogueString* RogueScanner__to_String( RogueClassScanner* THIS )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueCharacter_List*,buffer_0,(((RogueCharacter_List*)(RogueCharacter_List__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueCharacter_List*,ROGUE_CREATE_OBJECT(Character_List))) )))));
  while (ROGUE_COND(((RogueScanner__has_another( ROGUE_ARG(THIS) )))))
  {
    ROGUE_GC_CHECK;
    RogueCharacter_List__add__Character( buffer_0, ROGUE_ARG(((RogueScanner__read( ROGUE_ARG(THIS) )))) );
  }
  if (ROGUE_COND((false)))
  {
  }
  else
  {
    return (RogueString*)(((RogueString*)(RogueCharacter_List__to_String( buffer_0 ))));
  }
}

RogueString* RogueScanner__type_name( RogueClassScanner* THIS )
{
  return (RogueString*)(Rogue_literal_strings[345]);
}

RogueLogical RogueScanner__has_another( RogueClassScanner* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(((THIS->position) < (THIS->count)));
}

RogueCharacter RogueScanner__peek( RogueClassScanner* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((THIS->position) == (THIS->count))))
  {
    return (RogueCharacter)((RogueCharacter)0);
  }
  return (RogueCharacter)(THIS->data->data->as_characters[THIS->position]);
}

RogueCharacter RogueScanner__read( RogueClassScanner* THIS )
{
  ROGUE_GC_CHECK;
  RogueCharacter result_0 = (THIS->data->data->as_characters[THIS->position]);
  THIS->position = ((THIS->position) + (1));
  if (ROGUE_COND(((((RogueInt32)(result_0))) == (10))))
  {
    ++THIS->line;
    THIS->column = 1;
  }
  else
  {
    ++THIS->column;
  }
  return (RogueCharacter)(result_0);
}

RogueClassScanner* RogueScanner__init__String_Int32_Logical( RogueClassScanner* THIS, RogueString* _auto_460_0, RogueInt32 _auto_459_1, RogueLogical preserve_crlf_2 )
{
  ROGUE_GC_CHECK;
  THIS->source = _auto_460_0;
  THIS->spaces_per_tab = _auto_459_1;
  RogueInt32 tab_count_3 = (0);
  if (ROGUE_COND(!!(THIS->spaces_per_tab)))
  {
    tab_count_3 = ((RogueInt32)(((RogueString__count__Character( ROGUE_ARG(THIS->source), (RogueCharacter)9 )))));
  }
  RogueInt32 spaces_per_tab_4 = (THIS->spaces_per_tab);
  RogueInt32 new_count_5 = (((((((RogueString__count( ROGUE_ARG(THIS->source) )))) + (((tab_count_3) * (spaces_per_tab_4))))) - (tab_count_3)));
  THIS->data = ((RogueCharacter_List*)(RogueCharacter_List__init__Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueCharacter_List*,ROGUE_CREATE_OBJECT(Character_List))), new_count_5 )));
  char* src = THIS->source->utf8 - 1;
  RogueCharacter* dest = THIS->data->data->as_characters - 1;
  for (int i=THIS->source->character_count; --i>=0; )
  {
    RogueCharacter ch = (RogueCharacter) *(++src);
    if (ch == '\t')
    {
      if (spaces_per_tab_4)
      {
        for (int j=spaces_per_tab_4; --j>=0; ) *(++dest) = ' ';
      }
      else
      {
        *(++dest) = ch;
      }
    }
    else if (ch == '\r')
    {
      if (preserve_crlf_2) *(++dest) = '\r';
      else                --new_count_5;
    }
    else if (ch & 0x80)
    {
      if (ch & 0x20)
      {
        if (ch & 0x10)
        {
          ch  = ((ch&7)<<18) | ((*(++src) & 0x3F) << 12);
          ch |= (*(++src) & 0x3F) << 6;
          *(++dest) = ch | (*(++src) & 0x3F);
        }
        else
        {
          ch  = ((ch&15)<<12) | ((*(++src) & 0x3F) << 6);
          *(++dest) = ch | (*(++src) & 0x3F);
        }
      }
      else
      {
        *(++dest) = ((ch&31)<<6) | (*(++src) & 0x3F);
      }
    }
    else
    {
      *(++dest) = ch;
    }
  }

  THIS->data->count = new_count_5;
  THIS->count = new_count_5;
  THIS->line = 1;
  THIS->column = 1;
  THIS->position = 0;
  THIS->source = (RogueString__create__Character_List( ROGUE_ARG(THIS->data) ));
  return (RogueClassScanner*)(THIS);
}

RogueLogical RogueScanner__consume__Character( RogueClassScanner* THIS, RogueCharacter ch_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((((RogueScanner__peek( ROGUE_ARG(THIS) )))) != (ch_0))))
  {
    return (RogueLogical)(false);
  }
  RogueScanner__read( ROGUE_ARG(THIS) );
  return (RogueLogical)(true);
}

RogueLogical RogueScanner__consume_eols( RogueClassScanner* THIS )
{
  ROGUE_GC_CHECK;
  RogueLogical found_0 = (false);
  while (ROGUE_COND(((RogueScanner__consume__Character( ROGUE_ARG(THIS), (RogueCharacter)10 )))))
  {
    ROGUE_GC_CHECK;
    found_0 = ((RogueLogical)(true));
  }
  return (RogueLogical)(found_0);
}

RogueLogical RogueScanner__consume_whitespace( RogueClassScanner* THIS )
{
  ROGUE_GC_CHECK;
  RogueLogical found_0 = (false);
  while (ROGUE_COND(((((RogueScanner__consume__Character( ROGUE_ARG(THIS), (RogueCharacter)' ' )))) || (((RogueScanner__consume__Character( ROGUE_ARG(THIS), (RogueCharacter)9 )))))))
  {
    ROGUE_GC_CHECK;
    found_0 = ((RogueLogical)(true));
  }
  return (RogueLogical)(found_0);
}

RogueLogical RogueReader_Character___has_another( RogueObject* THIS )
{
  switch (THIS->type->index)
  {
    case 56:
      return RogueScanner__has_another( (RogueClassScanner*)THIS );
    case 61:
      return RogueConsole__has_another( (RogueClassConsole*)THIS );
    case 109:
      return RogueExtendedASCIIReader__has_another( (RogueClassExtendedASCIIReader*)THIS );
    case 110:
      return RogueUTF8Reader__has_another( (RogueClassUTF8Reader*)THIS );
    default:
      return 0;
  }
}

RogueCharacter RogueReader_Character___read( RogueObject* THIS )
{
  switch (THIS->type->index)
  {
    case 56:
      return RogueScanner__read( (RogueClassScanner*)THIS );
    case 61:
      return RogueConsole__read( (RogueClassConsole*)THIS );
    case 109:
      return RogueExtendedASCIIReader__read( (RogueClassExtendedASCIIReader*)THIS );
    case 110:
      return RogueUTF8Reader__read( (RogueClassUTF8Reader*)THIS );
    default:
      return 0;
  }
}

RogueClassStringConsolidationTable* RogueStringConsolidationTable__init_object( RogueClassStringConsolidationTable* THIS )
{
  ROGUE_GC_CHECK;
  RogueStringTable_String___init_object( ROGUE_ARG(((RogueClassStringTable_String_*)THIS)) );
  return (RogueClassStringConsolidationTable*)(THIS);
}

RogueString* RogueStringConsolidationTable__type_name( RogueClassStringConsolidationTable* THIS )
{
  return (RogueString*)(Rogue_literal_strings[444]);
}

RogueString* RogueStringConsolidationTable__get__String( RogueClassStringConsolidationTable* THIS, RogueString* st_0 )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueString*,result_1,((RogueTable_String_String___get__String( ROGUE_ARG(((RogueClassTable_String_String_*)THIS)), st_0 ))));
  if (ROGUE_COND(!!(result_1)))
  {
    return (RogueString*)(result_1);
  }
  RogueTable_String_String___set__String_String( ROGUE_ARG(((RogueClassTable_String_String_*)THIS)), st_0, st_0 );
  return (RogueString*)(st_0);
}

RogueClassFileWriter* RogueFileWriter__init_object( RogueClassFileWriter* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  THIS->buffer = ((RogueByte_List*)(RogueByte_List__init__Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueByte_List*,ROGUE_CREATE_OBJECT(Byte_List))), 1024 )));
  return (RogueClassFileWriter*)(THIS);
}

RogueString* RogueFileWriter__type_name( RogueClassFileWriter* THIS )
{
  return (RogueString*)(Rogue_literal_strings[347]);
}

void RogueFileWriter__close( RogueClassFileWriter* THIS )
{
  ROGUE_GC_CHECK;
  RogueFileWriter__flush( ROGUE_ARG(THIS) );
  if (ROGUE_COND(!!(THIS->fp)))
  {
    fclose( THIS->fp ); THIS->fp = 0;

    RogueSystem__sync_storage();
  }
}

void RogueFileWriter__flush( RogueClassFileWriter* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((((THIS->buffer->count) == (0))) || (!(!!(THIS->fp))))))
  {
    return;
  }
  fwrite( THIS->buffer->data->as_bytes, 1, THIS->buffer->count, THIS->fp );
  fflush( THIS->fp );

  RogueByte_List__clear( ROGUE_ARG(THIS->buffer) );
}

void RogueFileWriter__write__Byte( RogueClassFileWriter* THIS, RogueByte ch_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(!!(THIS->fp))))
  {
    return;
  }
  THIS->position = ((THIS->position) + (1));
  RogueByte_List__add__Byte( ROGUE_ARG(THIS->buffer), ch_0 );
  if (ROGUE_COND(((THIS->buffer->count) == (1024))))
  {
    RogueFileWriter__flush( ROGUE_ARG(THIS) );
  }
}

void RogueFileWriter__write__Byte_List( RogueClassFileWriter* THIS, RogueByte_List* bytes_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(!!(THIS->fp))))
  {
    return;
  }
  RogueFileWriter__flush( ROGUE_ARG(THIS) );
  THIS->position += bytes_0->count;
  fwrite( bytes_0->data->as_bytes, 1, bytes_0->count, THIS->fp );

}

RogueClassFileWriter* RogueFileWriter__init__String_Logical( RogueClassFileWriter* THIS, RogueString* _filepath_0, RogueLogical append_1 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(((RogueFileWriter__open__String_Logical( ROGUE_ARG(THIS), _filepath_0, append_1 ))))))
  {
    throw ((RogueException*)(RogueIOError___throw( ROGUE_ARG(((RogueClassIOError*)(((RogueException*)Rogue_call_ROGUEM4( 13, ROGUE_ARG(((RogueException*)ROGUE_CREATE_REF(RogueClassIOError*,ROGUE_CREATE_OBJECT(IOError)))), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[103] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(THIS->filepath) ))) )))), Rogue_literal_strings[169] )))) )))) ))))) )));
  }
  return (RogueClassFileWriter*)(THIS);
}

void RogueFileWriter__on_cleanup( RogueClassFileWriter* THIS )
{
  ROGUE_GC_CHECK;
  RogueFileWriter__close( ROGUE_ARG(THIS) );
}

RogueInt64 RogueFileWriter__fp( RogueClassFileWriter* THIS )
{
  return (RogueInt64)(((RogueInt64)(THIS->fp)));
}

RogueLogical RogueFileWriter__open__String_Logical( RogueClassFileWriter* THIS, RogueString* _auto_522_0, RogueLogical append_1 )
{
  ROGUE_GC_CHECK;
  THIS->filepath = _auto_522_0;
  RogueFileWriter__close( ROGUE_ARG(THIS) );
  THIS->error = false;
  THIS->filepath = (RogueFile__expand_path__String( ROGUE_ARG(THIS->filepath) ));
  THIS->filepath = (RogueFile__fix_slashes__String( ROGUE_ARG(THIS->filepath) ));
  if (ROGUE_COND(append_1))
  {
    THIS->fp = fopen( (char*)THIS->filepath->utf8, "ab" );

  }
  else
  {
    THIS->fp = fopen( (char*)THIS->filepath->utf8, "wb" );

  }
  THIS->error = !(THIS->fp);

  return (RogueLogical)(!(THIS->error));
}

void RogueFileWriter__write__String( RogueClassFileWriter* THIS, RogueString* data_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(!!(THIS->fp))))
  {
    return;
  }
  RogueFileWriter__flush( ROGUE_ARG(THIS) );
  THIS->position += ((RogueString__byte_count( data_0 )));
  fwrite( data_0->utf8, 1, data_0->byte_count, THIS->fp );

}

void RogueWriter_Byte___close( RogueObject* THIS )
{
  switch (THIS->type->index)
  {
    case 59:
      RogueFileWriter__close( (RogueClassFileWriter*)THIS );
    return;
    case 75:
      RogueWindowsProcessWriter__close( (RogueClassWindowsProcessWriter*)THIS );
    return;
    case 79:
      RogueFDWriter__close( (RogueClassFDWriter*)THIS );
    return;
  }
}

void RogueWriter_Byte___flush( RogueObject* THIS )
{
  switch (THIS->type->index)
  {
    case 59:
      RogueFileWriter__flush( (RogueClassFileWriter*)THIS );
    return;
    case 75:
      RogueWindowsProcessWriter__flush( (RogueClassWindowsProcessWriter*)THIS );
    return;
    case 79:
      RogueFDWriter__flush( (RogueClassFDWriter*)THIS );
    return;
  }
}

RogueClassConsole* RogueConsole__init_object( RogueClassConsole* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  THIS->output_buffer = ((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )));
  THIS->error = ((RogueClassConsoleErrorPrinter*)(((RogueObject*)Rogue_call_ROGUEM0( 1, ROGUE_ARG(((RogueObject*)ROGUE_CREATE_REF(RogueClassConsoleErrorPrinter*,ROGUE_CREATE_OBJECT(ConsoleErrorPrinter)))) ))));
  THIS->immediate_mode = false;
  THIS->events = ((RogueConsoleEvent_List*)(RogueConsoleEvent_List__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueConsoleEvent_List*,ROGUE_CREATE_OBJECT(ConsoleEvent_List))) )));
  THIS->decode_utf8 = true;
  THIS->input_buffer = ((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )));
  THIS->_input_bytes = ((RogueByte_List*)(RogueByte_List__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueByte_List*,ROGUE_CREATE_OBJECT(Byte_List))) )));
  return (RogueClassConsole*)(THIS);
}

RogueClassConsole* RogueConsole__init( RogueClassConsole* THIS )
{
  ROGUE_GC_CHECK;
#ifdef ROGUE_PLATFORM_WINDOWS
    // Enable ANSI colors and styles on Windows
    HANDLE h_stdout = GetStdHandle( STD_OUTPUT_HANDLE );
    DWORD mode;
    GetConsoleMode( h_stdout, &mode );
    SetConsoleMode( h_stdout, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING );
    SetConsoleOutputCP(65001);  // Extended characters

    HANDLE h_stdin = GetStdHandle( STD_INPUT_HANDLE );
    GetConsoleMode( h_stdin, &mode );

  THIS->windows_in_quick_edit_mode = ((RogueLogical)(!!(mode & ENABLE_QUICK_EDIT_MODE)));
#else
    tcgetattr( STDIN_FILENO, &THIS->original_terminal_settings );
    THIS->original_stdin_flags = fcntl( STDIN_FILENO, F_GETFL );

    if ( !THIS->original_terminal_settings.c_cc[VMIN] ) THIS->original_terminal_settings.c_cc[VMIN] = 1;
    THIS->original_terminal_settings.c_lflag |= (ECHO | ECHOE | ICANON);
    THIS->original_stdin_flags &= ~(O_NONBLOCK);
#endif

  RogueGlobal__on_exit___Function___( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), ROGUE_ARG(((RogueClass_Function___*)(((RogueClassFunction_2437*)(RogueFunction_2437__init__Console( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassFunction_2437*,ROGUE_CREATE_OBJECT(Function_2437))), ROGUE_ARG(THIS) )))))) );
  THIS->show_cursor = true;
  return (RogueClassConsole*)(THIS);
}

RogueString* RogueConsole__to_String( RogueClassConsole* THIS )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueCharacter_List*,buffer_0,(((RogueCharacter_List*)(RogueCharacter_List__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueCharacter_List*,ROGUE_CREATE_OBJECT(Character_List))) )))));
  while (ROGUE_COND(((RogueConsole__has_another( ROGUE_ARG(THIS) )))))
  {
    ROGUE_GC_CHECK;
    RogueCharacter_List__add__Character( buffer_0, ROGUE_ARG(((RogueConsole__read( ROGUE_ARG(THIS) )))) );
  }
  if (ROGUE_COND((false)))
  {
  }
  else
  {
    return (RogueString*)(((RogueString*)(RogueCharacter_List__to_String( buffer_0 ))));
  }
}

RogueString* RogueConsole__type_name( RogueClassConsole* THIS )
{
  return (RogueString*)(Rogue_literal_strings[348]);
}

RogueLogical RogueConsole__has_another( RogueClassConsole* THIS )
{
  ROGUE_GC_CHECK;
  {
    ROGUE_DEF_LOCAL_REF(RogueConsoleEvent_List*,_auto_2439_0,(THIS->events));
    RogueInt32 _auto_2440_0 = (0);
    RogueInt32 _auto_2441_0 = (((_auto_2439_0->count) - (1)));
    for (;ROGUE_COND(((_auto_2440_0) <= (_auto_2441_0)));++_auto_2440_0)
    {
      ROGUE_GC_CHECK;
      RogueClassConsoleEvent _auto_567_0 = (((RogueClassConsoleEvent*)(_auto_2439_0->data->as_bytes))[_auto_2440_0]);
      if (ROGUE_COND(((RogueConsoleEvent__is_character( _auto_567_0 )))))
      {
        return (RogueLogical)(true);
      }
    }
  }
  RogueConsole___fill_event_queue( ROGUE_ARG(THIS) );
  {
    ROGUE_DEF_LOCAL_REF(RogueConsoleEvent_List*,_auto_2444_0,(THIS->events));
    RogueInt32 _auto_2445_0 = (0);
    RogueInt32 _auto_2446_0 = (((_auto_2444_0->count) - (1)));
    for (;ROGUE_COND(((_auto_2445_0) <= (_auto_2446_0)));++_auto_2445_0)
    {
      ROGUE_GC_CHECK;
      RogueClassConsoleEvent _auto_568_0 = (((RogueClassConsoleEvent*)(_auto_2444_0->data->as_bytes))[_auto_2445_0]);
      if (ROGUE_COND(((RogueConsoleEvent__is_character( _auto_568_0 )))))
      {
        return (RogueLogical)(true);
      }
    }
  }
  return (RogueLogical)(false);
}

RogueCharacter RogueConsole__read( RogueClassConsole* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(((RogueConsole__has_another( ROGUE_ARG(THIS) ))))))
  {
    return (RogueCharacter)((RogueCharacter)0);
  }
  while (ROGUE_COND(!!(THIS->events->count)))
  {
    ROGUE_GC_CHECK;
    RogueClassConsoleEvent event_0 = (((RogueConsoleEvent_List__remove_first( ROGUE_ARG(THIS->events) ))));
    if (ROGUE_COND(((RogueConsoleEvent__is_character( event_0 )))))
    {
      return (RogueCharacter)(((RogueCharacter)(event_0.x)));
    }
  }
  return (RogueCharacter)((RogueCharacter)0);
}

void RogueConsole__flush( RogueClassConsole* THIS )
{
  ROGUE_GC_CHECK;
  RogueConsole__flush__StringBuilder( ROGUE_ARG(THIS), ROGUE_ARG(THIS->output_buffer) );
}

RogueClassConsole* RogueConsole__print__Character( RogueClassConsole* THIS, RogueCharacter value_0 )
{
  ROGUE_GC_CHECK;
  RogueStringBuilder__print__Character( ROGUE_ARG(THIS->output_buffer), value_0 );
  if (ROGUE_COND(((value_0) == ((RogueCharacter)10))))
  {
    RogueConsole__flush( ROGUE_ARG(THIS) );
  }
  return (RogueClassConsole*)(THIS);
}

RogueClassConsole* RogueConsole__print__String( RogueClassConsole* THIS, RogueString* value_0 )
{
  ROGUE_GC_CHECK;
  RogueStringBuilder__print__String( ROGUE_ARG(THIS->output_buffer), value_0 );
  if (ROGUE_COND(((THIS->output_buffer->count) > (1024))))
  {
    RogueConsole__flush( ROGUE_ARG(THIS) );
  }
  return (RogueClassConsole*)(THIS);
}

RogueClassConsole* RogueConsole__print__StringBuilder( RogueClassConsole* THIS, RogueStringBuilder* value_0 )
{
  ROGUE_GC_CHECK;
  RogueStringBuilder__print__StringBuilder( ROGUE_ARG(THIS->output_buffer), value_0 );
  return (RogueClassConsole*)(THIS);
}

void RogueConsole__flush__StringBuilder( RogueClassConsole* THIS, RogueStringBuilder* buffer_0 )
{
  ROGUE_GC_CHECK;
  RogueConsole__write__StringBuilder( ROGUE_ARG(THIS), buffer_0 );
  RogueStringBuilder__clear( buffer_0 );
}

RogueLogical RogueConsole___fill_input_buffer__Int32( RogueClassConsole* THIS, RogueInt32 minimum_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((THIS->_input_bytes->count) >= (minimum_0))))
  {
    return (RogueLogical)(true);
  }
  RogueInt32 n_1 = (1024);
  while (ROGUE_COND(((n_1) == (1024))))
  {
    ROGUE_GC_CHECK;
    char bytes[1024];
    n_1 = (RogueInt32) ROGUE_READ_CALL( STDIN_FILENO, &bytes, 1024 );

    if (ROGUE_COND(((((n_1) == (0))) && (!(THIS->immediate_mode)))))
    {
      throw ((RogueException*)(RogueIOError___throw( ROGUE_ARG(((RogueClassIOError*)(((RogueException*)Rogue_call_ROGUEM4( 13, ROGUE_ARG(((RogueException*)ROGUE_CREATE_REF(RogueClassIOError*,ROGUE_CREATE_OBJECT(IOError)))), Rogue_literal_strings[300] ))))) )));
    }
    if (ROGUE_COND(((n_1) > (0))))
    {
      {
        RogueInt32 i_2 = (0);
        RogueInt32 _auto_566_3 = (n_1);
        for (;ROGUE_COND(((i_2) < (_auto_566_3)));++i_2)
        {
          ROGUE_GC_CHECK;
          RogueByte_List__add__Byte( ROGUE_ARG(THIS->_input_bytes), ROGUE_ARG(((RogueByte)(((RogueByte)bytes[i_2])))) );
        }
      }
    }
  }
  return (RogueLogical)(((THIS->_input_bytes->count) >= (minimum_0)));
}

void RogueConsole___fill_event_queue( RogueClassConsole* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND((((RogueSystem__is_windows())) && (THIS->immediate_mode))))
  {
    RogueConsole___fill_event_queue_windows( ROGUE_ARG(THIS) );
  }
  else
  {
    RogueConsole___fill_event_queue_unix( ROGUE_ARG(THIS) );
  }
}

void RogueConsole___fill_event_queue_windows( RogueClassConsole* THIS )
{
  ROGUE_GC_CHECK;
#if defined(ROGUE_PLATFORM_WINDOWS)
  HANDLE       h_stdin = GetStdHandle( STD_INPUT_HANDLE );
  DWORD        event_count = 0;

  GetNumberOfConsoleInputEvents( h_stdin, &event_count );

  while (ROGUE_COND(!!(((RogueInt32)((RogueInt32)event_count)))))
  {
    ROGUE_GC_CHECK;
    RogueConsole___fill_event_queue_windows_process_next( ROGUE_ARG(THIS) );
    GetNumberOfConsoleInputEvents( h_stdin, &event_count );

  }
#endif // ROGUE_PLATFORM_WINDOWS

}

void RogueConsole___fill_event_queue_windows_process_next( RogueClassConsole* THIS )
{
  ROGUE_GC_CHECK;
#if defined(ROGUE_PLATFORM_WINDOWS)

  RogueInt32 unicode_0 = 0;
  RogueInt32 event_flags_1 = 0;
  RogueInt32 new_button_state_2 = 0;
  RogueInt32 x_3 = 0;
  RogueInt32 y_4 = 0;
  HANDLE       h_stdin = GetStdHandle( STD_INPUT_HANDLE );
  DWORD        event_count = 0;
  INPUT_RECORD record;
  ReadConsoleInput( h_stdin, &record, 1, &event_count );
  if (record.EventType == MOUSE_EVENT)
  {
    event_flags_1 = (RogueInt32) record.Event.MouseEvent.dwEventFlags;
    new_button_state_2 = (RogueInt32) record.Event.MouseEvent.dwButtonState;
    x_3 = (RogueInt32) record.Event.MouseEvent.dwMousePosition.X;
    y_4 = (RogueInt32) record.Event.MouseEvent.dwMousePosition.Y;

    // Adjust Y coordinate to be relative to visible top-left corner of console
    CONSOLE_SCREEN_BUFFER_INFO info;
    HANDLE h_stdout = GetStdHandle( STD_OUTPUT_HANDLE );
    if (GetConsoleScreenBufferInfo(h_stdout,&info))
    {
      y_4 -= info.srWindow.Top;
    }

  if (ROGUE_COND(!!(event_flags_1)))
  {
    if (ROGUE_COND(!!(((event_flags_1) & (((RogueInt32)(DOUBLE_CLICK)))))))
    {
      RogueConsoleEvent_List__add__ConsoleEvent( ROGUE_ARG(THIS->events), RogueClassConsoleEvent( RogueClassConsoleEventType( THIS->_windows_last_press_type ), x_3, y_4 ) );
      RogueConsoleEvent_List__add__ConsoleEvent( ROGUE_ARG(THIS->events), RogueClassConsoleEvent( RogueClassConsoleEventType( 3 ), x_3, y_4 ) );
    }
    else if (ROGUE_COND(!!(((event_flags_1) & (((RogueInt32)(MOUSE_MOVED)))))))
    {
      RogueConsoleEvent_List__add__ConsoleEvent( ROGUE_ARG(THIS->events), RogueClassConsoleEvent( RogueClassConsoleEventType( 4 ), x_3, y_4 ) );
    }
    else if (ROGUE_COND(!!(((event_flags_1) & (((RogueInt32)(MOUSE_WHEELED)))))))
    {
      if (ROGUE_COND(((((new_button_state_2) & (-65536))) > (0))))
      {
        RogueConsoleEvent_List__add__ConsoleEvent( ROGUE_ARG(THIS->events), RogueClassConsoleEvent( RogueClassConsoleEventType( 5 ), x_3, y_4 ) );
      }
      else
      {
        RogueConsoleEvent_List__add__ConsoleEvent( ROGUE_ARG(THIS->events), RogueClassConsoleEvent( RogueClassConsoleEventType( 6 ), x_3, y_4 ) );
      }
    }
  }
  else
  {
    RogueInt32 toggled_5 = (((THIS->windows_button_state) ^ (new_button_state_2)));
    if (ROGUE_COND(!!(toggled_5)))
    {
      if (ROGUE_COND(!!(((toggled_5) & (((RogueInt32)(FROM_LEFT_1ST_BUTTON_PRESSED)))))))
      {
        if (ROGUE_COND(!!(((new_button_state_2) & (((RogueInt32)(FROM_LEFT_1ST_BUTTON_PRESSED)))))))
        {
          THIS->_windows_last_press_type = ((RogueConsoleEventType__to_Int32( RogueClassConsoleEventType( 1 ) )));
          RogueConsoleEvent_List__add__ConsoleEvent( ROGUE_ARG(THIS->events), RogueClassConsoleEvent( RogueClassConsoleEventType( 1 ), x_3, y_4 ) );
        }
        else
        {
          RogueConsoleEvent_List__add__ConsoleEvent( ROGUE_ARG(THIS->events), RogueClassConsoleEvent( RogueClassConsoleEventType( 3 ), x_3, y_4 ) );
        }
      }
      else if (ROGUE_COND(!!(((toggled_5) & (((RogueInt32)(RIGHTMOST_BUTTON_PRESSED)))))))
      {
        if (ROGUE_COND(!!(((new_button_state_2) & (((RogueInt32)(RIGHTMOST_BUTTON_PRESSED)))))))
        {
          THIS->_windows_last_press_type = ((RogueConsoleEventType__to_Int32( RogueClassConsoleEventType( 2 ) )));
          RogueConsoleEvent_List__add__ConsoleEvent( ROGUE_ARG(THIS->events), RogueClassConsoleEvent( RogueClassConsoleEventType( 2 ), x_3, y_4 ) );
        }
        else
        {
          RogueConsoleEvent_List__add__ConsoleEvent( ROGUE_ARG(THIS->events), RogueClassConsoleEvent( RogueClassConsoleEventType( 3 ), x_3, y_4 ) );
        }
      }
    }
    THIS->windows_button_state = new_button_state_2;
  }
  return;
  } // if MOUSE_EVENT

  if (record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown &&
      record.Event.KeyEvent.uChar.UnicodeChar)
  {
    unicode_0 = (RogueInt32) record.Event.KeyEvent.uChar.UnicodeChar;

  if (ROGUE_COND(((unicode_0) == (13))))
  {
    unicode_0 = ((RogueInt32)(10));
  }
  RogueConsoleEvent_List__add__ConsoleEvent( ROGUE_ARG(THIS->events), RogueClassConsoleEvent( RogueClassConsoleEventType( 0 ), unicode_0, 0 ) );
  return;
  }

#endif // ROGUE_PLATFORM_WINDOWS

}

void RogueConsole___fill_event_queue_unix( RogueClassConsole* THIS )
{
  ROGUE_GC_CHECK;
  RogueConsole___fill_event_queue_unix_process_next( ROGUE_ARG(THIS) );
  while (ROGUE_COND(!!(THIS->_input_bytes->count)))
  {
    ROGUE_GC_CHECK;
    RogueConsole___fill_event_queue_unix_process_next( ROGUE_ARG(THIS) );
  }
}

void RogueConsole___fill_event_queue_unix_process_next( RogueClassConsole* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(((RogueConsole___fill_input_buffer__Int32( ROGUE_ARG(THIS), 1 ))))))
  {
    return;
  }
  RogueByte b1_0 = (((RogueByte_List__remove_first( ROGUE_ARG(THIS->_input_bytes) ))));
  if (ROGUE_COND(((((RogueInt32)(b1_0))) == (27))))
  {
    if (ROGUE_COND(((((THIS->_input_bytes->count) >= (2))) && (((((RogueInt32)(((RogueByte_List__first( ROGUE_ARG(THIS->_input_bytes) )))))) == (91))))))
    {
      RogueByte_List__remove_first( ROGUE_ARG(THIS->_input_bytes) );
      if (ROGUE_COND(((((RogueInt32)(((RogueByte_List__first( ROGUE_ARG(THIS->_input_bytes) )))))) == (77))))
      {
        if (ROGUE_COND(((RogueConsole___fill_input_buffer__Int32( ROGUE_ARG(THIS), 4 )))))
        {
          RogueByte_List__remove_first( ROGUE_ARG(THIS->_input_bytes) );
          RogueClassConsoleEventType event_type_1 = RogueClassConsoleEventType();
          RogueClassUnixConsoleMouseEventType type_2 = (RogueClassUnixConsoleMouseEventType( ((RogueInt32)(((RogueByte_List__remove_first( ROGUE_ARG(THIS->_input_bytes) ))))) ));
          switch (type_2.value)
          {
            case 32:
            {
              event_type_1 = ((RogueClassConsoleEventType)(RogueClassConsoleEventType( 1 )));
              break;
            }
            case 34:
            {
              event_type_1 = ((RogueClassConsoleEventType)(RogueClassConsoleEventType( 2 )));
              break;
            }
            case 35:
            {
              event_type_1 = ((RogueClassConsoleEventType)(RogueClassConsoleEventType( 3 )));
              break;
            }
            case 64:
            {
              event_type_1 = ((RogueClassConsoleEventType)(RogueClassConsoleEventType( 4 )));
              break;
            }
            case 66:
            {
              event_type_1 = ((RogueClassConsoleEventType)(RogueClassConsoleEventType( 4 )));
              break;
            }
            case 67:
            {
              event_type_1 = ((RogueClassConsoleEventType)(RogueClassConsoleEventType( 4 )));
              break;
            }
            case 96:
            {
              event_type_1 = ((RogueClassConsoleEventType)(RogueClassConsoleEventType( 5 )));
              break;
            }
            case 97:
            {
              event_type_1 = ((RogueClassConsoleEventType)(RogueClassConsoleEventType( 6 )));
              break;
            }
            default:
            {
              return;
            }
          }
          RogueInt32 x_3 = (((((RogueInt32)(((RogueByte_List__remove_first( ROGUE_ARG(THIS->_input_bytes) )))))) - (33)));
          RogueInt32 y_4 = (((((RogueInt32)(((RogueByte_List__remove_first( ROGUE_ARG(THIS->_input_bytes) )))))) - (33)));
          RogueConsoleEvent_List__add__ConsoleEvent( ROGUE_ARG(THIS->events), RogueClassConsoleEvent( event_type_1, x_3, y_4 ) );
          return;
        }
      }
      else if (ROGUE_COND(((((RogueInt32)(((RogueByte_List__first( ROGUE_ARG(THIS->_input_bytes) )))))) == (51))))
      {
        RogueByte_List__remove_first( ROGUE_ARG(THIS->_input_bytes) );
        if (ROGUE_COND(((RogueConsole___fill_input_buffer__Int32( ROGUE_ARG(THIS), 1 )))))
        {
          if (ROGUE_COND(((((RogueInt32)(((RogueByte_List__first( ROGUE_ARG(THIS->_input_bytes) )))))) == (126))))
          {
            RogueByte_List__remove_first( ROGUE_ARG(THIS->_input_bytes) );
            RogueConsoleEvent_List__add__ConsoleEvent( ROGUE_ARG(THIS->events), RogueClassConsoleEvent( RogueClassConsoleEventType( 0 ), 8, 0 ) );
            return;
          }
        }
      }
      else
      {
        RogueInt32 ch_5 = (((((((RogueInt32)(((RogueByte_List__remove_first( ROGUE_ARG(THIS->_input_bytes) )))))) - (65))) + (17)));
        RogueConsoleEvent_List__add__ConsoleEvent( ROGUE_ARG(THIS->events), RogueClassConsoleEvent( RogueClassConsoleEventType( 0 ), ch_5, 0 ) );
        return;
      }
    }
  }
  if (ROGUE_COND(((((((RogueInt32)(b1_0))) < (192))) || (!(THIS->decode_utf8)))))
  {
    RogueConsoleEvent_List__add__ConsoleEvent( ROGUE_ARG(THIS->events), RogueClassConsoleEvent( RogueClassConsoleEventType( 0 ), ((RogueInt32)(b1_0)), 0 ) );
    return;
  }
  RogueInt32 ch_6 = (((RogueInt32)(b1_0)));
  RogueGlobal__println__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[301] )))), ROGUE_ARG(((RogueString*)(RogueString__operatorPLUS__Object( ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], Rogue_literal_strings[0] ))), ROGUE_ARG(((RogueObject*)(THIS->_input_bytes))) )))) )))) )))) );
  {
    {
      {
        if (ROGUE_COND(((((((RogueInt32)(b1_0))) & (224))) == (192))))
        {
          RogueGlobal__println__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[302] )))) )))) );
          if ( !(((RogueConsole___fill_input_buffer__Int32( ROGUE_ARG(THIS), 1 )))) ) goto _auto_2443;
          RogueGlobal__println__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[303] )))) )))) );
          ch_6 = ((RogueInt32)(((((RogueInt32)(b1_0))) & (31))));
          ch_6 = ((RogueInt32)(((((ch_6) << (6))) | (((((RogueInt32)(((RogueByte_List__remove_first( ROGUE_ARG(THIS->_input_bytes) )))))) & (63))))));
        }
        else if (ROGUE_COND(((((((RogueInt32)(b1_0))) & (240))) == (224))))
        {
          RogueGlobal__println__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[304] )))) )))) );
          if ( !(((RogueConsole___fill_input_buffer__Int32( ROGUE_ARG(THIS), 2 )))) ) goto _auto_2443;
          RogueGlobal__println__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[305] )))) )))) );
          ch_6 = ((RogueInt32)(((((RogueInt32)(b1_0))) & (15))));
          ch_6 = ((RogueInt32)(((((ch_6) << (6))) | (((((RogueInt32)(((RogueByte_List__remove_first( ROGUE_ARG(THIS->_input_bytes) )))))) & (63))))));
          ch_6 = ((RogueInt32)(((((ch_6) << (6))) | (((((RogueInt32)(((RogueByte_List__remove_first( ROGUE_ARG(THIS->_input_bytes) )))))) & (63))))));
        }
        else if (ROGUE_COND(((((((RogueInt32)(b1_0))) & (248))) == (240))))
        {
          RogueGlobal__println__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[306] )))) )))) );
          if ( !(((RogueConsole___fill_input_buffer__Int32( ROGUE_ARG(THIS), 3 )))) ) goto _auto_2443;
          RogueGlobal__println__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[307] )))) )))) );
          ch_6 = ((RogueInt32)(((((RogueInt32)(b1_0))) & (7))));
          ch_6 = ((RogueInt32)(((((ch_6) << (6))) | (((((RogueInt32)(((RogueByte_List__remove_first( ROGUE_ARG(THIS->_input_bytes) )))))) & (63))))));
          ch_6 = ((RogueInt32)(((((ch_6) << (6))) | (((((RogueInt32)(((RogueByte_List__remove_first( ROGUE_ARG(THIS->_input_bytes) )))))) & (63))))));
          ch_6 = ((RogueInt32)(((((ch_6) << (6))) | (((((RogueInt32)(((RogueByte_List__remove_first( ROGUE_ARG(THIS->_input_bytes) )))))) & (63))))));
        }
        else if (ROGUE_COND(((((((RogueInt32)(b1_0))) & (252))) == (248))))
        {
          if ( !(((RogueConsole___fill_input_buffer__Int32( ROGUE_ARG(THIS), 4 )))) ) goto _auto_2443;
          ch_6 = ((RogueInt32)(((((RogueInt32)(b1_0))) & (3))));
          ch_6 = ((RogueInt32)(((((ch_6) << (6))) | (((((RogueInt32)(((RogueByte_List__remove_first( ROGUE_ARG(THIS->_input_bytes) )))))) & (63))))));
          ch_6 = ((RogueInt32)(((((ch_6) << (6))) | (((((RogueInt32)(((RogueByte_List__remove_first( ROGUE_ARG(THIS->_input_bytes) )))))) & (63))))));
          ch_6 = ((RogueInt32)(((((ch_6) << (6))) | (((((RogueInt32)(((RogueByte_List__remove_first( ROGUE_ARG(THIS->_input_bytes) )))))) & (63))))));
          ch_6 = ((RogueInt32)(((((ch_6) << (6))) | (((((RogueInt32)(((RogueByte_List__remove_first( ROGUE_ARG(THIS->_input_bytes) )))))) & (63))))));
        }
        else
        {
          if ( !(((RogueConsole___fill_input_buffer__Int32( ROGUE_ARG(THIS), 5 )))) ) goto _auto_2443;
          ch_6 = ((RogueInt32)(((((RogueInt32)(b1_0))) & (1))));
          ch_6 = ((RogueInt32)(((((ch_6) << (6))) | (((((RogueInt32)(((RogueByte_List__remove_first( ROGUE_ARG(THIS->_input_bytes) )))))) & (63))))));
          ch_6 = ((RogueInt32)(((((ch_6) << (6))) | (((((RogueInt32)(((RogueByte_List__remove_first( ROGUE_ARG(THIS->_input_bytes) )))))) & (63))))));
          ch_6 = ((RogueInt32)(((((ch_6) << (6))) | (((((RogueInt32)(((RogueByte_List__remove_first( ROGUE_ARG(THIS->_input_bytes) )))))) & (63))))));
          ch_6 = ((RogueInt32)(((((ch_6) << (6))) | (((((RogueInt32)(((RogueByte_List__remove_first( ROGUE_ARG(THIS->_input_bytes) )))))) & (63))))));
          ch_6 = ((RogueInt32)(((((ch_6) << (6))) | (((((RogueInt32)(((RogueByte_List__remove_first( ROGUE_ARG(THIS->_input_bytes) )))))) & (63))))));
        }
        }
      goto _auto_2442;
    }
    _auto_2443:;
  }
  _auto_2442:;
  RogueConsoleEvent_List__add__ConsoleEvent( ROGUE_ARG(THIS->events), RogueClassConsoleEvent( RogueClassConsoleEventType( 0 ), ch_6, 0 ) );
}

void RogueConsole__reset_io_mode( RogueClassConsole* THIS )
{
  ROGUE_GC_CHECK;
  RogueConsole__set_immediate_mode__Logical( ROGUE_ARG(THIS), false );
  RogueConsole__set_show_cursor__Logical_PrintWriter( ROGUE_ARG(THIS), true, ROGUE_ARG(((((RogueClassPrintWriter*)(((RogueClassGlobal*)ROGUE_SINGLETON(Global))))))) );
}

void RogueConsole__set_immediate_mode__Logical( RogueClassConsole* THIS, RogueLogical setting_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((THIS->immediate_mode) != (setting_0))))
  {
    THIS->immediate_mode = setting_0;
    if (ROGUE_COND(THIS->immediate_mode))
    {
      THIS->windows_button_state = 0;
#if defined(ROGUE_PLATFORM_WINDOWS)
        HANDLE h_stdin = GetStdHandle( STD_INPUT_HANDLE );
        DWORD mode;
        GetConsoleMode( h_stdin, &mode );
        SetConsoleMode( h_stdin, (mode & ~(ENABLE_QUICK_EDIT_MODE)) | ENABLE_EXTENDED_FLAGS | ENABLE_MOUSE_INPUT );
#else
        termios new_settings;
        tcgetattr( STDIN_FILENO, &new_settings );
        new_settings.c_lflag &= ~(ECHO | ECHOE | ICANON);
        new_settings.c_cc[VMIN] = 0;
        new_settings.c_cc[VTIME] = 0;
        tcsetattr( STDIN_FILENO, TCSANOW, &new_settings );
        fcntl( STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO,F_GETFL)|O_NONBLOCK );

      RogueGlobal__flush( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ROGUE_ARG(([&]()->RogueClassGlobal*{
        ROGUE_DEF_LOCAL_REF(RogueClassGlobal*,_auto_569_0,(((RogueClassGlobal*)ROGUE_SINGLETON(Global))));
        RogueGlobal__flush( _auto_569_0 );
         return _auto_569_0;
      }())
      ), Rogue_literal_strings[397] )))) );
#endif

    }
    else
    {
#if defined(ROGUE_PLATFORM_WINDOWS)
        HANDLE h_stdin = GetStdHandle( STD_INPUT_HANDLE );
        DWORD mode;
        GetConsoleMode( h_stdin, &mode );
        if (THIS->windows_in_quick_edit_mode) mode |= ENABLE_QUICK_EDIT_MODE;
        mode |= (ENABLE_EXTENDED_FLAGS | ENABLE_MOUSE_INPUT);
        SetConsoleMode( h_stdin, mode );
#else
        tcsetattr( STDIN_FILENO, TCSANOW, &THIS->original_terminal_settings );
        fcntl( STDIN_FILENO, F_SETFL, THIS->original_stdin_flags );

      RogueGlobal__flush( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ROGUE_ARG(([&]()->RogueClassGlobal*{
        ROGUE_DEF_LOCAL_REF(RogueClassGlobal*,_auto_570_0,(((RogueClassGlobal*)ROGUE_SINGLETON(Global))));
        RogueGlobal__flush( _auto_570_0 );
         return _auto_570_0;
      }())
      ), Rogue_literal_strings[398] )))) );
#endif

    }
  }
}

void RogueConsole__set_show_cursor__Logical_PrintWriter( RogueClassConsole* THIS, RogueLogical setting_0, RogueClassPrintWriter* output_1 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((THIS->show_cursor) == (setting_0))))
  {
    return;
  }
  THIS->show_cursor = setting_0;
  RoguePrintWriter__flush( ((((RogueObject*)output_1))) );
  RoguePrintWriter__flush( ROGUE_ARG(((((RogueObject*)(RoguePrintWriter__print__Character( ROGUE_ARG(((((RogueObject*)(RoguePrintWriter__print__String( ((((RogueObject*)output_1))), Rogue_literal_strings[399] )))))), ROGUE_ARG(((((setting_0))) ? ((RogueCharacter)'h') : (RogueCharacter)'l')) )))))) );
}

RogueInt32 RogueConsole__width( RogueClassConsole* THIS )
{
  ROGUE_GC_CHECK;
#ifdef ROGUE_PLATFORM_WINDOWS
    HANDLE h_stdout = GetStdHandle( STD_OUTPUT_HANDLE );
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(h_stdout,&info)) return info.dwSize.X;
    else return 80;
#else
    struct winsize sz;
    ioctl( STDOUT_FILENO, TIOCGWINSZ, &sz );

    return sz.ws_col;
#endif

}

void RogueConsole__write__StringBuilder( RogueClassConsole* THIS, RogueStringBuilder* buffer_0 )
{
  ROGUE_GC_CHECK;
  Rogue_fwrite( (char*)buffer_0->utf8->data->as_bytes, buffer_0->utf8->count, STDOUT_FILENO );

}

void RogueBufferedPrintWriter_output_buffer___flush( RogueObject* THIS )
{
  switch (THIS->type->index)
  {
    case 61:
      RogueConsole__flush( (RogueClassConsole*)THIS );
    return;
    case 63:
      RogueConsoleErrorPrinter__flush( (RogueClassConsoleErrorPrinter*)THIS );
    return;
  }
}

RogueClassBufferedPrintWriter_output_buffer_* RogueBufferedPrintWriter_output_buffer___print__Character( RogueObject* THIS, RogueCharacter value_0 )
{
  switch (THIS->type->index)
  {
    case 61:
      return (RogueClassBufferedPrintWriter_output_buffer_*)RogueConsole__print__Character( (RogueClassConsole*)THIS, value_0 );
    case 63:
      return (RogueClassBufferedPrintWriter_output_buffer_*)RogueConsoleErrorPrinter__print__Character( (RogueClassConsoleErrorPrinter*)THIS, value_0 );
    default:
      return 0;
  }
}

RogueClassBufferedPrintWriter_output_buffer_* RogueBufferedPrintWriter_output_buffer___print__String( RogueObject* THIS, RogueString* value_0 )
{
  switch (THIS->type->index)
  {
    case 61:
      return (RogueClassBufferedPrintWriter_output_buffer_*)RogueConsole__print__String( (RogueClassConsole*)THIS, value_0 );
    case 63:
      return (RogueClassBufferedPrintWriter_output_buffer_*)RogueConsoleErrorPrinter__print__String( (RogueClassConsoleErrorPrinter*)THIS, value_0 );
    default:
      return 0;
  }
}

RogueClassBufferedPrintWriter_output_buffer_* RogueBufferedPrintWriter_output_buffer___print__StringBuilder( RogueObject* THIS, RogueStringBuilder* value_0 )
{
  switch (THIS->type->index)
  {
    case 61:
      return (RogueClassBufferedPrintWriter_output_buffer_*)RogueConsole__print__StringBuilder( (RogueClassConsole*)THIS, value_0 );
    case 63:
      return (RogueClassBufferedPrintWriter_output_buffer_*)RogueConsoleErrorPrinter__print__StringBuilder( (RogueClassConsoleErrorPrinter*)THIS, value_0 );
    default:
      return 0;
  }
}

RogueClassConsoleErrorPrinter* RogueConsoleErrorPrinter__init_object( RogueClassConsoleErrorPrinter* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  THIS->output_buffer = ((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )));
  return (RogueClassConsoleErrorPrinter*)(THIS);
}

RogueString* RogueConsoleErrorPrinter__type_name( RogueClassConsoleErrorPrinter* THIS )
{
  return (RogueString*)(Rogue_literal_strings[373]);
}

void RogueConsoleErrorPrinter__flush( RogueClassConsoleErrorPrinter* THIS )
{
  ROGUE_GC_CHECK;
  RogueConsoleErrorPrinter__flush__StringBuilder( ROGUE_ARG(THIS), ROGUE_ARG(THIS->output_buffer) );
}

RogueClassConsoleErrorPrinter* RogueConsoleErrorPrinter__print__Character( RogueClassConsoleErrorPrinter* THIS, RogueCharacter value_0 )
{
  ROGUE_GC_CHECK;
  RogueStringBuilder__print__Character( ROGUE_ARG(THIS->output_buffer), value_0 );
  if (ROGUE_COND(((value_0) == ((RogueCharacter)10))))
  {
    RogueConsoleErrorPrinter__flush( ROGUE_ARG(THIS) );
  }
  return (RogueClassConsoleErrorPrinter*)(THIS);
}

RogueClassConsoleErrorPrinter* RogueConsoleErrorPrinter__print__String( RogueClassConsoleErrorPrinter* THIS, RogueString* value_0 )
{
  ROGUE_GC_CHECK;
  RogueStringBuilder__print__String( ROGUE_ARG(THIS->output_buffer), value_0 );
  if (ROGUE_COND(((THIS->output_buffer->count) > (1024))))
  {
    RogueConsoleErrorPrinter__flush( ROGUE_ARG(THIS) );
  }
  return (RogueClassConsoleErrorPrinter*)(THIS);
}

RogueClassConsoleErrorPrinter* RogueConsoleErrorPrinter__print__StringBuilder( RogueClassConsoleErrorPrinter* THIS, RogueStringBuilder* value_0 )
{
  ROGUE_GC_CHECK;
  RogueStringBuilder__print__StringBuilder( ROGUE_ARG(THIS->output_buffer), value_0 );
  return (RogueClassConsoleErrorPrinter*)(THIS);
}

RogueClassConsoleErrorPrinter* RogueConsoleErrorPrinter__println( RogueClassConsoleErrorPrinter* THIS )
{
  ROGUE_GC_CHECK;
  RogueStringBuilder__print__Character( ROGUE_ARG(THIS->output_buffer), (RogueCharacter)10 );
  RogueConsoleErrorPrinter__flush( ROGUE_ARG(THIS) );
  return (RogueClassConsoleErrorPrinter*)(THIS);
}

RogueClassConsoleErrorPrinter* RogueConsoleErrorPrinter__println__String( RogueClassConsoleErrorPrinter* THIS, RogueString* value_0 )
{
  ROGUE_GC_CHECK;
  return (RogueClassConsoleErrorPrinter*)(((RogueClassConsoleErrorPrinter*)(RogueConsoleErrorPrinter__println( ROGUE_ARG(((RogueClassConsoleErrorPrinter*)(RogueConsoleErrorPrinter__print__String( ROGUE_ARG(THIS), value_0 )))) ))));
}

void RogueConsoleErrorPrinter__flush__StringBuilder( RogueClassConsoleErrorPrinter* THIS, RogueStringBuilder* buffer_0 )
{
  ROGUE_GC_CHECK;
  RogueConsoleErrorPrinter__write__StringBuilder( ROGUE_ARG(THIS), buffer_0 );
  RogueStringBuilder__clear( buffer_0 );
}

void RogueConsoleErrorPrinter__write__StringBuilder( RogueClassConsoleErrorPrinter* THIS, RogueStringBuilder* buffer_0 )
{
  ROGUE_GC_CHECK;
  Rogue_fwrite( (char*)buffer_0->utf8->data->as_bytes, buffer_0->utf8->count, STDERR_FILENO );

}

RogueConsoleEvent_List* RogueConsoleEvent_List__init_object( RogueConsoleEvent_List* THIS )
{
  ROGUE_GC_CHECK;
  RogueGenericList__init_object( ROGUE_ARG(((RogueClassGenericList*)THIS)) );
  return (RogueConsoleEvent_List*)(THIS);
}

RogueConsoleEvent_List* RogueConsoleEvent_List__init( RogueConsoleEvent_List* THIS )
{
  ROGUE_GC_CHECK;
  RogueConsoleEvent_List__init__Int32( ROGUE_ARG(THIS), 0 );
  return (RogueConsoleEvent_List*)(THIS);
}

RogueString* RogueConsoleEvent_List__to_String( RogueConsoleEvent_List* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[293] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(((RogueString*)(RogueConsoleEvent_List__join__String( ROGUE_ARG(THIS), Rogue_literal_strings[15] )))) ))) )))), Rogue_literal_strings[294] )))) ))));
}

RogueString* RogueConsoleEvent_List__type_name( RogueConsoleEvent_List* THIS )
{
  return (RogueString*)(Rogue_literal_strings[383]);
}

RogueConsoleEvent_List* RogueConsoleEvent_List__init__Int32( RogueConsoleEvent_List* THIS, RogueInt32 initial_capacity_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((initial_capacity_0) != (((RogueConsoleEvent_List__capacity( ROGUE_ARG(THIS) )))))))
  {
    THIS->data = RogueType_create_array( initial_capacity_0, sizeof(RogueClassConsoleEvent), false, 127 );
  }
  RogueConsoleEvent_List__clear( ROGUE_ARG(THIS) );
  return (RogueConsoleEvent_List*)(THIS);
}

void RogueConsoleEvent_List__add__ConsoleEvent( RogueConsoleEvent_List* THIS, RogueClassConsoleEvent value_0 )
{
  ROGUE_GC_CHECK;
  RogueConsoleEvent_List__set__Int32_ConsoleEvent( ROGUE_ARG(((RogueConsoleEvent_List*)(RogueConsoleEvent_List__reserve__Int32( ROGUE_ARG(THIS), 1 )))), ROGUE_ARG(THIS->count), value_0 );
  THIS->count = ((THIS->count) + (1));
}

RogueInt32 RogueConsoleEvent_List__capacity( RogueConsoleEvent_List* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(!!(THIS->data))))
  {
    return (RogueInt32)(0);
  }
  return (RogueInt32)(THIS->data->count);
}

void RogueConsoleEvent_List__clear( RogueConsoleEvent_List* THIS )
{
  ROGUE_GC_CHECK;
  RogueConsoleEvent_List__discard_from__Int32( ROGUE_ARG(THIS), 0 );
}

void RogueConsoleEvent_List__discard_from__Int32( RogueConsoleEvent_List* THIS, RogueInt32 index_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!!(THIS->data)))
  {
    RogueInt32 n_1 = (((THIS->count) - (index_0)));
    if (ROGUE_COND(((n_1) > (0))))
    {
      RogueArray__zero__Int32_Int32( ROGUE_ARG(((RogueArray*)THIS->data)), index_0, n_1 );
      THIS->count = index_0;
    }
  }
}

RogueClassConsoleEvent RogueConsoleEvent_List__get__Int32( RogueConsoleEvent_List* THIS, RogueInt32 index_0 )
{
  return (RogueClassConsoleEvent)(((RogueClassConsoleEvent*)(THIS->data->as_bytes))[index_0]);
}

RogueString* RogueConsoleEvent_List__join__String( RogueConsoleEvent_List* THIS, RogueString* separator_0 )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueStringBuilder*,builder_1,(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))));
  RogueLogical first_2 = (true);
  {
    ROGUE_DEF_LOCAL_REF(RogueConsoleEvent_List*,_auto_2457_0,(THIS));
    RogueInt32 _auto_2458_0 = (0);
    RogueInt32 _auto_2459_0 = (((_auto_2457_0->count) - (1)));
    for (;ROGUE_COND(((_auto_2458_0) <= (_auto_2459_0)));++_auto_2458_0)
    {
      ROGUE_GC_CHECK;
      RogueClassConsoleEvent item_0 = (((RogueClassConsoleEvent*)(_auto_2457_0->data->as_bytes))[_auto_2458_0]);
      if (ROGUE_COND(first_2))
      {
        first_2 = ((RogueLogical)(false));
      }
      else
      {
        RogueStringBuilder__print__String( builder_1, separator_0 );
      }
      RogueStringBuilder__print__String( builder_1, ROGUE_ARG(((RogueString*)(RogueConsoleEvent__to_String( item_0 )))) );
    }
  }
  return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( builder_1 ))));
}

RogueConsoleEvent_List* RogueConsoleEvent_List__reserve__Int32( RogueConsoleEvent_List* THIS, RogueInt32 additional_elements_0 )
{
  ROGUE_GC_CHECK;
  RogueInt32 required_capacity_1 = (((THIS->count) + (additional_elements_0)));
  if (ROGUE_COND(((required_capacity_1) == (0))))
  {
    return (RogueConsoleEvent_List*)(THIS);
  }
  if (ROGUE_COND(!(!!(THIS->data))))
  {
    if (ROGUE_COND(((required_capacity_1) == (1))))
    {
      required_capacity_1 = ((RogueInt32)(10));
    }
    THIS->data = RogueType_create_array( required_capacity_1, sizeof(RogueClassConsoleEvent), false, 127 );
  }
  else if (ROGUE_COND(((required_capacity_1) > (THIS->data->count))))
  {
    RogueInt32 cap_2 = (((RogueConsoleEvent_List__capacity( ROGUE_ARG(THIS) ))));
    if (ROGUE_COND(((required_capacity_1) < (((cap_2) + (cap_2))))))
    {
      required_capacity_1 = ((RogueInt32)(((cap_2) + (cap_2))));
    }
    ROGUE_DEF_LOCAL_REF(RogueArray*,new_data_3,(RogueType_create_array( required_capacity_1, sizeof(RogueClassConsoleEvent), false, 127 )));
    RogueArray_set(new_data_3,0,((RogueArray*)(THIS->data)),0,-1);
    THIS->data = new_data_3;
  }
  return (RogueConsoleEvent_List*)(THIS);
}

RogueClassConsoleEvent RogueConsoleEvent_List__remove_at__Int32( RogueConsoleEvent_List* THIS, RogueInt32 index_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((RogueLogical)((((unsigned int)index_0) >= (unsigned int)THIS->count)))))
  {
    throw ((RogueException*)(RogueOutOfBoundsError___throw( ROGUE_ARG(((RogueClassOutOfBoundsError*)(RogueOutOfBoundsError__init__Int32_Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassOutOfBoundsError*,ROGUE_CREATE_OBJECT(OutOfBoundsError))), index_0, ROGUE_ARG(THIS->count) )))) )));
  }
  RogueClassConsoleEvent result_1 = (((RogueClassConsoleEvent*)(THIS->data->as_bytes))[index_0]);
  RogueArray_set(THIS->data,index_0,((RogueArray*)(THIS->data)),((index_0) + (1)),-1);
  RogueClassConsoleEvent zero_value_2 = RogueClassConsoleEvent();
  THIS->count = ((THIS->count) + (-1));
  ((RogueClassConsoleEvent*)(THIS->data->as_bytes))[THIS->count] = zero_value_2;
  return (RogueClassConsoleEvent)(result_1);
}

RogueClassConsoleEvent RogueConsoleEvent_List__remove_first( RogueConsoleEvent_List* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueClassConsoleEvent)(((RogueConsoleEvent_List__remove_at__Int32( ROGUE_ARG(THIS), 0 ))));
}

void RogueConsoleEvent_List__set__Int32_ConsoleEvent( RogueConsoleEvent_List* THIS, RogueInt32 index_0, RogueClassConsoleEvent new_value_1 )
{
  ROGUE_GC_CHECK;
  ((RogueClassConsoleEvent*)(THIS->data->as_bytes))[index_0] = new_value_1;
}

RogueString* RogueArray_ConsoleEvent___type_name( RogueArray* THIS )
{
  return (RogueString*)(Rogue_literal_strings[394]);
}

RogueClassFileReader* RogueFileReader__init_object( RogueClassFileReader* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  THIS->buffer = ((RogueByte_List*)(RogueByte_List__init__Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueByte_List*,ROGUE_CREATE_OBJECT(Byte_List))), 1024 )));
  return (RogueClassFileReader*)(THIS);
}

RogueString* RogueFileReader__to_String( RogueClassFileReader* THIS )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueByte_List*,buffer_0,(((RogueByte_List*)(RogueByte_List__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueByte_List*,ROGUE_CREATE_OBJECT(Byte_List))) )))));
  while (ROGUE_COND(((RogueFileReader__has_another( ROGUE_ARG(THIS) )))))
  {
    ROGUE_GC_CHECK;
    RogueByte_List__add__Byte( buffer_0, ROGUE_ARG(((RogueFileReader__read( ROGUE_ARG(THIS) )))) );
  }
  if (ROGUE_COND(true))
  {
    return (RogueString*)((RogueString__create__Byte_List_StringEncoding( buffer_0, RogueClassStringEncoding( 3 ) )));
  }
}

RogueString* RogueFileReader__type_name( RogueClassFileReader* THIS )
{
  return (RogueString*)(Rogue_literal_strings[349]);
}

void RogueFileReader__close( RogueClassFileReader* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!!(THIS->fp)))
  {
    fclose( THIS->fp );
    THIS->fp = 0;

  }
  THIS->position = 0;
  THIS->count = 0;
}

RogueLogical RogueFileReader__has_another( RogueClassFileReader* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(((THIS->position) < (THIS->count)));
}

RogueByte RogueFileReader__peek( RogueClassFileReader* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((THIS->position) == (THIS->count))))
  {
    return (RogueByte)(0);
  }
  if (ROGUE_COND(((THIS->buffer_position) == (THIS->buffer->count))))
  {
    THIS->buffer->count = (RogueInt32) fread( THIS->buffer->data->as_bytes, 1, 1024, THIS->fp );

    THIS->buffer_position = 0;
  }
  return (RogueByte)(THIS->buffer->data->as_bytes[THIS->buffer_position]);
}

RogueByte RogueFileReader__read( RogueClassFileReader* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((THIS->position) == (THIS->count))))
  {
    return (RogueByte)(0);
  }
  RogueByte result_0 = (((RogueFileReader__peek( ROGUE_ARG(THIS) ))));
  THIS->position = ((THIS->position) + (1));
  ++THIS->buffer_position;
  if (ROGUE_COND(((THIS->position) == (THIS->count))))
  {
    RogueFileReader__close( ROGUE_ARG(THIS) );
  }
  return (RogueByte)(result_0);
}

RogueInt32 RogueFileReader__read__Byte_List_Int32( RogueClassFileReader* THIS, RogueByte_List* result_0, RogueInt32 limit_1 )
{
  ROGUE_GC_CHECK;
  RogueByte_List__reserve__Int32( result_0, limit_1 );
  RogueInt32 total_read_2 = (0);
  RogueInt32 n_3 = (((RogueInt32__or_smaller__Int32( ROGUE_ARG(((THIS->buffer->count) - (THIS->buffer_position))), limit_1 ))));
  if (ROGUE_COND(((n_3) > (0))))
  {
    memcpy( result_0->data->as_bytes + result_0->count, THIS->buffer->data->as_bytes+THIS->buffer_position, n_3 );

    THIS->buffer_position += n_3;
    THIS->position += n_3;
    total_read_2 += n_3;
    limit_1 -= n_3;
  }
  if (ROGUE_COND(((limit_1) > (0))))
  {
    n_3 = (RogueInt32) fread( result_0->data->as_bytes+result_0->count, 1, limit_1, THIS->fp );

    total_read_2 += n_3;
    THIS->position += n_3;
  }
  if (ROGUE_COND(((THIS->position) == (THIS->count))))
  {
    RogueFileReader__close( ROGUE_ARG(THIS) );
  }
  result_0->count += total_read_2;
  return (RogueInt32)(total_read_2);
}

RogueClassFileReader* RogueFileReader__init__String( RogueClassFileReader* THIS, RogueString* _filepath_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(((RogueFileReader__open__String( ROGUE_ARG(THIS), _filepath_0 ))))))
  {
    throw ((RogueException*)(RogueIOError___throw( ROGUE_ARG(((RogueClassIOError*)(((RogueException*)Rogue_call_ROGUEM4( 13, ROGUE_ARG(((RogueException*)ROGUE_CREATE_REF(RogueClassIOError*,ROGUE_CREATE_OBJECT(IOError)))), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[103] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(THIS->filepath) ))) )))), Rogue_literal_strings[104] )))) )))) ))))) )));
  }
  return (RogueClassFileReader*)(THIS);
}

void RogueFileReader__on_cleanup( RogueClassFileReader* THIS )
{
  ROGUE_GC_CHECK;
  RogueFileReader__close( ROGUE_ARG(THIS) );
}

RogueInt64 RogueFileReader__fp( RogueClassFileReader* THIS )
{
  return (RogueInt64)(((RogueInt64)(THIS->fp)));
}

RogueLogical RogueFileReader__open__String( RogueClassFileReader* THIS, RogueString* _auto_634_0 )
{
  ROGUE_GC_CHECK;
  THIS->filepath = _auto_634_0;
  RogueFileReader__close( ROGUE_ARG(THIS) );
  THIS->filepath = (RogueFile__fix_slashes__String( ROGUE_ARG(THIS->filepath) ));
  THIS->filepath = (RogueFile__expand_path__String( ROGUE_ARG(THIS->filepath) ));
  THIS->fp = fopen( (char*)THIS->filepath->utf8, "rb" );
  if ( !THIS->fp ) return false;

  fseek( THIS->fp, 0, SEEK_END );
  THIS->count = (RogueInt32) ftell( THIS->fp );
  fseek( THIS->fp, 0, SEEK_SET );

  if (ROGUE_COND(((THIS->count) == (0))))
  {
    RogueFileReader__close( ROGUE_ARG(THIS) );
  }
  return (RogueLogical)(true);
}

void RogueReader_Byte___close( RogueObject* THIS )
{
  switch (THIS->type->index)
  {
    case 66:
      RogueFileReader__close( (RogueClassFileReader*)THIS );
    return;
    case 74:
      RogueWindowsProcessReader__close( (RogueClassWindowsProcessReader*)THIS );
    return;
    case 77:
      RoguePosixProcessReader__close( (RogueClassPosixProcessReader*)THIS );
    return;
    case 78:
      RogueFDReader__close( (RogueClassFDReader*)THIS );
    return;
  }
}

RogueLogical RogueReader_Byte___has_another( RogueObject* THIS )
{
  switch (THIS->type->index)
  {
    case 66:
      return RogueFileReader__has_another( (RogueClassFileReader*)THIS );
    case 74:
      return RogueWindowsProcessReader__has_another( (RogueClassWindowsProcessReader*)THIS );
    case 77:
      return RoguePosixProcessReader__has_another( (RogueClassPosixProcessReader*)THIS );
    case 78:
      return RogueFDReader__has_another( (RogueClassFDReader*)THIS );
    default:
      return 0;
  }
}

RogueByte RogueReader_Byte___read( RogueObject* THIS )
{
  switch (THIS->type->index)
  {
    case 66:
      return RogueFileReader__read( (RogueClassFileReader*)THIS );
    case 74:
      return RogueWindowsProcessReader__read( (RogueClassWindowsProcessReader*)THIS );
    case 77:
      return RoguePosixProcessReader__read( (RogueClassPosixProcessReader*)THIS );
    case 78:
      return RogueFDReader__read( (RogueClassFDReader*)THIS );
    default:
      return 0;
  }
}

RogueClassLineReader* RogueLineReader__init_object( RogueClassLineReader* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  THIS->buffer = ((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )));
  return (RogueClassLineReader*)(THIS);
}

RogueString* RogueLineReader__to_String( RogueClassLineReader* THIS )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueString_List*,buffer_0,(((RogueString_List*)(RogueString_List__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueString_List*,ROGUE_CREATE_OBJECT(String_List))) )))));
  while (ROGUE_COND((Rogue_call_ROGUEM5( 14, ROGUE_ARG(THIS) ))))
  {
    ROGUE_GC_CHECK;
    RogueString_List__add__String( buffer_0, ROGUE_ARG(((RogueString*)(RogueLineReader__read( ROGUE_ARG(THIS) )))) );
  }
  if (ROGUE_COND((false)))
  {
  }
  else
  {
    return (RogueString*)(((RogueString*)(RogueString_List__to_String( buffer_0 ))));
  }
}

RogueString* RogueLineReader__type_name( RogueClassLineReader* THIS )
{
  return (RogueString*)(Rogue_literal_strings[350]);
}

RogueLogical RogueLineReader__has_another( RogueClassLineReader* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(((RogueLineReader__prepare_next( ROGUE_ARG(THIS) ))));
}

RogueString* RogueLineReader__peek( RogueClassLineReader* THIS )
{
  ROGUE_GC_CHECK;
  RogueLineReader__prepare_next( ROGUE_ARG(THIS) );
  return (RogueString*)(THIS->next);
}

RogueString* RogueLineReader__read( RogueClassLineReader* THIS )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueString*,result_0,(((RogueString*)(RogueLineReader__peek( ROGUE_ARG(THIS) )))));
  THIS->next = ((RogueString*)(NULL));
  THIS->position = ((THIS->position) + (1));
  return (RogueString*)(result_0);
}

RogueClassLineReader* RogueLineReader__init__Reader_Character_( RogueClassLineReader* THIS, RogueClassReader_Character_* _auto_688_0 )
{
  ROGUE_GC_CHECK;
  THIS->source = _auto_688_0;
  THIS->next = ((RogueString*)(NULL));
  return (RogueClassLineReader*)(THIS);
}

RogueClassLineReader* RogueLineReader__init__Reader_Byte__StringEncoding( RogueClassLineReader* THIS, RogueClassReader_Byte_* reader_0, RogueClassStringEncoding encoding_1 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((RogueStringEncoding__operatorEQUALSEQUALS__StringEncoding( encoding_1, RogueClassStringEncoding( 2 ) )))))
  {
    RogueLineReader__init__Reader_Character_( ROGUE_ARG(THIS), ROGUE_ARG(((((RogueClassReader_Character_*)(((RogueClassExtendedASCIIReader*)(RogueExtendedASCIIReader__init__Reader_Byte_( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassExtendedASCIIReader*,ROGUE_CREATE_OBJECT(ExtendedASCIIReader))), ((reader_0)) )))))))) );
  }
  else
  {
    RogueLineReader__init__Reader_Character_( ROGUE_ARG(THIS), ROGUE_ARG(((((RogueClassReader_Character_*)(((RogueClassUTF8Reader*)(RogueUTF8Reader__init__Reader_Byte_( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassUTF8Reader*,ROGUE_CREATE_OBJECT(UTF8Reader))), ((reader_0)) )))))))) );
  }
  return (RogueClassLineReader*)(THIS);
}

RogueClassLineReader* RogueLineReader__init__File_StringEncoding( RogueClassLineReader* THIS, RogueClassFile* file_0, RogueClassStringEncoding encoding_1 )
{
  ROGUE_GC_CHECK;
  RogueLineReader__init__Reader_Byte__StringEncoding( ROGUE_ARG(THIS), ROGUE_ARG(((((RogueClassReader_Byte_*)(((RogueClassFileReader*)(RogueFile__reader( file_0 )))))))), encoding_1 );
  return (RogueClassLineReader*)(THIS);
}

RogueLogical RogueLineReader__prepare_next( RogueClassLineReader* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!!(THIS->next)))
  {
    return (RogueLogical)(true);
  }
  if (ROGUE_COND(!((RogueReader_Character___has_another( ROGUE_ARG(((((RogueObject*)THIS->source)))) )))))
  {
    return (RogueLogical)(false);
  }
  THIS->prev = (RogueCharacter)0;
  RogueStringBuilder__clear( ROGUE_ARG(THIS->buffer) );
  while (ROGUE_COND((RogueReader_Character___has_another( ROGUE_ARG(((((RogueObject*)THIS->source)))) ))))
  {
    ROGUE_GC_CHECK;
    RogueCharacter ch_0 = ((RogueReader_Character___read( ROGUE_ARG(((((RogueObject*)THIS->source)))) )));
    if (ROGUE_COND(((ch_0) == ((RogueCharacter)13))))
    {
      continue;
    }
    if (ROGUE_COND(((ch_0) == ((RogueCharacter)10))))
    {
      THIS->prev = (RogueCharacter)10;
      goto _auto_691;
    }
    RogueStringBuilder__print__Character( ROGUE_ARG(THIS->buffer), ch_0 );
  }
  _auto_691:;
  THIS->next = ((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(THIS->buffer) )));
  return (RogueLogical)(true);
}

RogueClassFunction_819* RogueFunction_819__init_object( RogueClassFunction_819* THIS )
{
  ROGUE_GC_CHECK;
  Rogue_Function_____init_object( ROGUE_ARG(((RogueClass_Function___*)THIS)) );
  return (RogueClassFunction_819*)(THIS);
}

RogueString* RogueFunction_819__type_name( RogueClassFunction_819* THIS )
{
  return (RogueString*)(Rogue_literal_strings[395]);
}

void RogueFunction_819__call( RogueClassFunction_819* THIS )
{
  RogueGlobal__flush( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)) );
}

RogueClassProcess* RogueProcess__init_object( RogueClassProcess* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  return (RogueClassProcess*)(THIS);
}

RogueString* RogueProcess__type_name( RogueClassProcess* THIS )
{
  return (RogueString*)(Rogue_literal_strings[353]);
}

RogueByte_List* RogueProcess__error_bytes( RogueClassProcess* THIS )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueByte_List*,result_0,(((RogueByte_List*)(RogueByte_List__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueByte_List*,ROGUE_CREATE_OBJECT(Byte_List))) )))));
  if (ROGUE_COND(!!(THIS->error_reader)))
  {
    while (ROGUE_COND((RogueReader_Byte___has_another( ROGUE_ARG(((((RogueObject*)THIS->error_reader)))) ))))
    {
      ROGUE_GC_CHECK;
      RogueByte_List__add__Byte( result_0, ROGUE_ARG((RogueReader_Byte___read( ROGUE_ARG(((((RogueObject*)THIS->error_reader)))) ))) );
    }
  }
  return (RogueByte_List*)(result_0);
}

RogueClassProcessResult* RogueProcess__finish( RogueClassProcess* THIS )
{
  ROGUE_GC_CHECK;
  THIS->is_finished = true;
  if (ROGUE_COND(!(!!(THIS->result))))
  {
    THIS->result = ((RogueClassProcessResult*)(RogueProcessResult__init__Int32_Byte_List_Byte_List( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassProcessResult*,ROGUE_CREATE_OBJECT(ProcessResult))), 1, ROGUE_ARG(((RogueByte_List*)(RogueByte_List__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueByte_List*,ROGUE_CREATE_OBJECT(Byte_List))) )))), ROGUE_ARG(((RogueByte_List*)(RogueByte_List__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueByte_List*,ROGUE_CREATE_OBJECT(Byte_List))) )))) )));
  }
  return (RogueClassProcessResult*)(THIS->result);
}

void RogueProcess__on_cleanup( RogueClassProcess* THIS )
{
  ROGUE_GC_CHECK;
  Rogue_call_ROGUEM0( 19, ROGUE_ARG(THIS) );
}

RogueByte_List* RogueProcess__output_bytes( RogueClassProcess* THIS )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueByte_List*,result_0,(((RogueByte_List*)(RogueByte_List__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueByte_List*,ROGUE_CREATE_OBJECT(Byte_List))) )))));
  if (ROGUE_COND(!!(THIS->output_reader)))
  {
    while (ROGUE_COND((RogueReader_Byte___has_another( ROGUE_ARG(((((RogueObject*)THIS->output_reader)))) ))))
    {
      ROGUE_GC_CHECK;
      RogueByte_List__add__Byte( result_0, ROGUE_ARG((RogueReader_Byte___read( ROGUE_ARG(((((RogueObject*)THIS->output_reader)))) ))) );
    }
  }
  return (RogueByte_List*)(result_0);
}

RogueLogical RogueProcess__update_io__Logical( RogueClassProcess* THIS, RogueLogical poll_blocks_0 )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(false);
}

RogueClassProcessResult* RogueProcessResult__init_object( RogueClassProcessResult* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  return (RogueClassProcessResult*)(THIS);
}

RogueString* RogueProcessResult__to_String( RogueClassProcessResult* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(!!(((RogueString__count( ROGUE_ARG(((RogueString*)(RogueProcessResult__output_string( ROGUE_ARG(THIS) )))) )))))))
  {
    return (RogueString*)(((RogueString*)(RogueProcessResult__error_string( ROGUE_ARG(THIS) ))));
  }
  if (ROGUE_COND(!(!!(((RogueString__count( ROGUE_ARG(((RogueString*)(RogueProcessResult__error_string( ROGUE_ARG(THIS) )))) )))))))
  {
    return (RogueString*)(((RogueString*)(RogueProcessResult__output_string( ROGUE_ARG(THIS) ))));
  }
  return (RogueString*)((RogueString__operatorPLUS__String_String( ROGUE_ARG(((RogueString*)(RogueProcessResult__output_string( ROGUE_ARG(THIS) )))), ROGUE_ARG(((RogueString*)(RogueProcessResult__error_string( ROGUE_ARG(THIS) )))) )));
}

RogueString* RogueProcessResult__type_name( RogueClassProcessResult* THIS )
{
  return (RogueString*)(Rogue_literal_strings[359]);
}

RogueClassProcessResult* RogueProcessResult__init__Int32_Byte_List_Byte_List( RogueClassProcessResult* THIS, RogueInt32 _auto_837_0, RogueByte_List* _auto_836_1, RogueByte_List* _auto_835_2 )
{
  ROGUE_GC_CHECK;
  THIS->exit_code = _auto_837_0;
  THIS->output_bytes = _auto_836_1;
  THIS->error_bytes = _auto_835_2;
  return (RogueClassProcessResult*)(THIS);
}

RogueString* RogueProcessResult__error_string( RogueClassProcessResult* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(!!(THIS->error_string))))
  {
    THIS->error_string = (RogueString__create__Byte_List_StringEncoding( ROGUE_ARG(THIS->error_bytes), RogueClassStringEncoding( 3 ) ));
  }
  return (RogueString*)(THIS->error_string);
}

RogueString* RogueProcessResult__output_string( RogueClassProcessResult* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(!!(THIS->output_string))))
  {
    THIS->output_string = (RogueString__create__Byte_List_StringEncoding( ROGUE_ARG(THIS->output_bytes), RogueClassStringEncoding( 3 ) ));
  }
  return (RogueString*)(THIS->output_string);
}

RogueLogical RogueProcessResult__success( RogueClassProcessResult* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(((0) == (THIS->exit_code)));
}

RogueClassWindowsProcess* RogueWindowsProcess__init_object( RogueClassWindowsProcess* THIS )
{
  ROGUE_GC_CHECK;
  RogueProcess__init_object( ROGUE_ARG(((RogueClassProcess*)THIS)) );
  return (RogueClassWindowsProcess*)(THIS);
}

RogueString* RogueWindowsProcess__type_name( RogueClassWindowsProcess* THIS )
{
  return (RogueString*)(Rogue_literal_strings[435]);
}

RogueLogical RogueWindowsProcess__is_finished( RogueClassWindowsProcess* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(THIS->is_finished))
  {
    return (RogueLogical)(true);
  }
  if (ROGUE_COND(!(THIS->process_created)))
  {
    THIS->is_finished = true;
    return (RogueLogical)(true);
  }
#ifdef ROGUE_PLATFORM_WINDOWS
  DWORD exit_code;
  if (GetExitCodeProcess( THIS->process_info.hProcess, &exit_code ))
  {
    if (exit_code != STILL_ACTIVE)
    {
      THIS->exit_code = exit_code;
      THIS->is_finished = true;
    }
  }
#endif

  while (ROGUE_COND(((RogueWindowsProcess__update_io( ROGUE_ARG(THIS) )))))
  {
    ROGUE_GC_CHECK;
  }
  return (RogueLogical)(THIS->is_finished);
}

RogueClassProcessResult* RogueWindowsProcess__finish( RogueClassWindowsProcess* THIS )
{
  ROGUE_GC_CHECK;
#ifdef ROGUE_PLATFORM_WINDOWS

  while (ROGUE_COND(!(((RogueWindowsProcess__is_finished( ROGUE_ARG(THIS) ))))))
  {
    ROGUE_GC_CHECK;
    RogueWindowsProcessWriter__close_pipe( ROGUE_ARG(((RogueClassWindowsProcessWriter*)(RogueObject_as(THIS->input_writer,RogueTypeWindowsProcessWriter)))) );
    WaitForSingleObject( THIS->process_info.hProcess, 250 );

  }
  RogueWindowsProcess__on_cleanup( ROGUE_ARG(THIS) );
#endif

  if (ROGUE_COND(!(!!(THIS->result))))
  {
    THIS->result = ((RogueClassProcessResult*)(RogueProcessResult__init__Int32_Byte_List_Byte_List( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassProcessResult*,ROGUE_CREATE_OBJECT(ProcessResult))), ROGUE_ARG(THIS->exit_code), ROGUE_ARG(((RogueByte_List*)(RogueProcess__output_bytes( ROGUE_ARG(((RogueClassProcess*)THIS)) )))), ROGUE_ARG(((RogueByte_List*)(RogueProcess__error_bytes( ROGUE_ARG(((RogueClassProcess*)THIS)) )))) )));
  }
  return (RogueClassProcessResult*)(THIS->result);
}

RogueLogical RogueWindowsProcess__launch__Logical_Logical_Logical( RogueClassWindowsProcess* THIS, RogueLogical readable_0, RogueLogical writable_1, RogueLogical inherit_environment_2 )
{
  ROGUE_GC_CHECK;
#ifdef ROGUE_PLATFORM_WINDOWS

  THIS->output_reader = ((RogueClassReader_Byte_*)(((RogueClassWindowsProcessReader*)(RogueWindowsProcessReader__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassWindowsProcessReader*,ROGUE_CREATE_OBJECT(WindowsProcessReader))) )))));
  THIS->error_reader = ((RogueClassReader_Byte_*)(((RogueClassWindowsProcessReader*)(RogueWindowsProcessReader__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassWindowsProcessReader*,ROGUE_CREATE_OBJECT(WindowsProcessReader))) )))));
  THIS->input_writer = ((RogueClassWriter_Byte_*)(((RogueClassWindowsProcessWriter*)(RogueWindowsProcessWriter__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassWindowsProcessWriter*,ROGUE_CREATE_OBJECT(WindowsProcessWriter))) )))));
  THIS->arg_string = ((RogueString*)(RogueString__cloned( ROGUE_ARG(((RogueString*)(RogueString_List__join__String( ROGUE_ARG(THIS->args), Rogue_literal_strings[42] )))) )));
  ROGUE_DEF_LOCAL_REF(RogueClassWindowsProcessReader*,output_reader_3,(((RogueClassWindowsProcessReader*)(RogueObject_as(THIS->output_reader,RogueTypeWindowsProcessReader)))));
  ROGUE_DEF_LOCAL_REF(RogueClassWindowsProcessReader*,error_reader_4,(((RogueClassWindowsProcessReader*)(RogueObject_as(THIS->error_reader,RogueTypeWindowsProcessReader)))));
  ROGUE_DEF_LOCAL_REF(RogueClassWindowsProcessWriter*,input_writer_5,(((RogueClassWindowsProcessWriter*)(RogueObject_as(THIS->input_writer,RogueTypeWindowsProcessWriter)))));
  THIS->startup_info.cb = sizeof( STARTUPINFO );
  THIS->startup_info.hStdOutput = output_reader_3->output_writer;
  THIS->startup_info.hStdError =  error_reader_4->output_writer;
  THIS->startup_info.hStdInput =  input_writer_5->input_reader;
  THIS->startup_info.dwFlags |= STARTF_USESTDHANDLES;

  if ( !CreateProcess(
      NULL,                  // lpApplicationName
      THIS->arg_string->utf8,     // lpCommandLine
      NULL,                  // lpProcessAttributes
      NULL,                  // lpThreadAttributes
      TRUE,                  // bInheritHandles
      0,                     // dwCreationFlags  (CREATE_NO_WINDOW?)
      NULL,                  // lpEnvironment
      NULL,                  // lpCurrentDirectory
      &THIS->startup_info,  // lpStartupInfo
      &THIS->process_info   // lpProcessInformation
  )) return false;

  THIS->process_created = true;
  return true;

#else
  return false;

#endif

}

void RogueWindowsProcess__on_cleanup( RogueClassWindowsProcess* THIS )
{
  ROGUE_GC_CHECK;
  RogueReader_Byte___close( ROGUE_ARG(((((RogueObject*)THIS->output_reader)))) );
  RogueReader_Byte___close( ROGUE_ARG(((((RogueObject*)THIS->error_reader)))) );
  RogueWriter_Byte___close( ROGUE_ARG(((((RogueObject*)THIS->input_writer)))) );
  if (ROGUE_COND(THIS->process_created))
  {
#ifdef ROGUE_PLATFORM_WINDOWS
    CloseHandle( THIS->process_info.hProcess );
    CloseHandle( THIS->process_info.hThread );
#endif

    THIS->process_created = false;
  }
}

RogueClassWindowsProcess* RogueWindowsProcess__init__String_Logical_Logical_Logical_Logical_Logical( RogueClassWindowsProcess* THIS, RogueString* _auto_4020, RogueLogical readable_1, RogueLogical writable_2, RogueLogical is_blocking_3, RogueLogical inherit_environment_4, RogueLogical env_5 )
{
  ROGUE_DEF_LOCAL_REF(RogueString*,cmd_0,_auto_4020);
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((((RogueString__count__Character( cmd_0, (RogueCharacter)'"' )))) > (2))))
  {
    cmd_0 = ((RogueString*)(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[38] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], cmd_0 ))) )))), Rogue_literal_strings[38] )))) )))));
  }
  ROGUE_DEF_LOCAL_REF(RogueString_List*,_auto_822_0,(((RogueString_List*)(RogueString_List__init__Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueString_List*,ROGUE_CREATE_OBJECT(String_List))), 3 )))));
  {
    RogueString_List__add__String( _auto_822_0, Rogue_literal_strings[39] );
    RogueString_List__add__String( _auto_822_0, Rogue_literal_strings[40] );
    RogueString_List__add__String( _auto_822_0, cmd_0 );
  }
  RogueWindowsProcess__init__String_List_Logical_Logical_Logical_Logical_Logical( ROGUE_ARG(THIS), _auto_822_0, readable_1, writable_2, is_blocking_3, inherit_environment_4, env_5 );
  return (RogueClassWindowsProcess*)(THIS);
}

RogueClassWindowsProcess* RogueWindowsProcess__init__String_List_Logical_Logical_Logical_Logical_Logical( RogueClassWindowsProcess* THIS, RogueString_List* _auto_821_0, RogueLogical readable_1, RogueLogical writable_2, RogueLogical _auto_820_3, RogueLogical inherit_environment_4, RogueLogical env_5 )
{
  ROGUE_GC_CHECK;
  THIS->args = _auto_821_0;
  THIS->is_blocking = _auto_820_3;
  if (ROGUE_COND(env_5))
  {
    inherit_environment_4 = ((RogueLogical)(env_5));
  }
  if (ROGUE_COND(!(((RogueWindowsProcess__launch__Logical_Logical_Logical( ROGUE_ARG(THIS), readable_1, writable_2, inherit_environment_4 ))))))
  {
    THIS->error = true;
    THIS->is_finished = true;
    THIS->exit_code = 1;
  }
  return (RogueClassWindowsProcess*)(THIS);
}

RogueLogical RogueWindowsProcess__update_io( RogueClassWindowsProcess* THIS )
{
  ROGUE_GC_CHECK;
  RogueLogical any_updates_0 = (false);
  if (ROGUE_COND(((RogueWindowsProcessReader__fill_buffer( ROGUE_ARG(((RogueClassWindowsProcessReader*)(RogueObject_as(THIS->output_reader,RogueTypeWindowsProcessReader)))) )))))
  {
    any_updates_0 = ((RogueLogical)(true));
  }
  if (ROGUE_COND(((RogueWindowsProcessReader__fill_buffer( ROGUE_ARG(((RogueClassWindowsProcessReader*)(RogueObject_as(THIS->error_reader,RogueTypeWindowsProcessReader)))) )))))
  {
    any_updates_0 = ((RogueLogical)(true));
  }
  RogueInt32 before_count_1 = (((RogueClassWindowsProcessWriter*)(RogueObject_as(THIS->input_writer,RogueTypeWindowsProcessWriter)))->buffer->count);
  RogueWriter_Byte___flush( ROGUE_ARG(((((RogueObject*)THIS->input_writer)))) );
  if (ROGUE_COND(((((RogueClassWindowsProcessWriter*)(RogueObject_as(THIS->input_writer,RogueTypeWindowsProcessWriter)))->buffer->count) != (before_count_1))))
  {
    any_updates_0 = ((RogueLogical)(true));
  }
  return (RogueLogical)(any_updates_0);
}

RogueClassWindowsProcessReader* RogueWindowsProcessReader__init_object( RogueClassWindowsProcessReader* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  THIS->buffer = ((RogueByte_List*)(RogueByte_List__init__Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueByte_List*,ROGUE_CREATE_OBJECT(Byte_List))), 1024 )));
  return (RogueClassWindowsProcessReader*)(THIS);
}

RogueClassWindowsProcessReader* RogueWindowsProcessReader__init( RogueClassWindowsProcessReader* THIS )
{
  ROGUE_GC_CHECK;
  RogueLogical success_0 = (false);
#ifdef ROGUE_PLATFORM_WINDOWS
  SECURITY_ATTRIBUTES security_attributes;
  security_attributes.nLength = sizeof( SECURITY_ATTRIBUTES );
  security_attributes.bInheritHandle = TRUE;
  security_attributes.lpSecurityDescriptor = NULL;
  if ( !CreatePipe(&THIS->output_reader,&THIS->output_writer,&security_attributes,0) ) goto ERROR_EXIT;
  if ( !SetHandleInformation(THIS->output_reader,HANDLE_FLAG_INHERIT,0) ) goto ERROR_EXIT;
  success_0 = true;
  ERROR_EXIT:
#endif

  if (ROGUE_COND(!(success_0)))
  {
    throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(((RogueException*)Rogue_call_ROGUEM4( 13, ROGUE_ARG(((RogueException*)ROGUE_CREATE_REF(RogueClassError*,ROGUE_CREATE_OBJECT(Error)))), Rogue_literal_strings[41] ))))) ));
  }
  return (RogueClassWindowsProcessReader*)(THIS);
}

RogueString* RogueWindowsProcessReader__to_String( RogueClassWindowsProcessReader* THIS )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueByte_List*,buffer_0,(((RogueByte_List*)(RogueByte_List__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueByte_List*,ROGUE_CREATE_OBJECT(Byte_List))) )))));
  while (ROGUE_COND(((RogueWindowsProcessReader__has_another( ROGUE_ARG(THIS) )))))
  {
    ROGUE_GC_CHECK;
    RogueByte_List__add__Byte( buffer_0, ROGUE_ARG(((RogueWindowsProcessReader__read( ROGUE_ARG(THIS) )))) );
  }
  if (ROGUE_COND(true))
  {
    return (RogueString*)((RogueString__create__Byte_List_StringEncoding( buffer_0, RogueClassStringEncoding( 3 ) )));
  }
}

RogueString* RogueWindowsProcessReader__type_name( RogueClassWindowsProcessReader* THIS )
{
  return (RogueString*)(Rogue_literal_strings[354]);
}

void RogueWindowsProcessReader__close( RogueClassWindowsProcessReader* THIS )
{
  ROGUE_GC_CHECK;
#ifdef ROGUE_PLATFORM_WINDOWS
  if (THIS->output_reader)
  {
    CloseHandle( THIS->output_reader );
    THIS->output_reader = NULL;
  }
  if (THIS->output_writer)
  {
    CloseHandle( THIS->output_writer );
    THIS->output_writer = NULL;
  }
#endif

}

RogueLogical RogueWindowsProcessReader__has_another( RogueClassWindowsProcessReader* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!!(THIS->buffer->count)))
  {
    return (RogueLogical)(true);
  }
  return (RogueLogical)(((RogueWindowsProcessReader__fill_buffer( ROGUE_ARG(THIS) ))));
}

RogueByte RogueWindowsProcessReader__read( RogueClassWindowsProcessReader* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(((RogueWindowsProcessReader__has_another( ROGUE_ARG(THIS) ))))))
  {
    return (RogueByte)(0);
  }
  THIS->position = ((THIS->position) + (1));
  return (RogueByte)(((RogueByte_List__remove_first( ROGUE_ARG(THIS->buffer) ))));
}

RogueLogical RogueWindowsProcessReader__fill_buffer( RogueClassWindowsProcessReader* THIS )
{
  ROGUE_GC_CHECK;
  RogueByte_List__reserve__Int32( ROGUE_ARG(THIS->buffer), 1024 );
#ifdef ROGUE_PLATFORM_WINDOWS
  if (THIS->output_reader)
  {
    DWORD available_count;
    if (PeekNamedPipe( THIS->output_reader, NULL, 0, NULL, &available_count, NULL ))
    {
      if (available_count)
      {
        DWORD read_count;
        if (ReadFile(THIS->output_reader, THIS->buffer->data->as_bytes+THIS->buffer->count, 1024, &read_count, NULL))
        {
          if (read_count)
          {
            THIS->buffer->count += read_count;
            return true;
          }
        }
      }
    }
  }
#endif

  return (RogueLogical)(false);
}

RogueClassWindowsProcessWriter* RogueWindowsProcessWriter__init_object( RogueClassWindowsProcessWriter* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  THIS->buffer = ((RogueByte_List*)(RogueByte_List__init__Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueByte_List*,ROGUE_CREATE_OBJECT(Byte_List))), 1024 )));
  return (RogueClassWindowsProcessWriter*)(THIS);
}

RogueClassWindowsProcessWriter* RogueWindowsProcessWriter__init( RogueClassWindowsProcessWriter* THIS )
{
  ROGUE_GC_CHECK;
  RogueLogical success_0 = (false);
#ifdef ROGUE_PLATFORM_WINDOWS
  SECURITY_ATTRIBUTES security_attributes;
  security_attributes.nLength = sizeof( SECURITY_ATTRIBUTES );
  security_attributes.bInheritHandle = TRUE;
  security_attributes.lpSecurityDescriptor = NULL;
  if ( !CreatePipe(&THIS->input_reader,&THIS->input_writer,&security_attributes,0) ) goto ERROR_EXIT;
  if ( !SetHandleInformation(THIS->input_writer,HANDLE_FLAG_INHERIT,0) ) goto ERROR_EXIT;
  success_0 = true;
  ERROR_EXIT:
#endif

  if (ROGUE_COND(!(success_0)))
  {
    throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(((RogueException*)Rogue_call_ROGUEM4( 13, ROGUE_ARG(((RogueException*)ROGUE_CREATE_REF(RogueClassError*,ROGUE_CREATE_OBJECT(Error)))), Rogue_literal_strings[41] ))))) ));
  }
  return (RogueClassWindowsProcessWriter*)(THIS);
}

RogueString* RogueWindowsProcessWriter__type_name( RogueClassWindowsProcessWriter* THIS )
{
  return (RogueString*)(Rogue_literal_strings[355]);
}

void RogueWindowsProcessWriter__close( RogueClassWindowsProcessWriter* THIS )
{
  ROGUE_GC_CHECK;
#ifdef ROGUE_PLATFORM_WINDOWS
  if (THIS->input_reader)
  {
    CloseHandle( THIS->input_reader );
    THIS->input_reader = NULL;
  }
#endif

  RogueWindowsProcessWriter__close_pipe( ROGUE_ARG(THIS) );
}

void RogueWindowsProcessWriter__flush( RogueClassWindowsProcessWriter* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(!!(THIS->buffer->count))))
  {
    return;
  }
  RogueInt32 write_count_0 = (0);
#ifdef ROGUE_PLATFORM_WINDOWS
  if (THIS->input_writer)
  {
    DWORD write_count;
    if (WriteFile(THIS->input_writer,THIS->buffer->data->as_bytes,THIS->buffer->count,&write_count,NULL))
    {
      write_count_0 = write_count;
    }
  }
#endif

  if (ROGUE_COND(!!(write_count_0)))
  {
    THIS->position += write_count_0;
    RogueByte_List__discard__Int32_Int32( ROGUE_ARG(THIS->buffer), 0, write_count_0 );
  }
}

void RogueWindowsProcessWriter__close_pipe( RogueClassWindowsProcessWriter* THIS )
{
  ROGUE_GC_CHECK;
#ifdef ROGUE_PLATFORM_WINDOWS
  if (THIS->input_writer)
  {
    CloseHandle( THIS->input_writer );
    THIS->input_writer = NULL;
  }
#endif

}

RogueClassPosixProcess* RoguePosixProcess__init_object( RogueClassPosixProcess* THIS )
{
  ROGUE_GC_CHECK;
  RogueProcess__init_object( ROGUE_ARG(((RogueClassProcess*)THIS)) );
  return (RogueClassPosixProcess*)(THIS);
}

RogueString* RoguePosixProcess__type_name( RogueClassPosixProcess* THIS )
{
  return (RogueString*)(Rogue_literal_strings[436]);
}

RogueClassProcessResult* RoguePosixProcess__finish( RogueClassPosixProcess* THIS )
{
  ROGUE_GC_CHECK;
#ifndef ROGUE_PLATFORM_WINDOWS

  if (ROGUE_COND(!(THIS->is_finished)))
  {
    THIS->is_finished = true;
    if (ROGUE_COND(!!(THIS->input_writer)))
    {
      RogueWriter_Byte___close( ROGUE_ARG(((((RogueObject*)THIS->input_writer)))) );
    }
    while (ROGUE_COND(((RoguePosixProcess__update_io__Logical( ROGUE_ARG(THIS), true )))))
    {
      ROGUE_GC_CHECK;
    }
    int status;
    if (THIS->pid)
    {
      waitpid( THIS->pid, &status, 0 );
      THIS->exit_code = WEXITSTATUS( status );
      THIS->pid = 0;
    }

    if (ROGUE_COND(!!(THIS->output_reader)))
    {
      RogueFDReader__close( ROGUE_ARG(((RogueClassPosixProcessReader*)(RogueObject_as(THIS->output_reader,RogueTypePosixProcessReader)))->fd_reader) );
      RogueFDReader__close( ROGUE_ARG(((RogueClassPosixProcessReader*)(RogueObject_as(THIS->error_reader,RogueTypePosixProcessReader)))->fd_reader) );
    }
    posix_spawn_file_actions_destroy( &THIS->actions );

  }
  if (THIS->cout_pipe[1] != -1) close( THIS->cout_pipe[1] );
  if (THIS->cerr_pipe[1] != -1) close( THIS->cerr_pipe[1] );
  if (THIS->cin_pipe[0] != -1)  close( THIS->cin_pipe[0] );
  THIS->cout_pipe[1] = -1;
  THIS->cerr_pipe[1] = -1;
  THIS->cin_pipe[0] = -1;
  if (THIS->cout_pipe[0] != -1) close( THIS->cout_pipe[0] );
  if (THIS->cerr_pipe[0] != -1) close( THIS->cerr_pipe[0] );
  if (THIS->cin_pipe[1] != -1)  close( THIS->cin_pipe[1] );
  THIS->cout_pipe[0] = -1;
  THIS->cerr_pipe[0] = -1;
  THIS->cin_pipe[1] = -1;

#endif // not ROGUE_PLATFORM_WINDOWS

  if (ROGUE_COND(!(!!(THIS->result))))
  {
    THIS->result = ((RogueClassProcessResult*)(RogueProcessResult__init__Int32_Byte_List_Byte_List( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassProcessResult*,ROGUE_CREATE_OBJECT(ProcessResult))), ROGUE_ARG(THIS->exit_code), ROGUE_ARG(((RogueByte_List*)(RogueProcess__output_bytes( ROGUE_ARG(((RogueClassProcess*)THIS)) )))), ROGUE_ARG(((RogueByte_List*)(RogueProcess__error_bytes( ROGUE_ARG(((RogueClassProcess*)THIS)) )))) )));
  }
  return (RogueClassProcessResult*)(THIS->result);
}

RogueLogical RoguePosixProcess__launch__Logical_Logical_Logical( RogueClassPosixProcess* THIS, RogueLogical readable_0, RogueLogical writable_1, RogueLogical inherit_environment_2 )
{
  ROGUE_GC_CHECK;
#ifndef ROGUE_PLATFORM_WINDOWS
  THIS->cin_pipe[0]  = THIS->cin_pipe[1]  = -1;
  THIS->cout_pipe[0] = THIS->cout_pipe[1] = -1;
  THIS->cerr_pipe[0] = THIS->cerr_pipe[1] = -1;

  memset(&THIS->poll_list, 0, sizeof(THIS->poll_list));
  THIS->poll_list[0].fd = -1;
  THIS->poll_list[1].fd = -1;

  if (readable_0)
  {
    if (0 != pipe(THIS->cout_pipe) || 0 != pipe(THIS->cerr_pipe)) return false;
  }

  if (writable_1)
  {
    if (0 != pipe(THIS->cin_pipe)) return false;
  }

  RogueInt32 n_3 = (THIS->args->count);
  char** args = new char*[ n_3+1 ];
  args[ n_3 ] = 0;

  {
    ROGUE_DEF_LOCAL_REF(RogueString_List*,_auto_827_0,(THIS->args));
    RogueInt32 index_0 = (0);
    RogueInt32 _auto_828_0 = (((_auto_827_0->count) - (1)));
    for (;ROGUE_COND(((index_0) <= (_auto_828_0)));++index_0)
    {
      ROGUE_GC_CHECK;
      ROGUE_DEF_LOCAL_REF(RogueString*,arg_0,(((RogueString*)(_auto_827_0->data->as_objects[index_0]))));
      args[ index_0 ] = (char*) arg_0->utf8;

    }
  }
  posix_spawn_file_actions_init( &THIS->actions );

  if (ROGUE_COND(readable_0))
  {
    posix_spawn_file_actions_addclose( &THIS->actions, THIS->cout_pipe[0] );
    posix_spawn_file_actions_addclose( &THIS->actions, THIS->cerr_pipe[0] );
    posix_spawn_file_actions_adddup2( &THIS->actions, THIS->cout_pipe[1], STDOUT_FILENO );
    posix_spawn_file_actions_adddup2( &THIS->actions, THIS->cerr_pipe[1], STDERR_FILENO );
    posix_spawn_file_actions_addclose( &THIS->actions, THIS->cout_pipe[1] );
    posix_spawn_file_actions_addclose( &THIS->actions, THIS->cerr_pipe[1] );
    THIS->poll_list[0].fd = THIS->cout_pipe[0];
    THIS->poll_list[1].fd = THIS->cerr_pipe[0];
    THIS->poll_list[0].events = POLLIN;
    THIS->poll_list[1].events = POLLIN;

  }
  if (ROGUE_COND(writable_1))
  {
      posix_spawn_file_actions_addclose( &THIS->actions, THIS->cin_pipe[1] );
      posix_spawn_file_actions_adddup2( &THIS->actions, THIS->cin_pipe[0], STDIN_FILENO );
      posix_spawn_file_actions_addclose( &THIS->actions, THIS->cin_pipe[0] );

  }
  else
  {
      posix_spawn_file_actions_addclose( &THIS->actions, STDIN_FILENO );

  }
  int result = posix_spawnp( &THIS->pid, args[0], &THIS->actions, NULL, &args[0],
      (inherit_environment_2 ? environ : NULL) );
  delete [] args;
  if (THIS->cout_pipe[1] != -1) close( THIS->cout_pipe[1] );
  if (THIS->cerr_pipe[1] != -1) close( THIS->cerr_pipe[1] );
  if (THIS->cin_pipe[0] != -1)  close( THIS->cin_pipe[0] );
  THIS->cout_pipe[1] = -1;
  THIS->cerr_pipe[1] = -1;
  THIS->cin_pipe[0] = -1;
  if (0 != result) return false;

  if (ROGUE_COND(readable_0))
  {
    THIS->output_reader = ((RogueClassReader_Byte_*)(((RogueClassPosixProcessReader*)(RoguePosixProcessReader__init__Process_Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassPosixProcessReader*,ROGUE_CREATE_OBJECT(PosixProcessReader))), ROGUE_ARG(((RogueClassProcess*)(THIS))), ROGUE_ARG(((RogueInt32)(THIS->cout_pipe[0]))) )))));
    THIS->error_reader = ((RogueClassReader_Byte_*)(((RogueClassPosixProcessReader*)(RoguePosixProcessReader__init__Process_Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassPosixProcessReader*,ROGUE_CREATE_OBJECT(PosixProcessReader))), ROGUE_ARG(((RogueClassProcess*)(THIS))), ROGUE_ARG(((RogueInt32)(THIS->cerr_pipe[0]))) )))));
  }
  if (ROGUE_COND(writable_1))
  {
    THIS->input_writer = ((RogueClassWriter_Byte_*)(((RogueClassFDWriter*)(RogueFDWriter__init__Int32_Logical( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassFDWriter*,ROGUE_CREATE_OBJECT(FDWriter))), ROGUE_ARG(((RogueInt32)(THIS->cin_pipe[1]))), true )))));
  }
#endif

  return (RogueLogical)(true);
}

RogueLogical RoguePosixProcess__update_io__Logical( RogueClassPosixProcess* THIS, RogueLogical poll_blocks_0 )
{
  ROGUE_GC_CHECK;
#ifndef ROGUE_PLATFORM_WINDOWS

  RogueInt32 fd0_1 = (-1);
  RogueInt32 fd1_2 = (-1);
  if (ROGUE_COND(!!(THIS->output_reader)))
  {
    fd0_1 = ((RogueInt32)(((RogueClassPosixProcessReader*)(RogueObject_as(THIS->output_reader,RogueTypePosixProcessReader)))->fd_reader->fd));
  }
  if (ROGUE_COND(!!(THIS->error_reader)))
  {
    fd1_2 = ((RogueInt32)(((RogueClassPosixProcessReader*)(RogueObject_as(THIS->error_reader,RogueTypePosixProcessReader)))->fd_reader->fd));
  }
  if (ROGUE_COND(((((fd0_1) == (-1))) && (((fd1_2) == (-1))))))
  {
    return (RogueLogical)(false);
  }
  RogueInt32 status_3 = 0;
  THIS->poll_list[0].fd = fd0_1;
  THIS->poll_list[1].fd = fd1_2;
  status_3 = poll( THIS->poll_list, 2, (poll_blocks_0?-1:0) );

  if (ROGUE_COND(((status_3) > (0))))
  {
    RogueLogical keep_going_4 = (false);
    {
      RogueInt32 which_fd_7 = (0);
      RogueInt32 _auto_824_8 = (2);
      for (;ROGUE_COND(((which_fd_7) < (_auto_824_8)));++which_fd_7)
      {
        ROGUE_GC_CHECK;
        RogueLogical has_input_5 = (((RogueLogical)(THIS->poll_list[which_fd_7].revents & POLLIN)));
        ROGUE_DEF_LOCAL_REF(RogueClassReader_Byte_*,reader_6,(((((((which_fd_7) == (0))))) ? (ROGUE_ARG((RogueClassReader_Byte_*)THIS->output_reader)) : ROGUE_ARG(THIS->error_reader))));
        if (ROGUE_COND(has_input_5))
        {
          RogueFDReader__buffer_more( ROGUE_ARG(((RogueClassPosixProcessReader*)(RogueObject_as(reader_6,RogueTypePosixProcessReader)))->fd_reader) );
          keep_going_4 = ((RogueLogical)(true));
        }
      }
    }
    return (RogueLogical)(keep_going_4);
  }
  else if (ROGUE_COND(((((status_3) == (-1))) && (((RogueLogical)(errno == EINTR))))))
  {
    return (RogueLogical)(true);
  }
#endif // ROGUE_PLATFORM_WINDOWS

  return (RogueLogical)(false);
}

RogueClassPosixProcess* RoguePosixProcess__init__String_List_Logical_Logical_Logical_Logical_Logical( RogueClassPosixProcess* THIS, RogueString_List* _auto_826_0, RogueLogical readable_1, RogueLogical writable_2, RogueLogical _auto_825_3, RogueLogical inherit_environment_4, RogueLogical env_5 )
{
  ROGUE_GC_CHECK;
  THIS->args = _auto_826_0;
  THIS->is_blocking = _auto_825_3;
#ifndef ROGUE_PLATFORM_WINDOWS

  if (ROGUE_COND(env_5))
  {
    inherit_environment_4 = ((RogueLogical)(env_5));
  }
  if (ROGUE_COND(!(((RoguePosixProcess__launch__Logical_Logical_Logical( ROGUE_ARG(THIS), readable_1, writable_2, inherit_environment_4 ))))))
  {
    if (THIS->cout_pipe[0] != -1) close( THIS->cout_pipe[0] );
    if (THIS->cout_pipe[1] != -1) close( THIS->cout_pipe[1] );
    if (THIS->cerr_pipe[0] != -1) close( THIS->cerr_pipe[0] );
    if (THIS->cerr_pipe[1] != -1) close( THIS->cerr_pipe[1] );
    if (THIS->cin_pipe[0] != -1)  close( THIS->cin_pipe[0] );
    if (THIS->cin_pipe[1] != -1)  close( THIS->cin_pipe[1] );
    THIS->cin_pipe[0]  = THIS->cin_pipe[1]  = -1;
    THIS->cout_pipe[0] = THIS->cout_pipe[1] = -1;
    THIS->cerr_pipe[0] = THIS->cerr_pipe[1] = -1;

    if (ROGUE_COND(readable_1))
    {
      THIS->output_reader = ((RogueClassReader_Byte_*)(((RogueClassPosixProcessReader*)(RoguePosixProcessReader__init__Process_Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassPosixProcessReader*,ROGUE_CREATE_OBJECT(PosixProcessReader))), ROGUE_ARG(((RogueClassProcess*)(THIS))), -1 )))));
      THIS->error_reader = ((RogueClassReader_Byte_*)(((RogueClassPosixProcessReader*)(RoguePosixProcessReader__init__Process_Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassPosixProcessReader*,ROGUE_CREATE_OBJECT(PosixProcessReader))), ROGUE_ARG(((RogueClassProcess*)(THIS))), -1 )))));
    }
    if (ROGUE_COND(writable_2))
    {
      THIS->input_writer = ((RogueClassWriter_Byte_*)(((RogueClassFDWriter*)(RogueFDWriter__init__Int32_Logical( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassFDWriter*,ROGUE_CREATE_OBJECT(FDWriter))), -1, true )))));
    }
    THIS->error = true;
    THIS->is_finished = true;
    THIS->exit_code = -1;
  }
#endif

  return (RogueClassPosixProcess*)(THIS);
}

RogueClassPosixProcessReader* RoguePosixProcessReader__init_object( RogueClassPosixProcessReader* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  return (RogueClassPosixProcessReader*)(THIS);
}

RogueString* RoguePosixProcessReader__to_String( RogueClassPosixProcessReader* THIS )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueByte_List*,buffer_0,(((RogueByte_List*)(RogueByte_List__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueByte_List*,ROGUE_CREATE_OBJECT(Byte_List))) )))));
  while (ROGUE_COND(((RoguePosixProcessReader__has_another( ROGUE_ARG(THIS) )))))
  {
    ROGUE_GC_CHECK;
    RogueByte_List__add__Byte( buffer_0, ROGUE_ARG(((RoguePosixProcessReader__read( ROGUE_ARG(THIS) )))) );
  }
  if (ROGUE_COND(true))
  {
    return (RogueString*)((RogueString__create__Byte_List_StringEncoding( buffer_0, RogueClassStringEncoding( 3 ) )));
  }
}

RogueString* RoguePosixProcessReader__type_name( RogueClassPosixProcessReader* THIS )
{
  return (RogueString*)(Rogue_literal_strings[356]);
}

void RoguePosixProcessReader__close( RogueClassPosixProcessReader* THIS )
{
  ROGUE_GC_CHECK;
  THIS->fd_reader->fd = -1;
  THIS->position = 0;
}

RogueLogical RoguePosixProcessReader__has_another( RogueClassPosixProcessReader* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((THIS->fd_reader->buffer_position) >= (THIS->fd_reader->buffer->count))))
  {
    Rogue_call_ROGUEM12( 25, ROGUE_ARG(THIS->process), ROGUE_ARG(THIS->process->is_blocking) );
  }
  return (RogueLogical)(((THIS->fd_reader->buffer_position) < (THIS->fd_reader->buffer->count)));
}

RogueByte RoguePosixProcessReader__read( RogueClassPosixProcessReader* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(((RoguePosixProcessReader__has_another( ROGUE_ARG(THIS) ))))))
  {
    return (RogueByte)(0);
  }
  THIS->position = ((THIS->position) + (1));
  return (RogueByte)(((RogueFDReader__read( ROGUE_ARG(THIS->fd_reader) ))));
}

RogueClassPosixProcessReader* RoguePosixProcessReader__init__Process_Int32( RogueClassPosixProcessReader* THIS, RogueClassProcess* _auto_829_0, RogueInt32 fd_1 )
{
  ROGUE_GC_CHECK;
  THIS->process = _auto_829_0;
  THIS->fd_reader = ((RogueClassFDReader*)(RogueFDReader__init__Int32_Logical( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassFDReader*,ROGUE_CREATE_OBJECT(FDReader))), fd_1, false )));
  return (RogueClassPosixProcessReader*)(THIS);
}

RogueClassFDReader* RogueFDReader__init_object( RogueClassFDReader* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  THIS->buffer = ((RogueByte_List*)(RogueByte_List__init__Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueByte_List*,ROGUE_CREATE_OBJECT(Byte_List))), 1024 )));
  return (RogueClassFDReader*)(THIS);
}

RogueString* RogueFDReader__to_String( RogueClassFDReader* THIS )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueByte_List*,buffer_0,(((RogueByte_List*)(RogueByte_List__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueByte_List*,ROGUE_CREATE_OBJECT(Byte_List))) )))));
  while (ROGUE_COND(((RogueFDReader__has_another( ROGUE_ARG(THIS) )))))
  {
    ROGUE_GC_CHECK;
    RogueByte_List__add__Byte( buffer_0, ROGUE_ARG(((RogueFDReader__read( ROGUE_ARG(THIS) )))) );
  }
  if (ROGUE_COND(true))
  {
    return (RogueString*)((RogueString__create__Byte_List_StringEncoding( buffer_0, RogueClassStringEncoding( 3 ) )));
  }
}

RogueString* RogueFDReader__type_name( RogueClassFDReader* THIS )
{
  return (RogueString*)(Rogue_literal_strings[357]);
}

void RogueFDReader__close( RogueClassFDReader* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((THIS->fd) >= (0))))
  {
    close( THIS->fd );

    THIS->fd = -1;
  }
}

RogueLogical RogueFDReader__has_another( RogueClassFDReader* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(((((THIS->fd) >= (0))) || (((THIS->buffer_position) < (THIS->buffer->count)))));
}

RogueByte RogueFDReader__peek( RogueClassFDReader* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(((RogueFDReader__has_another( ROGUE_ARG(THIS) ))))))
  {
    return (RogueByte)(0);
  }
  if (ROGUE_COND(((THIS->buffer_position) == (THIS->buffer->count))))
  {
    RogueByte_List__clear( ROGUE_ARG(THIS->buffer) );
    THIS->buffer_position = 0;
    if (ROGUE_COND(!(((RogueFDReader__buffer_more( ROGUE_ARG(THIS) ))))))
    {
      return (RogueByte)(0);
    }
  }
  return (RogueByte)(THIS->buffer->data->as_bytes[THIS->buffer_position]);
}

RogueByte RogueFDReader__read( RogueClassFDReader* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(((RogueFDReader__has_another( ROGUE_ARG(THIS) ))))))
  {
    return (RogueByte)(0);
  }
  RogueByte result_0 = (((RogueFDReader__peek( ROGUE_ARG(THIS) ))));
  THIS->position = ((THIS->position) + (1));
  ++THIS->buffer_position;
  return (RogueByte)(result_0);
}

RogueClassFDReader* RogueFDReader__init__Int32_Logical( RogueClassFDReader* THIS, RogueInt32 _auto_831_0, RogueLogical _auto_830_1 )
{
  ROGUE_GC_CHECK;
  THIS->fd = _auto_831_0;
  THIS->auto_close = _auto_830_1;
  return (RogueClassFDReader*)(THIS);
}

void RogueFDReader__on_cleanup( RogueClassFDReader* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(THIS->auto_close))
  {
    RogueFDReader__close( ROGUE_ARG(THIS) );
  }
}

RogueLogical RogueFDReader__buffer_more( RogueClassFDReader* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((THIS->fd) < (0))))
  {
    return (RogueLogical)(false);
  }
  RogueByte_List__reserve__Int32( ROGUE_ARG(THIS->buffer), 1024 );
  RogueInt32 cur_count_0 = (THIS->buffer->count);
  RogueInt32 n_1 = 0;
  n_1 = (RogueInt32) read( THIS->fd, THIS->buffer->data->as_bytes+cur_count_0, 1024 );

  if (ROGUE_COND(((n_1) <= (0))))
  {
    if (ROGUE_COND(THIS->auto_close))
    {
      RogueFDReader__close( ROGUE_ARG(THIS) );
    }
    THIS->fd = -1;
    return (RogueLogical)(false);
  }
  THIS->buffer->count += n_1;
  return (RogueLogical)(true);
}

RogueClassFDWriter* RogueFDWriter__init_object( RogueClassFDWriter* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  THIS->buffer = ((RogueByte_List*)(RogueByte_List__init__Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueByte_List*,ROGUE_CREATE_OBJECT(Byte_List))), 1024 )));
  return (RogueClassFDWriter*)(THIS);
}

RogueString* RogueFDWriter__type_name( RogueClassFDWriter* THIS )
{
  return (RogueString*)(Rogue_literal_strings[358]);
}

void RogueFDWriter__close( RogueClassFDWriter* THIS )
{
  ROGUE_GC_CHECK;
  RogueFDWriter__flush( ROGUE_ARG(THIS) );
  if (ROGUE_COND(((THIS->fd) != (-1))))
  {
    close( THIS->fd );

    THIS->fd = -1;
  }
}

void RogueFDWriter__flush( RogueClassFDWriter* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((((THIS->buffer->count) == (0))) || (((THIS->fd) == (-1))))))
  {
    return;
  }
  if (-1 == write( THIS->fd, THIS->buffer->data->as_bytes, THIS->buffer->count ))
  {
    if (THIS->auto_close) close( THIS->fd );
    THIS->fd = -1;
  }

  RogueByte_List__clear( ROGUE_ARG(THIS->buffer) );
}

RogueClassFDWriter* RogueFDWriter__init__Int32_Logical( RogueClassFDWriter* THIS, RogueInt32 _auto_834_0, RogueLogical _auto_833_1 )
{
  ROGUE_GC_CHECK;
  THIS->fd = _auto_834_0;
  THIS->auto_close = _auto_833_1;
  return (RogueClassFDWriter*)(THIS);
}

void RogueFDWriter__on_cleanup( RogueClassFDWriter* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(THIS->auto_close))
  {
    RogueFDWriter__close( ROGUE_ARG(THIS) );
  }
}

RogueClassMorlock* RogueMorlock__init_object( RogueClassMorlock* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  return (RogueClassMorlock*)(THIS);
}

RogueString* RogueMorlock__type_name( RogueClassMorlock* THIS )
{
  return (RogueString*)(Rogue_literal_strings[360]);
}

RogueClassMorlock* RogueMorlock__init__String_List( RogueClassMorlock* THIS, RogueString_List* _auto_4021 )
{
  ROGUE_DEF_LOCAL_REF(RogueString_List*,args_0,_auto_4021);
  ROGUE_GC_CHECK;
  args_0 = ((RogueString_List*)(((RogueString_List*)(RogueMorlock__parse_args__String_List( ROGUE_ARG(THIS), args_0 )))));
  RogueBootstrap__configure__Logical( ((RogueClassBootstrap*)ROGUE_SINGLETON(Bootstrap)), ROGUE_ARG(((!!(args_0->count)) && ((RogueString__operatorEQUALSEQUALS__String_String( ROGUE_ARG(((RogueString*)(RogueString_List__first( args_0 )))), Rogue_literal_strings[3] ))))) );
  if (ROGUE_COND(((((RogueString_List__is_empty( args_0 )))) || ((RogueString__operatorEQUALSEQUALS__String_String( ROGUE_ARG(((RogueString*)(RogueString_List__first( args_0 )))), Rogue_literal_strings[221] ))))))
  {
    RogueMorlock__print_usage( ROGUE_ARG(THIS) );
    RogueSystem__exit__Int32( 0 );
  }
  {
    ROGUE_DEF_LOCAL_REF(RogueString*,_auto_1093_0,(((RogueString*)(args_0->data->as_objects[0]))));
    if (ROGUE_COND((RogueString__operatorEQUALSEQUALS__String_String( _auto_1093_0, Rogue_literal_strings[4] ))))
    {
      ROGUE_DEF_LOCAL_REF(RogueClassPackageInfo*,info_1,(((RogueClassPackageInfo*)(RogueMorlock__resolve_package__String_Logical( ROGUE_ARG(THIS), ROGUE_ARG(((RogueString*)(args_0->data->as_objects[1]))), true )))));
      if (ROGUE_COND(((!!(info_1->version)) && (((RogueString_List__contains__String( ROGUE_ARG(info_1->installed_versions), ROGUE_ARG(info_1->version) )))))))
      {
        throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(RogueMorlock__error__String( ROGUE_ARG(THIS), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(info_1->name) ))) )))), Rogue_literal_strings[231] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(info_1->version) ))) )))), Rogue_literal_strings[159] )))) )))) )))) ));
      }
      RoguePackageInfo__fetch_latest_script( info_1 );
      RogueMorlock__run_script__String_PackageInfo( ROGUE_ARG(THIS), Rogue_literal_strings[4], info_1 );
    }
    else if (ROGUE_COND((RogueString__operatorEQUALSEQUALS__String_String( _auto_1093_0, Rogue_literal_strings[277] ))))
    {
      ROGUE_DEF_LOCAL_REF(RogueClassPackageInfo*,info_2,(((RogueClassPackageInfo*)(RogueMorlock__resolve_package__String_Logical( ROGUE_ARG(THIS), ROGUE_ARG(((RogueString*)(args_0->data->as_objects[1]))), false )))));
      if (ROGUE_COND(((!!(info_2->version)) && (!(((RogueString_List__contains__String( ROGUE_ARG(info_2->installed_versions), ROGUE_ARG(info_2->version) ))))))))
      {
        throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(RogueMorlock__error__String( ROGUE_ARG(THIS), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(info_2->name) ))) )))), Rogue_literal_strings[231] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(info_2->version) ))) )))), Rogue_literal_strings[278] )))) )))) )))) ));
      }
      if (ROGUE_COND(!((RogueFile__exists__String( ROGUE_ARG(info_2->folder) )))))
      {
        throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(RogueMorlock__error__String( ROGUE_ARG(THIS), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(info_2->name) ))) )))), Rogue_literal_strings[278] )))) )))) )))) ));
      }
      RogueMorlock__run_script__String_PackageInfo( ROGUE_ARG(THIS), Rogue_literal_strings[277], info_2 );
    }
    else
    {
      throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(RogueMorlock__error__String( ROGUE_ARG(THIS), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[279] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(((RogueString*)(args_0->data->as_objects[0]))) ))) )))), Rogue_literal_strings[238] )))) )))) )))) ));
    }
  }
  return (RogueClassMorlock*)(THIS);
}

RogueString* RogueMorlock__create_build_folder__PackageInfo( RogueClassMorlock* THIS, RogueClassPackageInfo* info_0 )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueString*,build_folder_1,(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(THIS->HOME) ))) )))), Rogue_literal_strings[51] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], Rogue_literal_strings[69] ))) )))), Rogue_literal_strings[51] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(info_0->provider) ))) )))), Rogue_literal_strings[51] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(info_0->app_name) ))) )))) )))));
  if (ROGUE_COND(!((RogueFile__is_folder__String( build_folder_1 )))))
  {
    RogueGlobal__println( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[247], build_folder_1 ))) )))) );
    RogueFile__delete__String( build_folder_1 );
    RogueFile__create_folder__String( build_folder_1 );
  }
  return (RogueString*)(build_folder_1);
}

void RogueMorlock__create_folder__String_Logical( RogueClassMorlock* THIS, RogueString* path_0, RogueLogical chown_1 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!((RogueFile__is_folder__String( path_0 )))))
  {
    if (ROGUE_COND(!((RogueFile__create_folder__String( path_0 )))))
    {
      ROGUE_DEF_LOCAL_REF(RogueString*,error_message_2,((RogueString__operatorPLUS__String_String( Rogue_literal_strings[54], path_0 ))));
      if (ROGUE_COND((RogueSystem__is_windows())))
      {
        throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(((RogueException*)Rogue_call_ROGUEM4( 13, ROGUE_ARG(((RogueException*)ROGUE_CREATE_REF(RogueClassError*,ROGUE_CREATE_OBJECT(Error)))), error_message_2 ))))) ));
      }
      ROGUE_DEF_LOCAL_REF(RogueString*,cmd_3,(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[55] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG((RogueFile__esc__String( path_0 ))) ))) )))) )))));
      RogueGlobal__execute__String_Logical_Logical_Logical( cmd_3, ROGUE_ARG(!!(error_message_2)), false, false );
    }
    if (ROGUE_COND(!((RogueSystem__is_windows()))))
    {
      if (ROGUE_COND(chown_1))
      {
        if (ROGUE_COND((RogueSystem__is_macos())))
        {
          ROGUE_DEF_LOCAL_REF(RogueString*,cmd_4,(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[63] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(((RogueString*)(RogueSystemEnvironment__get__String( ROGUE_ARG((RogueSystem__environment())), Rogue_literal_strings[64] )))) ))) )))), Rogue_literal_strings[65] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG((RogueFile__esc__String( ROGUE_ARG(((RogueClassMorlock*)ROGUE_SINGLETON(Morlock))->HOME) ))) ))) )))) )))));
          if (ROGUE_COND(!(((RogueProcessResult__success( ROGUE_ARG((RogueProcess__run__String_Logical_Logical_Logical( cmd_4, false, true, false ))) ))))))
          {
            RogueGlobal__execute__String_Logical_Logical_Logical( ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[61], cmd_4 ))), ROGUE_ARG(!!((RogueString__operatorPLUS__String_String( Rogue_literal_strings[66], ROGUE_ARG(((RogueClassMorlock*)ROGUE_SINGLETON(Morlock))->HOME) )))), false, false );
          }
        }
      }
      RogueGlobal__execute__String_Logical_Logical_Logical( ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[67] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG((RogueFile__esc__String( path_0 ))) ))) )))) )))), false, false, true );
    }
  }
}

RogueClassError* RogueMorlock__error__String( RogueClassMorlock* THIS, RogueString* message_0 )
{
  ROGUE_GC_CHECK;
  return (RogueClassError*)(((RogueClassError*)(((RogueException*)Rogue_call_ROGUEM4( 13, ROGUE_ARG(((RogueException*)ROGUE_CREATE_REF(RogueClassError*,ROGUE_CREATE_OBJECT(Error)))), message_0 )))));
}

void RogueMorlock__header( RogueClassMorlock* THIS )
{
  ROGUE_GC_CHECK;
  RogueGlobal__println( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), ROGUE_ARG((RogueString__operatorTIMES__String_Int32( Rogue_literal_strings[47], ROGUE_ARG(((RogueInt32__or_smaller__Int32( ROGUE_ARG(((RogueConsole__width( ((RogueClassConsole*)ROGUE_SINGLETON(Console)) )))), 80 )))) ))) )))) );
}

void RogueMorlock__header__String( RogueClassMorlock* THIS, RogueString* message_0 )
{
  ROGUE_GC_CHECK;
  RogueMorlock__header( ROGUE_ARG(THIS) );
  RogueGlobal__println( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), message_0 )))) );
  RogueMorlock__header( ROGUE_ARG(THIS) );
}

void RogueMorlock__run_script__String_PackageInfo( RogueClassMorlock* THIS, RogueString* action_0, RogueClassPackageInfo* info_1 )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueString*,build_folder_2,0);
  if (ROGUE_COND((RogueString__operatorEQUALSEQUALS__String_String( action_0, Rogue_literal_strings[4] ))))
  {
    build_folder_2 = ((RogueString*)(((RogueString*)(RogueMorlock__create_build_folder__PackageInfo( ROGUE_ARG(THIS), info_1 )))));
  }
  ROGUE_DEF_LOCAL_REF(RogueClassValue*,script_args_3,(((RogueClassValue*)(RoguePackageInfo__package_args( info_1 )))));
  Rogue_call_ROGUEM6( 144, script_args_3, Rogue_literal_strings[118], ROGUE_ARG((RogueValue__create__String( action_0 ))) );
  ROGUE_TRY
  {
    {
      ROGUE_DEF_LOCAL_REF(RogueString*,_auto_1090_0,(((RogueString*)(RogueString__to_escaped_ascii__String( ROGUE_ARG(((RogueString*)(RogueValue__to_json__Logical_Logical( script_args_3, false, false )))), ROGUE_ARG(((RogueString*)(RogueCharacter__to_String( (RogueCharacter)'"' )))) )))));
      ROGUE_DEF_LOCAL_REF(RogueString*,script_args_0,(_auto_1090_0));
      ROGUE_DEF_LOCAL_REF(RogueString*,launcher_filepath_4,(Rogue_literal_strings[259]));
      ROGUE_DEF_LOCAL_REF(RogueString*,package_filepath_5,(Rogue_literal_strings[260]));
      ROGUE_DEF_LOCAL_REF(RogueString*,exe_filepath_6,((RogueString__operatorSLASH__String_String( ROGUE_ARG(info_1->folder), ROGUE_ARG(info_1->app_name) ))));
      ROGUE_DEF_LOCAL_REF(RogueString*,crc32_filepath_7,((RogueString__operatorSLASH__String_String( ROGUE_ARG(info_1->folder), Rogue_literal_strings[261] ))));
      RogueInt32 crc32_8 = 0;
      {
        {
          {
            crc32_8 = ((RogueInt32)((((((RogueFile__crc32__String( ROGUE_ARG(info_1->filepath) ))) ^ ((RogueFile__crc32__String( package_filepath_5 ))))) ^ ((RogueFile__crc32__String( launcher_filepath_4 ))))));
            if ( !((RogueFile__exists__String( exe_filepath_6 ))) ) goto _auto_1092;
            if ( !((RogueFile__exists__String( crc32_filepath_7 ))) ) goto _auto_1092;
            if ( !((RogueString__operatorEQUALSEQUALS__String_String( ROGUE_ARG(((RogueString*)(RogueInt32__to_String( crc32_8 )))), ROGUE_ARG(((RogueString*)(RogueString__trimmed( ROGUE_ARG((RogueString__create__File_StringEncoding( ROGUE_ARG(((RogueClassFile*)(RogueFile__init__String( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassFile*,ROGUE_CREATE_OBJECT(File))), crc32_filepath_7 )))), RogueClassStringEncoding( 3 ) ))) )))) ))) ) goto _auto_1092;
            }
          goto _auto_1091;
        }
        _auto_1092:;
        {
          build_folder_2 = ((RogueString*)(((RogueString*)(RogueMorlock__create_build_folder__PackageInfo( ROGUE_ARG(THIS), info_1 )))));
          ROGUE_DEF_LOCAL_REF(RogueString*,cmd_9,(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[273] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(info_1->filepath) ))) )))), Rogue_literal_strings[42] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], launcher_filepath_4 ))) )))), Rogue_literal_strings[42] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], package_filepath_5 ))) )))), Rogue_literal_strings[274] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG((RogueFile__esc__String( build_folder_2 ))) ))) )))), Rogue_literal_strings[51] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(info_1->app_name) ))) )))) )))));
          RogueGlobal__execute__String_Logical_Logical_Logical( cmd_9, false, false, false );
          RogueFile__delete__String( exe_filepath_6 );
          RogueFile__copy__String_String_Logical_Logical_Logical_Logical( ROGUE_ARG((RogueString__operatorSLASH__String_String( build_folder_2, ROGUE_ARG(info_1->app_name) ))), exe_filepath_6, false, false, false, true );
          if (ROGUE_COND(!((RogueSystem__is_windows()))))
          {
            RogueGlobal__execute__String_Logical_Logical_Logical( ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[206] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG((RogueFile__esc__String( exe_filepath_6 ))) ))) )))) )))), false, false, true );
          }
          RogueFile__save__String_String_Logical( crc32_filepath_7, ROGUE_ARG(((RogueString*)(RogueInt32__to_String( crc32_8 )))), false );
          }
      }
      _auto_1091:;
      if (ROGUE_COND((RogueString__operatorEQUALSEQUALS__String_String( action_0, Rogue_literal_strings[4] ))))
      {
        RogueGlobal__execute__String_Logical_Logical_Logical( ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[177] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG((RogueFile__esc__String( build_folder_2 ))) ))) )))), Rogue_literal_strings[275] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG((RogueFile__esc__String( exe_filepath_6 ))) ))) )))), Rogue_literal_strings[276] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], script_args_0 ))) )))), Rogue_literal_strings[38] )))) )))), false, false, true );
      }
      else
      {
        RogueGlobal__execute__String_Logical_Logical_Logical( ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[177] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG((RogueFile__esc__String( ROGUE_ARG(info_1->folder) ))) ))) )))), Rogue_literal_strings[275] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG((RogueFile__esc__String( exe_filepath_6 ))) ))) )))), Rogue_literal_strings[276] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], script_args_0 ))) )))), Rogue_literal_strings[38] )))) )))), false, false, true );
      }
    }
  }
  ROGUE_CATCH( RogueClassError,err_10 )
  {
    if (ROGUE_COND(!!(build_folder_2)))
    {
      RogueFile__delete__String( build_folder_2 );
    }
    RogueSystem__exit__Int32( 1 );
  }
  ROGUE_END_TRY
}

RogueClassPackageInfo* RogueMorlock__resolve_package__String_Logical( RogueClassMorlock* THIS, RogueString* _auto_4022, RogueLogical allow_local_script_1 )
{
  ROGUE_DEF_LOCAL_REF(RogueString*,name_0,_auto_4022);
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueClassPackageInfo*,info_2,0);
  ROGUE_DEF_LOCAL_REF(RogueString*,version_3,0);
  if (ROGUE_COND(((RogueString__contains__Character_Logical( name_0, (RogueCharacter)'@', false )))))
  {
    version_3 = ((RogueString*)(((RogueString*)(RogueString__after_last__Character_Logical( name_0, (RogueCharacter)'@', false )))));
    name_0 = ((RogueString*)(((RogueString*)(RogueString__before_last__Character_Logical( name_0, (RogueCharacter)'@', false )))));
  }
  if (ROGUE_COND(((RogueString__contains__String_Logical( name_0, Rogue_literal_strings[92], false )))))
  {
    info_2 = ((RogueClassPackageInfo*)(((RogueClassPackageInfo*)(RoguePackageInfo__init__String_String( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassPackageInfo*,ROGUE_CREATE_OBJECT(PackageInfo))), ROGUE_ARG(THIS->HOME), name_0 )))));
  }
  else if (ROGUE_COND((((RogueFile__exists__String( name_0 ))) && (!((RogueFile__is_folder__String( name_0 )))))))
  {
    ROGUE_DEF_LOCAL_REF(RogueString*,filepath_4,(name_0));
    if (ROGUE_COND(!(allow_local_script_1)))
    {
      throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(RogueMorlock__error__String( ROGUE_ARG(THIS), Rogue_literal_strings[223] )))) ));
    }
    ROGUE_DEF_LOCAL_REF(RogueString*,provider_5,0);
    ROGUE_DEF_LOCAL_REF(RogueString*,app_name_6,0);
    {
      ROGUE_DEF_LOCAL_REF(RogueClassLineReader*,_auto_1074_0,(((RogueClassLineReader*)(RogueLineReader__init__File_StringEncoding( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassLineReader*,ROGUE_CREATE_OBJECT(LineReader))), ROGUE_ARG(((RogueClassFile*)(RogueFile__init__String( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassFile*,ROGUE_CREATE_OBJECT(File))), filepath_4 )))), RogueClassStringEncoding( 3 ) )))));
      while (ROGUE_COND((Rogue_call_ROGUEM5( 14, _auto_1074_0 ))))
      {
        ROGUE_GC_CHECK;
        ROGUE_DEF_LOCAL_REF(RogueString*,line_0,(((RogueString*)(RogueLineReader__read( _auto_1074_0 )))));
        if (ROGUE_COND(((((RogueString__contains__String_Logical( line_0, Rogue_literal_strings[224], false )))) && (((RogueString__contains_pattern__String_Logical( line_0, Rogue_literal_strings[225], false )))))))
        {
          ROGUE_DEF_LOCAL_REF(RogueString*,package_name_7,(((RogueString*)(RogueString__extract_string__String_Logical( line_0, Rogue_literal_strings[226], false )))));
          if (ROGUE_COND(!(!!(package_name_7))))
          {
            goto _auto_1075;
          }
          if (ROGUE_COND(((RogueString__contains__Character_Logical( package_name_7, (RogueCharacter)'/', false )))))
          {
            provider_5 = ((RogueString*)((RogueFile__filename__String( ROGUE_ARG((RogueFile__folder__String( package_name_7 ))) ))));
            app_name_6 = ((RogueString*)((RogueFile__filename__String( package_name_7 ))));
          }
          else
          {
            provider_5 = ((RogueString*)(package_name_7));
            app_name_6 = ((RogueString*)(package_name_7));
          }
          goto _auto_1075;
        }
      }
    }
    _auto_1075:;
    if (ROGUE_COND(!(!!(app_name_6))))
    {
      throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(RogueMorlock__error__String( ROGUE_ARG(THIS), Rogue_literal_strings[227] )))) ));
    }
    info_2 = ((RogueClassPackageInfo*)(((RogueClassPackageInfo*)(RoguePackageInfo__init__String_String( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassPackageInfo*,ROGUE_CREATE_OBJECT(PackageInfo))), ROGUE_ARG(THIS->HOME), ROGUE_ARG((RogueString__operatorSLASH__String_String( provider_5, app_name_6 ))) )))));
    info_2->using_local_script = true;
    RogueFile__create_folder__String( ROGUE_ARG(info_2->folder) );
    RogueFile__copy__String_String_Logical_Logical_Logical_Logical( filepath_4, ROGUE_ARG(info_2->filepath), false, true, false, true );
  }
  else if (ROGUE_COND(((RogueString__contains__Character_Logical( name_0, (RogueCharacter)'/', false )))))
  {
    info_2 = ((RogueClassPackageInfo*)(((RogueClassPackageInfo*)(RoguePackageInfo__init__String_String( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassPackageInfo*,ROGUE_CREATE_OBJECT(PackageInfo))), ROGUE_ARG(THIS->HOME), name_0 )))));
  }
  else
  {
    ROGUE_DEF_LOCAL_REF(RogueString_List*,listing_8,((RogueFile__listing__String_OptionalFilePattern_Logical_Logical_Logical_Logical_Logical_Logical( ROGUE_ARG((RogueString__operatorSLASH__String_String( ROGUE_ARG(THIS->HOME), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[228] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], name_0 ))) )))) )))) ))), (RogueOptionalFilePattern__create()), false, false, false, false, true, false ))));
    switch (listing_8->count)
    {
      case 0:
      {
        info_2 = ((RogueClassPackageInfo*)(((RogueClassPackageInfo*)(RoguePackageInfo__init__String_String( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassPackageInfo*,ROGUE_CREATE_OBJECT(PackageInfo))), ROGUE_ARG(THIS->HOME), name_0 )))));
        break;
      }
      case 1:
      {
        ROGUE_DEF_LOCAL_REF(RogueString*,provider_9,((RogueFile__filename__String( ROGUE_ARG((RogueFile__folder__String( ROGUE_ARG(((RogueString*)(RogueString_List__first( listing_8 )))) ))) ))));
        info_2 = ((RogueClassPackageInfo*)(((RogueClassPackageInfo*)(RoguePackageInfo__init__String_String( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassPackageInfo*,ROGUE_CREATE_OBJECT(PackageInfo))), ROGUE_ARG(THIS->HOME), ROGUE_ARG((RogueString__operatorSLASH__String_String( provider_9, name_0 ))) )))));
        break;
      }
      default:
      {
        {
          ROGUE_DEF_LOCAL_REF(RogueException*,_auto_1078_0,0);
          RogueLogical _auto_1077_0 = 0;
          ROGUE_DEF_LOCAL_REF(RogueClassStringBuilderPool*,_auto_1076_0,(RogueStringBuilder_pool));
          ROGUE_DEF_LOCAL_REF(RogueClassPackageInfo*,_auto_1079_0,0);
          ROGUE_DEF_LOCAL_REF(RogueStringBuilder*,builder_0,(((RogueStringBuilder*)(RogueStringBuilderPool__on_use( _auto_1076_0 )))));
          Rogue_ignore_unused(builder_0);

          ROGUE_TRY
          {
            RogueStringBuilder__println__String( builder_0, ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[229] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], name_0 ))) )))), Rogue_literal_strings[230] )))) )))) );
            {
              ROGUE_DEF_LOCAL_REF(RogueString_List*,_auto_1081_0,(listing_8));
              RogueInt32 _auto_1082_0 = (0);
              RogueInt32 _auto_1083_0 = (((_auto_1081_0->count) - (1)));
              for (;ROGUE_COND(((_auto_1082_0) <= (_auto_1083_0)));++_auto_1082_0)
              {
                ROGUE_GC_CHECK;
                ROGUE_DEF_LOCAL_REF(RogueString*,_auto_838_0,(((RogueString*)(_auto_1081_0->data->as_objects[_auto_1082_0]))));
                RogueStringBuilder__println__String( builder_0, ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[76] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG((RogueFile__filename__String( ROGUE_ARG((RogueFile__folder__String( _auto_838_0 ))) ))) ))) )))), Rogue_literal_strings[51] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], name_0 ))) )))) )))) );
              }
            }
            throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(RogueMorlock__error__String( ROGUE_ARG(THIS), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( builder_0 )))) )))) ));
          }
          ROGUE_CATCH( RogueException,_auto_1080_0 )
          {
            _auto_1078_0 = ((RogueException*)(_auto_1080_0));
          }
          ROGUE_END_TRY
          RogueStringBuilderPool__on_end_use__StringBuilder( _auto_1076_0, builder_0 );
          if (ROGUE_COND(!!(_auto_1078_0)))
          {
            throw ((RogueException*)Rogue_call_ROGUEM0( 16, _auto_1078_0 ));
          }
          if (ROGUE_COND(_auto_1077_0))
          {
            return (RogueClassPackageInfo*)(_auto_1079_0);
          }
        }
      }
    }
  }
  info_2->version = version_3;
  return (RogueClassPackageInfo*)(info_2);
}

RogueString_List* RogueMorlock__parse_args__String_List( RogueClassMorlock* THIS, RogueString_List* _auto_4023 )
{
  ROGUE_DEF_LOCAL_REF(RogueString_List*,args_0,_auto_4023);
  ROGUE_GC_CHECK;
  if (ROGUE_COND((RogueSystem__is_windows())))
  {
    THIS->HOME = Rogue_literal_strings[1];
  }
  else
  {
    THIS->HOME = Rogue_literal_strings[2];
  }
  args_0 = ((RogueString_List*)(((RogueString_List*)(RogueString_List__cloned( args_0 )))));
  if (ROGUE_COND(!!(args_0->count)))
  {
    {
      ROGUE_DEF_LOCAL_REF(RogueString*,_auto_842_0,(((RogueString*)(RogueString_List__first( args_0 )))));
      if (ROGUE_COND((RogueString__operatorEQUALSEQUALS__String_String( _auto_842_0, Rogue_literal_strings[3] ))))
      {
        if (ROGUE_COND(((args_0->count) >= (2))))
        {
          THIS->HOME = ((RogueString*)(args_0->data->as_objects[1]));
        }
      }
      else if (ROGUE_COND((((RogueString__operatorEQUALSEQUALS__String_String( _auto_842_0, Rogue_literal_strings[4] ))) || ((RogueString__operatorEQUALSEQUALS__String_String( _auto_842_0, Rogue_literal_strings[5] ))))))
      {
        if (ROGUE_COND(((args_0->count) != (2))))
        {
          throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(RogueMorlock__error__String( ROGUE_ARG(THIS), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[19] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(((RogueString*)(RogueString_List__first( args_0 )))) ))) )))), Rogue_literal_strings[20] )))) )))) )))) ));
        }
      }
    }
  }
  THIS->HOME = (RogueFile__expand_path__String( ROGUE_ARG(THIS->HOME) ));
  return (RogueString_List*)(args_0);
}

void RogueMorlock__print_usage( RogueClassMorlock* THIS )
{
  ROGUE_GC_CHECK;
  RogueGlobal__println( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), Rogue_literal_strings[222] )))) );
}

RogueClassBootstrap* RogueBootstrap__init_object( RogueClassBootstrap* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  THIS->printed_installing_header = false;
  THIS->bootstrapping = false;
  return (RogueClassBootstrap*)(THIS);
}

RogueString* RogueBootstrap__type_name( RogueClassBootstrap* THIS )
{
  return (RogueString*)(Rogue_literal_strings[361]);
}

void RogueBootstrap__configure__Logical( RogueClassBootstrap* THIS, RogueLogical _auto_843_0 )
{
  ROGUE_GC_CHECK;
  THIS->bootstrapping = _auto_843_0;
  if (ROGUE_COND((RogueSystem__is_windows())))
  {
    if (ROGUE_COND((((RogueProcess__run__String_Logical_Logical_Logical( Rogue_literal_strings[45], false, false, false ))->exit_code) != (0))))
    {
      throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(((RogueException*)Rogue_call_ROGUEM4( 13, ROGUE_ARG(((RogueException*)ROGUE_CREATE_REF(RogueClassError*,ROGUE_CREATE_OBJECT(Error)))), Rogue_literal_strings[46] ))))) ));
    }
  }
  if (ROGUE_COND(!((RogueFile__is_folder__String( ROGUE_ARG(((RogueClassMorlock*)ROGUE_SINGLETON(Morlock))->HOME) )))))
  {
    RogueBootstrap__print_installing_header( ROGUE_ARG(THIS) );
    RogueGlobal__println( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), Rogue_literal_strings[49] )))) );
    RogueMorlock__create_folder__String_Logical( ((RogueClassMorlock*)ROGUE_SINGLETON(Morlock)), ROGUE_ARG(((RogueClassMorlock*)ROGUE_SINGLETON(Morlock))->HOME), true );
  }
  RogueMorlock__create_folder__String_Logical( ((RogueClassMorlock*)ROGUE_SINGLETON(Morlock)), ROGUE_ARG((RogueString__operatorSLASH__String_String( ROGUE_ARG(((RogueClassMorlock*)ROGUE_SINGLETON(Morlock))->HOME), Rogue_literal_strings[68] ))), false );
  RogueMorlock__create_folder__String_Logical( ((RogueClassMorlock*)ROGUE_SINGLETON(Morlock)), ROGUE_ARG((RogueString__operatorSLASH__String_String( ROGUE_ARG(((RogueClassMorlock*)ROGUE_SINGLETON(Morlock))->HOME), Rogue_literal_strings[69] ))), false );
  RogueMorlock__create_folder__String_Logical( ((RogueClassMorlock*)ROGUE_SINGLETON(Morlock)), ROGUE_ARG((RogueString__operatorSLASH__String_String( ROGUE_ARG(((RogueClassMorlock*)ROGUE_SINGLETON(Morlock))->HOME), Rogue_literal_strings[70] ))), false );
  ROGUE_DEF_LOCAL_REF(RogueString*,binpath_1,((RogueFile__conventional_filepath__String( ROGUE_ARG((RogueString__operatorSLASH__String_String( ROGUE_ARG(((RogueClassMorlock*)ROGUE_SINGLETON(Morlock))->HOME), Rogue_literal_strings[68] ))) ))));
  {
    {
      {
        RogueOptionalString path_name_2 = (((RogueString_List__find___Function_String_RETURNSLogical_( ROGUE_ARG(((RogueString_List*)(RogueSystemEnvironment__names( ROGUE_ARG((RogueSystem__env())) )))), ROGUE_ARG(((RogueClass_Function_String_RETURNSLogical_*)(((RogueClassFunction_863*)ROGUE_SINGLETON(Function_863))))) ))));
        if ( !(path_name_2.exists) ) goto _auto_867;
        ROGUE_DEF_LOCAL_REF(RogueString*,paths_3,(((RogueString*)(RogueSystemEnvironment__get__String( ROGUE_ARG((RogueSystem__env())), ROGUE_ARG(path_name_2.value) )))));
        if ( !(paths_3) ) goto _auto_867;
        RogueCharacter separator_4 = ((((((RogueSystem__is_windows())))) ? ((RogueCharacter)';') : (RogueCharacter)':'));
        if ( !(((RogueString_List__find___Function_String_RETURNSLogical_( ROGUE_ARG(((RogueString_List*)(RogueString__split__Character_Logical( paths_3, separator_4, false )))), ROGUE_ARG(((RogueClass_Function_String_RETURNSLogical_*)(((RogueClassFunction_868*)(RogueFunction_868__init__String( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassFunction_868*,ROGUE_CREATE_OBJECT(Function_868))), binpath_1 )))))) ))).exists) ) goto _auto_867;
        }
      goto _auto_857;
    }
    _auto_867:;
    {
      RogueBootstrap__print_installing_header( ROGUE_ARG(THIS) );
      RogueMorlock__header( ((RogueClassMorlock*)ROGUE_SINGLETON(Morlock)) );
      if (ROGUE_COND((RogueSystem__is_windows())))
      {
        RogueGlobal__println( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), Rogue_literal_strings[71] )))) );
        RogueGlobal__println( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), binpath_1 )))) );
        RogueGlobal__println( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)) );
        RogueGlobal__println( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), Rogue_literal_strings[72] )))) );
        RogueGlobal__println( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), Rogue_literal_strings[73] )))) );
        RogueGlobal__println( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), Rogue_literal_strings[74] )))) );
        RogueGlobal__println( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), Rogue_literal_strings[75] )))) );
        RogueGlobal__println( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[76], binpath_1 ))) )))) );
        RogueGlobal__println( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), Rogue_literal_strings[77] )))) );
      }
      else
      {
        RogueGlobal__println( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), Rogue_literal_strings[78] )))) );
        RogueGlobal__println( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), binpath_1 )))) );
        RogueGlobal__println( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)) );
        ROGUE_DEF_LOCAL_REF(RogueString*,shell_5,(((RogueString*)(RogueSystemEnvironment__get__String( ROGUE_ARG((RogueSystem__env())), Rogue_literal_strings[79] )))));
        {
          {
            {
              if ( !(shell_5) ) goto _auto_871;
              shell_5 = ((RogueString*)(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[80] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG((RogueFile__filename__String( shell_5 ))) ))) )))), Rogue_literal_strings[81] )))) )))));
              if ( !((RogueFile__exists__String( shell_5 ))) ) goto _auto_871;
              RogueGlobal__println( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), Rogue_literal_strings[82] )))) );
              RogueGlobal__println( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[83] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], binpath_1 ))) )))), Rogue_literal_strings[84] )))), ROGUE_ARG(((RogueString*)(RogueString__operatorPLUS__Character( ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], Rogue_literal_strings[0] ))), (RogueCharacter)'$' )))) )))), Rogue_literal_strings[85] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], shell_5 ))) )))), Rogue_literal_strings[86] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], shell_5 ))) )))) )))) )))) );
              }
            goto _auto_870;
          }
          _auto_871:;
          {
            RogueGlobal__println( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), Rogue_literal_strings[87] )))) );
            RogueGlobal__println( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), ROGUE_ARG((RogueString__operatorPLUS__String_String( ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[88], binpath_1 ))), Rogue_literal_strings[89] ))) )))) );
            }
        }
        _auto_870:;
      }
      RogueMorlock__header( ((RogueClassMorlock*)ROGUE_SINGLETON(Morlock)) );
      RogueSystem__exit__Int32( 1 );
      }
  }
  _auto_857:;
  RogueBootstrap__install_rogue( ROGUE_ARG(THIS) );
  RogueBootstrap__install_rogo( ROGUE_ARG(THIS) );
  RogueBootstrap__install_morlock( ROGUE_ARG(THIS) );
}

RogueLogical RogueBootstrap__execute__String_String_Logical_Logical( RogueClassBootstrap* THIS, RogueString* cmd_0, RogueString* _auto_4024, RogueLogical suppress_error_2, RogueLogical bg_3 )
{
  ROGUE_DEF_LOCAL_REF(RogueString*,error_message_1,_auto_4024);
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(bg_3)))
  {
    RogueGlobal__println( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[58], cmd_0 ))) )))) );
  }
  if (ROGUE_COND(((0) == ((RogueSystem__run__String( cmd_0 ))))))
  {
    return (RogueLogical)(true);
  }
  if (ROGUE_COND(suppress_error_2))
  {
    return (RogueLogical)(false);
  }
  if (ROGUE_COND(!(!!(error_message_1))))
  {
    error_message_1 = ((RogueString*)((RogueString__operatorPLUS__String_String( Rogue_literal_strings[62], cmd_0 ))));
  }
  throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(((RogueException*)Rogue_call_ROGUEM4( 13, ROGUE_ARG(((RogueException*)ROGUE_CREATE_REF(RogueClassError*,ROGUE_CREATE_OBJECT(Error)))), error_message_1 ))))) ));
}

void RogueBootstrap__install_morlock( RogueClassBootstrap* THIS )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueString*,ext_0,((((((RogueSystem__is_windows())))) ? (ROGUE_ARG((RogueString*)Rogue_literal_strings[90])) : ROGUE_ARG(Rogue_literal_strings[0]))));
  ROGUE_DEF_LOCAL_REF(RogueString*,bin_filepath_1,(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(((RogueClassMorlock*)ROGUE_SINGLETON(Morlock))->HOME) ))) )))), Rogue_literal_strings[215] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ext_0 ))) )))) )))));
  if (ROGUE_COND((RogueFile__exists__String( bin_filepath_1 ))))
  {
    return;
  }
  ROGUE_DEF_LOCAL_REF(RogueClassPackage*,package_2,(((RogueClassPackage*)(RoguePackage__init__String_String( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassPackage*,ROGUE_CREATE_OBJECT(Package))), ROGUE_ARG(((RogueClassMorlock*)ROGUE_SINGLETON(Morlock))->HOME), Rogue_literal_strings[216] )))));
  RoguePackage__scan_repo_releases__String_String_Platforms( package_2, ROGUE_ARG(((RogueString*)(NULL))), ROGUE_ARG(((RogueString*)(NULL))), ROGUE_ARG(((RogueClassPlatforms*)(NULL))) );
  RoguePackage__select_version( package_2 );
  if (ROGUE_COND((((RogueFile__exists__String( ROGUE_ARG(package_2->install_folder) ))) && (!(((RogueString_List__is_empty( ROGUE_ARG((RogueFile__listing__String_OptionalFilePattern_Logical_Logical_Logical_Logical_Logical_Logical( ROGUE_ARG(package_2->install_folder), (RogueOptionalFilePattern__create()), false, false, false, false, false, false ))) ))))))))
  {
    RogueGlobal__println( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(package_2->name) ))) )))), Rogue_literal_strings[158] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(package_2->version) ))) )))), Rogue_literal_strings[159] )))) )))) )))) );
    return;
  }
  ROGUE_DEF_LOCAL_REF(RogueString*,build_folder_3,((RogueString__operatorSLASH__String_String( ROGUE_ARG(((RogueClassMorlock*)ROGUE_SINGLETON(Morlock))->HOME), Rogue_literal_strings[217] ))));
  RogueFile__delete__String( build_folder_3 );
  RogueMorlock__create_folder__String_Logical( ((RogueClassMorlock*)ROGUE_SINGLETON(Morlock)), build_folder_3, false );
  package_2->archive_filename = (RogueString__operatorSLASH__String_String( build_folder_3, ROGUE_ARG(package_2->archive_filename) ));
  RoguePackage__download( package_2 );
  RoguePackage__unpack__String( package_2, build_folder_3 );
  ROGUE_DEF_LOCAL_REF(RogueString*,archive_folder_4,0);
  {
    ROGUE_DEF_LOCAL_REF(RogueString_List*,_auto_1058_0,((RogueFile__listing__String_OptionalFilePattern_Logical_Logical_Logical_Logical_Logical_Logical( build_folder_3, (RogueOptionalFilePattern__create()), false, false, false, false, true, false ))));
    RogueInt32 _auto_1059_0 = (0);
    RogueInt32 _auto_1060_0 = (((_auto_1058_0->count) - (1)));
    for (;ROGUE_COND(((_auto_1059_0) <= (_auto_1060_0)));++_auto_1059_0)
    {
      ROGUE_GC_CHECK;
      ROGUE_DEF_LOCAL_REF(RogueString*,folder_0,(((RogueString*)(_auto_1058_0->data->as_objects[_auto_1059_0]))));
      if (ROGUE_COND((RogueFile__exists__String( ROGUE_ARG((RogueString__operatorSLASH__String_String( folder_0, Rogue_literal_strings[218] ))) ))))
      {
        archive_folder_4 = ((RogueString*)(folder_0));
        goto _auto_1061;
      }
    }
  }
  _auto_1061:;
  if (ROGUE_COND(!(!!(archive_folder_4))))
  {
    throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(((RogueException*)Rogue_call_ROGUEM4( 13, ROGUE_ARG(((RogueException*)ROGUE_CREATE_REF(RogueClassError*,ROGUE_CREATE_OBJECT(Error)))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[175], build_folder_3 ))) ))))) ));
  }
  package_2->archive_folder = archive_folder_4;
  ROGUE_DEF_LOCAL_REF(RogueString*,install_folder_5,(package_2->install_folder));
  RogueMorlock__create_folder__String_Logical( ((RogueClassMorlock*)ROGUE_SINGLETON(Morlock)), install_folder_5, false );
  RogueMorlock__header__String( ((RogueClassMorlock*)ROGUE_SINGLETON(Morlock)), Rogue_literal_strings[219] );
  RogueBootstrap__execute__String_String_Logical_Logical( ROGUE_ARG(THIS), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[182] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG((RogueFile__esc__String( archive_folder_4 ))) ))) )))), Rogue_literal_strings[220] )))) )))), ROGUE_ARG(((RogueString*)(NULL))), false, false );
  RoguePackage__install_executable__String_String_String_String_String_Logical( package_2, ROGUE_ARG(((RogueString*)(NULL))), ROGUE_ARG(((RogueString*)(NULL))), ROGUE_ARG(((RogueString*)(NULL))), ROGUE_ARG(((RogueString*)(NULL))), ROGUE_ARG(((RogueString*)(NULL))), true );
}

void RogueBootstrap__install_rogo( RogueClassBootstrap* THIS )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueString*,ext_0,((((((RogueSystem__is_windows())))) ? (ROGUE_ARG((RogueString*)Rogue_literal_strings[90])) : ROGUE_ARG(Rogue_literal_strings[0]))));
  ROGUE_DEF_LOCAL_REF(RogueString*,bin_filepath_1,(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(((RogueClassMorlock*)ROGUE_SINGLETON(Morlock))->HOME) ))) )))), Rogue_literal_strings[210] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ext_0 ))) )))) )))));
  if (ROGUE_COND((RogueFile__exists__String( bin_filepath_1 ))))
  {
    return;
  }
  ROGUE_DEF_LOCAL_REF(RogueClassPackage*,package_2,(((RogueClassPackage*)(RoguePackage__init__String_String( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassPackage*,ROGUE_CREATE_OBJECT(Package))), ROGUE_ARG(((RogueClassMorlock*)ROGUE_SINGLETON(Morlock))->HOME), Rogue_literal_strings[211] )))));
  RoguePackage__scan_repo_releases__String_String_Platforms( package_2, ROGUE_ARG(((RogueString*)(NULL))), ROGUE_ARG(((RogueString*)(NULL))), ROGUE_ARG(((RogueClassPlatforms*)(NULL))) );
  RoguePackage__select_version( package_2 );
  if (ROGUE_COND((((RogueFile__exists__String( ROGUE_ARG(package_2->install_folder) ))) && (!(((RogueString_List__is_empty( ROGUE_ARG((RogueFile__listing__String_OptionalFilePattern_Logical_Logical_Logical_Logical_Logical_Logical( ROGUE_ARG(package_2->install_folder), (RogueOptionalFilePattern__create()), false, false, false, false, false, false ))) ))))))))
  {
    RogueGlobal__println( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(package_2->name) ))) )))), Rogue_literal_strings[158] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(package_2->version) ))) )))), Rogue_literal_strings[159] )))) )))) )))) );
    return;
  }
  ROGUE_DEF_LOCAL_REF(RogueString*,build_folder_3,((RogueString__operatorSLASH__String_String( ROGUE_ARG(((RogueClassMorlock*)ROGUE_SINGLETON(Morlock))->HOME), Rogue_literal_strings[212] ))));
  RogueFile__delete__String( build_folder_3 );
  RogueMorlock__create_folder__String_Logical( ((RogueClassMorlock*)ROGUE_SINGLETON(Morlock)), build_folder_3, false );
  package_2->archive_filename = (RogueString__operatorSLASH__String_String( build_folder_3, ROGUE_ARG(package_2->archive_filename) ));
  RoguePackage__download( package_2 );
  RoguePackage__unpack__String( package_2, build_folder_3 );
  ROGUE_DEF_LOCAL_REF(RogueString*,archive_folder_4,0);
  {
    ROGUE_DEF_LOCAL_REF(RogueString_List*,_auto_1054_0,((RogueFile__listing__String_OptionalFilePattern_Logical_Logical_Logical_Logical_Logical_Logical( build_folder_3, (RogueOptionalFilePattern__create()), false, false, false, false, true, false ))));
    RogueInt32 _auto_1055_0 = (0);
    RogueInt32 _auto_1056_0 = (((_auto_1054_0->count) - (1)));
    for (;ROGUE_COND(((_auto_1055_0) <= (_auto_1056_0)));++_auto_1055_0)
    {
      ROGUE_GC_CHECK;
      ROGUE_DEF_LOCAL_REF(RogueString*,folder_0,(((RogueString*)(_auto_1054_0->data->as_objects[_auto_1055_0]))));
      if (ROGUE_COND((RogueFile__exists__String( ROGUE_ARG((RogueString__operatorSLASH__String_String( folder_0, Rogue_literal_strings[174] ))) ))))
      {
        archive_folder_4 = ((RogueString*)(folder_0));
        goto _auto_1057;
      }
    }
  }
  _auto_1057:;
  if (ROGUE_COND(!(!!(archive_folder_4))))
  {
    throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(((RogueException*)Rogue_call_ROGUEM4( 13, ROGUE_ARG(((RogueException*)ROGUE_CREATE_REF(RogueClassError*,ROGUE_CREATE_OBJECT(Error)))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[175], build_folder_3 ))) ))))) ));
  }
  package_2->archive_folder = archive_folder_4;
  ROGUE_DEF_LOCAL_REF(RogueString*,install_folder_5,(package_2->install_folder));
  RogueMorlock__create_folder__String_Logical( ((RogueClassMorlock*)ROGUE_SINGLETON(Morlock)), install_folder_5, false );
  RogueMorlock__header__String( ((RogueClassMorlock*)ROGUE_SINGLETON(Morlock)), Rogue_literal_strings[213] );
  RogueBootstrap__execute__String_String_Logical_Logical( ROGUE_ARG(THIS), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[182] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG((RogueFile__esc__String( archive_folder_4 ))) ))) )))), Rogue_literal_strings[214] )))) )))), ROGUE_ARG(((RogueString*)(NULL))), false, false );
  RoguePackage__install_executable__String_String_String_String_String_Logical( package_2, ROGUE_ARG(((RogueString*)(NULL))), ROGUE_ARG(((RogueString*)(NULL))), ROGUE_ARG(((RogueString*)(NULL))), ROGUE_ARG(((RogueString*)(NULL))), ROGUE_ARG(((RogueString*)(NULL))), true );
}

void RogueBootstrap__install_rogue( RogueClassBootstrap* THIS )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueString*,ext_0,((((((RogueSystem__is_windows())))) ? (ROGUE_ARG((RogueString*)Rogue_literal_strings[90])) : ROGUE_ARG(Rogue_literal_strings[0]))));
  ROGUE_DEF_LOCAL_REF(RogueString*,bin_filepath_1,(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(((RogueClassMorlock*)ROGUE_SINGLETON(Morlock))->HOME) ))) )))), Rogue_literal_strings[91] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ext_0 ))) )))) )))));
  if (ROGUE_COND((RogueFile__exists__String( bin_filepath_1 ))))
  {
    return;
  }
  ROGUE_DEF_LOCAL_REF(RogueClassPackage*,package_2,(((RogueClassPackage*)(RoguePackage__init__String_String( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassPackage*,ROGUE_CREATE_OBJECT(Package))), ROGUE_ARG(((RogueClassMorlock*)ROGUE_SINGLETON(Morlock))->HOME), Rogue_literal_strings[154] )))));
  RoguePackage__scan_repo_releases__String_String_Platforms( package_2, ROGUE_ARG(((RogueString*)(NULL))), ROGUE_ARG(((RogueString*)(NULL))), ROGUE_ARG(((RogueClassPlatforms*)(NULL))) );
  RoguePackage__select_version( package_2 );
  if (ROGUE_COND((((RogueFile__exists__String( ROGUE_ARG(package_2->install_folder) ))) && (!(((RogueString_List__is_empty( ROGUE_ARG((RogueFile__listing__String_OptionalFilePattern_Logical_Logical_Logical_Logical_Logical_Logical( ROGUE_ARG(package_2->install_folder), (RogueOptionalFilePattern__create()), false, false, false, false, false, false ))) ))))))))
  {
    RogueGlobal__println( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(package_2->name) ))) )))), Rogue_literal_strings[158] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(package_2->version) ))) )))), Rogue_literal_strings[159] )))) )))) )))) );
    return;
  }
  ROGUE_DEF_LOCAL_REF(RogueString*,build_folder_3,((RogueString__operatorSLASH__String_String( ROGUE_ARG(((RogueClassMorlock*)ROGUE_SINGLETON(Morlock))->HOME), Rogue_literal_strings[160] ))));
  RogueFile__delete__String( build_folder_3 );
  RogueMorlock__create_folder__String_Logical( ((RogueClassMorlock*)ROGUE_SINGLETON(Morlock)), build_folder_3, false );
  package_2->archive_filename = (RogueString__operatorSLASH__String_String( build_folder_3, ROGUE_ARG(package_2->archive_filename) ));
  RoguePackage__download( package_2 );
  RoguePackage__unpack__String( package_2, build_folder_3 );
  ROGUE_DEF_LOCAL_REF(RogueString*,archive_folder_4,0);
  ROGUE_DEF_LOCAL_REF(RogueString*,makefile_5,((((((RogueSystem__is_windows())))) ? (ROGUE_ARG((RogueString*)Rogue_literal_strings[173])) : ROGUE_ARG(Rogue_literal_strings[174]))));
  {
    ROGUE_DEF_LOCAL_REF(RogueString_List*,_auto_1027_0,((RogueFile__listing__String_OptionalFilePattern_Logical_Logical_Logical_Logical_Logical_Logical( build_folder_3, (RogueOptionalFilePattern__create()), false, false, false, false, true, false ))));
    RogueInt32 _auto_1028_0 = (0);
    RogueInt32 _auto_1029_0 = (((_auto_1027_0->count) - (1)));
    for (;ROGUE_COND(((_auto_1028_0) <= (_auto_1029_0)));++_auto_1028_0)
    {
      ROGUE_GC_CHECK;
      ROGUE_DEF_LOCAL_REF(RogueString*,folder_0,(((RogueString*)(_auto_1027_0->data->as_objects[_auto_1028_0]))));
      if (ROGUE_COND((RogueFile__exists__String( ROGUE_ARG((RogueString__operatorSLASH__String_String( folder_0, makefile_5 ))) ))))
      {
        archive_folder_4 = ((RogueString*)(folder_0));
        goto _auto_1030;
      }
    }
  }
  _auto_1030:;
  if (ROGUE_COND(!(!!(archive_folder_4))))
  {
    throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(((RogueException*)Rogue_call_ROGUEM4( 13, ROGUE_ARG(((RogueException*)ROGUE_CREATE_REF(RogueClassError*,ROGUE_CREATE_OBJECT(Error)))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[175], build_folder_3 ))) ))))) ));
  }
  package_2->archive_folder = archive_folder_4;
  ROGUE_DEF_LOCAL_REF(RogueString*,install_folder_6,(package_2->install_folder));
  ROGUE_DEF_LOCAL_REF(RogueString*,libraries_folder_7,(install_folder_6));
  RogueMorlock__create_folder__String_Logical( ((RogueClassMorlock*)ROGUE_SINGLETON(Morlock)), libraries_folder_7, false );
  RogueMorlock__header__String( ((RogueClassMorlock*)ROGUE_SINGLETON(Morlock)), Rogue_literal_strings[176] );
  if (ROGUE_COND((RogueSystem__is_windows())))
  {
    RogueBootstrap__execute__String_String_Logical_Logical( ROGUE_ARG(THIS), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[177] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG((RogueFile__esc__String( archive_folder_4 ))) ))) )))), Rogue_literal_strings[178] )))) )))), ROGUE_ARG(((RogueString*)(NULL))), false, false );
    RogueBootstrap__execute__String_String_Logical_Logical( ROGUE_ARG(THIS), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[179] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG((RogueFile__esc__String( ROGUE_ARG((RogueString__operatorSLASH__String_String( archive_folder_4, Rogue_literal_strings[180] ))) ))) ))) )))), Rogue_literal_strings[42] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG((RogueFile__esc__String( ROGUE_ARG((RogueString__operatorSLASH__String_String( install_folder_6, Rogue_literal_strings[181] ))) ))) ))) )))) )))), ROGUE_ARG(((RogueString*)(NULL))), false, false );
  }
  else
  {
    RogueBootstrap__execute__String_String_Logical_Logical( ROGUE_ARG(THIS), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[182] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG((RogueFile__esc__String( archive_folder_4 ))) ))) )))), Rogue_literal_strings[183] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG((RogueFile__esc__String( install_folder_6 ))) ))) )))), Rogue_literal_strings[116] )))) )))), ROGUE_ARG(((RogueString*)(NULL))), false, false );
  }
  ROGUE_DEF_LOCAL_REF(RogueString*,dest_filename_8,((((((RogueSystem__is_windows())))) ? (ROGUE_ARG((RogueString*)Rogue_literal_strings[184])) : ROGUE_ARG(Rogue_literal_strings[185]))));
  RoguePackage__install_executable__String_String_String_String_String_Logical( package_2, ROGUE_ARG(((RogueString*)(NULL))), ROGUE_ARG(((RogueString*)(NULL))), ROGUE_ARG(((RogueString*)(NULL))), ROGUE_ARG(((RogueString*)(NULL))), dest_filename_8, true );
}

void RogueBootstrap__print_installing_header( RogueClassBootstrap* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(THIS->printed_installing_header))
  {
    return;
  }
  THIS->printed_installing_header = true;
  RogueMorlock__header__String( ((RogueClassMorlock*)ROGUE_SINGLETON(Morlock)), Rogue_literal_strings[48] );
}

RogueClassFunction_863* RogueFunction_863__init_object( RogueClassFunction_863* THIS )
{
  ROGUE_GC_CHECK;
  Rogue_Function_String_RETURNSLogical___init_object( ROGUE_ARG(((RogueClass_Function_String_RETURNSLogical_*)THIS)) );
  return (RogueClassFunction_863*)(THIS);
}

RogueString* RogueFunction_863__type_name( RogueClassFunction_863* THIS )
{
  return (RogueString*)(Rogue_literal_strings[426]);
}

RogueLogical RogueFunction_863__call__String( RogueClassFunction_863* THIS, RogueString* name_0 )
{
  return (RogueLogical)(((RogueString__equals__String_Logical( name_0, Rogue_literal_strings[434], true ))));
}

RogueClassFunction_868* RogueFunction_868__init_object( RogueClassFunction_868* THIS )
{
  ROGUE_GC_CHECK;
  Rogue_Function_String_RETURNSLogical___init_object( ROGUE_ARG(((RogueClass_Function_String_RETURNSLogical_*)THIS)) );
  return (RogueClassFunction_868*)(THIS);
}

RogueString* RogueFunction_868__type_name( RogueClassFunction_868* THIS )
{
  return (RogueString*)(Rogue_literal_strings[427]);
}

RogueLogical RogueFunction_868__call__String( RogueClassFunction_868* THIS, RogueString* path_0 )
{
  return (RogueLogical)(((RogueString__equals__String_Logical( path_0, ROGUE_ARG(THIS->binpath), true ))));
}

RogueClassFunction_868* RogueFunction_868__init__String( RogueClassFunction_868* THIS, RogueString* _auto_869_0 )
{
  ROGUE_GC_CHECK;
  THIS->binpath = _auto_869_0;
  return (RogueClassFunction_868*)(THIS);
}

RogueClassPackage* RoguePackage__init_object( RogueClassPackage* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  THIS->releases = ((RogueClassValueList*)(RogueValueList__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassValueList*,ROGUE_CREATE_OBJECT(ValueList))) )));
  return (RogueClassPackage*)(THIS);
}

RogueClassPackage* RoguePackage__init( RogueClassPackage* THIS )
{
  ROGUE_GC_CHECK;
  RoguePackage__scan_repo_releases__String_String_Platforms( ROGUE_ARG(THIS), ROGUE_ARG(((RogueString*)(NULL))), ROGUE_ARG(((RogueString*)(NULL))), ROGUE_ARG(((RogueClassPlatforms*)(NULL))) );
  return (RogueClassPackage*)(THIS);
}

RogueString* RoguePackage__type_name( RogueClassPackage* THIS )
{
  return (RogueString*)(Rogue_literal_strings[362]);
}

RogueClassPackage* RoguePackage__init__String_String( RogueClassPackage* THIS, RogueString* _auto_882_0, RogueString* url_1 )
{
  ROGUE_GC_CHECK;
  THIS->morlock_home = _auto_882_0;
  ROGUE_DEF_LOCAL_REF(RogueClassPackageInfo*,info_2,(((RogueClassPackageInfo*)(RoguePackageInfo__init__String_String( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassPackageInfo*,ROGUE_CREATE_OBJECT(PackageInfo))), ROGUE_ARG(THIS->morlock_home), url_1 )))));
  THIS->name = info_2->name;
  RoguePackage__init__Value( ROGUE_ARG(THIS), ROGUE_ARG(((RogueClassValue*)(RoguePackageInfo__package_args( info_2 )))) );
  return (RogueClassPackage*)(THIS);
}

RogueClassPackage* RoguePackage__init__Value( RogueClassPackage* THIS, RogueClassValue* _auto_883_0 )
{
  ROGUE_GC_CHECK;
  THIS->properties = _auto_883_0;
  if (ROGUE_COND(((!(!!(THIS->name))) || (((((RogueString__count__Character( ROGUE_ARG(THIS->name), (RogueCharacter)'/' )))) > (1))))))
  {
    throw ((RogueException*)(RoguePackageError___throw( ROGUE_ARG(((RogueClassPackageError*)(RoguePackageError__init__String_String( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassPackageError*,ROGUE_CREATE_OBJECT(PackageError))), ROGUE_ARG((RogueFile__filename__String( ROGUE_ARG(((RogueString*)Rogue_call_ROGUEM0( 8, ROGUE_ARG(((RogueObject*)((RogueClassValue*)Rogue_call_ROGUEM4( 41, ROGUE_ARG(THIS->properties), Rogue_literal_strings[112] )))) ))) ))), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[117] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(((RogueString*)(RogueString__before_last__Character_Logical( ROGUE_ARG((RogueFile__filename__String( ROGUE_ARG(((RogueString*)Rogue_call_ROGUEM0( 8, ROGUE_ARG(((RogueObject*)((RogueClassValue*)Rogue_call_ROGUEM4( 41, ROGUE_ARG(THIS->properties), Rogue_literal_strings[112] )))) ))) ))), (RogueCharacter)'.', false )))) ))) )))), Rogue_literal_strings[38] )))) )))) )))) )));
  }
  if (ROGUE_COND(!(((RogueString__contains__Character_Logical( ROGUE_ARG(THIS->name), (RogueCharacter)'/', false ))))))
  {
    THIS->name = (RogueString__operatorSLASH__String_String( ROGUE_ARG(THIS->name), ROGUE_ARG(THIS->name) ));
  }
  THIS->provider = ((RogueString*)(RogueString__before_first__Character_Logical( ROGUE_ARG(THIS->name), (RogueCharacter)'/', false )));
  THIS->app_name = ((RogueString*)(RogueString__after_first__Character_Logical( ROGUE_ARG(THIS->name), (RogueCharacter)'/', false )));
  THIS->action = ((RogueString*)Rogue_call_ROGUEM0( 8, ROGUE_ARG(((RogueObject*)((RogueClassValue*)Rogue_call_ROGUEM4( 41, ROGUE_ARG(THIS->properties), Rogue_literal_strings[118] )))) ));
  THIS->morlock_home = ((RogueString*)Rogue_call_ROGUEM0( 8, ROGUE_ARG(((RogueObject*)((RogueClassValue*)Rogue_call_ROGUEM4( 41, ROGUE_ARG(THIS->properties), Rogue_literal_strings[119] )))) ));
  THIS->host = ((RogueString*)Rogue_call_ROGUEM0( 8, ROGUE_ARG(((RogueObject*)((RogueClassValue*)Rogue_call_ROGUEM4( 41, ROGUE_ARG(THIS->properties), Rogue_literal_strings[120] )))) ));
  THIS->repo = ((RogueString*)Rogue_call_ROGUEM0( 8, ROGUE_ARG(((RogueObject*)((RogueClassValue*)Rogue_call_ROGUEM4( 41, ROGUE_ARG(THIS->properties), Rogue_literal_strings[121] )))) ));
  THIS->launcher_folder = (RogueString__operatorSLASH__String_String( ROGUE_ARG(THIS->morlock_home), Rogue_literal_strings[68] ));
  if (ROGUE_COND((RogueOptionalValue__operator__Value( ROGUE_ARG(((RogueClassValue*)Rogue_call_ROGUEM4( 41, ROGUE_ARG(THIS->properties), Rogue_literal_strings[122] ))) ))))
  {
    THIS->specified_version = ((RogueString*)Rogue_call_ROGUEM0( 8, ROGUE_ARG(((RogueObject*)((RogueClassValue*)Rogue_call_ROGUEM4( 41, ROGUE_ARG(THIS->properties), Rogue_literal_strings[122] )))) ));
    THIS->version = THIS->specified_version;
  }
  RoguePackage__init( ROGUE_ARG(THIS) );
  return (RogueClassPackage*)(THIS);
}

void RoguePackage__copy_executable__String_String( RogueClassPackage* THIS, RogueString* src_filepath_0, RogueString* _auto_4025 )
{
  ROGUE_DEF_LOCAL_REF(RogueString*,dest_filename_1,_auto_4025);
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(!!(dest_filename_1))))
  {
    dest_filename_1 = ((RogueString*)(THIS->app_name));
    if (ROGUE_COND((RogueSystem__is_windows())))
    {
      dest_filename_1 = ((RogueString*)(((RogueString*)(RogueString__operatorPLUS__String( dest_filename_1, Rogue_literal_strings[199] )))));
    }
  }
  RoguePackage__create_folder__String( ROGUE_ARG(THIS), ROGUE_ARG(THIS->bin_folder) );
  ROGUE_DEF_LOCAL_REF(RogueString*,dest_filepath_2,((RogueString__operatorSLASH__String_String( ROGUE_ARG(THIS->bin_folder), dest_filename_1 ))));
  RogueGlobal__println( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[201] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], src_filepath_0 ))) )))), Rogue_literal_strings[202] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], dest_filepath_2 ))) )))) )))) )))) );
  RogueFile__copy__String_String_Logical_Logical_Logical_Logical( src_filepath_0, dest_filepath_2, false, false, false, false );
  if (ROGUE_COND(!((RogueSystem__is_windows()))))
  {
    RoguePackage__execute__String_Logical( ROGUE_ARG(THIS), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[206] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG((RogueFile__esc__String( dest_filepath_2 ))) ))) )))) )))), true );
  }
}

void RoguePackage__create_folder__String( RogueClassPackage* THIS, RogueString* folder_0 )
{
  ROGUE_GC_CHECK;
  RogueGlobal__println( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[76], folder_0 ))) )))) );
  RogueFile__create_folder__String( folder_0 );
  if (ROGUE_COND(!((RogueFile__is_folder__String( folder_0 )))))
  {
    throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(RoguePackage__error__String( ROGUE_ARG(THIS), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[200] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], folder_0 ))) )))), Rogue_literal_strings[97] )))) )))) )))) ));
  }
}

RogueString* RoguePackage__download( RogueClassPackage* THIS )
{
  ROGUE_GC_CHECK;
  RogueGlobal__println( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[161] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(THIS->name) ))) )))), Rogue_literal_strings[158] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(THIS->version) ))) )))) )))) )))) );
  RoguePackage__execute__String_Logical( ROGUE_ARG(THIS), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[162] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(THIS->url) ))) )))), Rogue_literal_strings[163] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG((RogueFile__esc__String( ROGUE_ARG(THIS->archive_filename) ))) ))) )))) )))), true );
  if (ROGUE_COND(!((RogueFile__exists__String( ROGUE_ARG(THIS->archive_filename) )))))
  {
    throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(RoguePackage__error__String( ROGUE_ARG(THIS), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[164], ROGUE_ARG(THIS->url) ))) )))) ));
  }
  return (RogueString*)(THIS->archive_filename);
}

RogueClassError* RoguePackage__error__String( RogueClassPackage* THIS, RogueString* message_0 )
{
  ROGUE_GC_CHECK;
  return (RogueClassError*)(((RogueClassError*)(((RogueClassPackageError*)(RoguePackageError__init__String_String( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassPackageError*,ROGUE_CREATE_OBJECT(PackageError))), ROGUE_ARG((RogueString__operatorSLASH__String_String( ROGUE_ARG(THIS->provider), ROGUE_ARG(THIS->app_name) ))), message_0 ))))));
}

void RoguePackage__execute__String_Logical( RogueClassPackage* THIS, RogueString* cmd_0, RogueLogical bg_1 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(bg_1)))
  {
    RogueGlobal__println( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[58], cmd_0 ))) )))) );
  }
  if (ROGUE_COND(((0) != ((RogueSystem__run__String( cmd_0 ))))))
  {
    throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(RoguePackage__error__String( ROGUE_ARG(THIS), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[62], cmd_0 ))) )))) ));
  }
}

RogueString* RoguePackage__filename_for_url__String( RogueClassPackage* THIS, RogueString* url_0 )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueString*,filename_1,((RogueFile__filename__String( url_0 ))));
  {
    {
      {
        ROGUE_DEF_LOCAL_REF(RogueString*,ext_2,((RogueFile__extension__String( filename_1 ))));
        if (ROGUE_COND(((RogueString__equals__String_Logical( ext_2, Rogue_literal_strings[142], true ))))) goto _auto_978;
        if (ROGUE_COND(((RogueString__equals__String_Logical( ext_2, Rogue_literal_strings[143], true ))))) goto _auto_978;
        if (ROGUE_COND(((RogueString__equals__String_Logical( ext_2, Rogue_literal_strings[144], true ))))) goto _auto_978;
        if (ROGUE_COND(((RogueString__contains__String_Logical( url_0, Rogue_literal_strings[145], false )))))
        {
          filename_1 = ((RogueString*)(((RogueString*)(RogueString__operatorPLUS__String( filename_1, Rogue_literal_strings[146] )))));
        }
        else if (ROGUE_COND(((RogueString__contains__String_Logical( url_0, Rogue_literal_strings[147], false )))))
        {
          filename_1 = ((RogueString*)(((RogueString*)(RogueString__operatorPLUS__String( filename_1, Rogue_literal_strings[148] )))));
        }
        else
        {
          throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(((RogueException*)Rogue_call_ROGUEM4( 13, ROGUE_ARG(((RogueException*)ROGUE_CREATE_REF(RogueClassError*,ROGUE_CREATE_OBJECT(Error)))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[149], filename_1 ))) ))))) ));
        }
        }
      _auto_978:;
      goto _auto_977;
    }
  }
  _auto_977:;
  return (RogueString*)(filename_1);
}

void RoguePackage__install_executable__String_String_String_String_String_Logical( RogueClassPackage* THIS, RogueString* default_0, RogueString* windows_1, RogueString* macos_2, RogueString* linux_3, RogueString* dest_filename_4, RogueLogical link_5 )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueString*,pattern_6,0);
  if (ROGUE_COND((RogueSystem__is_macos())))
  {
    ROGUE_DEF_LOCAL_REF(RogueString*,_auto_873_7,(macos_2));
    pattern_6 = ((RogueString*)(((((_auto_873_7))) ? (ROGUE_ARG((RogueString*)_auto_873_7)) : ROGUE_ARG(default_0))));
  }
  else if (ROGUE_COND((RogueSystem__is_linux())))
  {
    ROGUE_DEF_LOCAL_REF(RogueString*,_auto_874_8,(linux_3));
    pattern_6 = ((RogueString*)(((((_auto_874_8))) ? (ROGUE_ARG((RogueString*)_auto_874_8)) : ROGUE_ARG(default_0))));
  }
  else if (ROGUE_COND((RogueSystem__is_windows())))
  {
    ROGUE_DEF_LOCAL_REF(RogueString*,_auto_875_9,(windows_1));
    pattern_6 = ((RogueString*)(((((_auto_875_9))) ? (ROGUE_ARG((RogueString*)_auto_875_9)) : ROGUE_ARG(default_0))));
  }
  else
  {
    pattern_6 = ((RogueString*)(default_0));
  }
  if (ROGUE_COND(!(!!(pattern_6))))
  {
    if (ROGUE_COND((RogueFile__is_folder__String( Rogue_literal_strings[186] ))))
    {
      pattern_6 = ((RogueString*)(Rogue_literal_strings[187]));
    }
    else if (ROGUE_COND((RogueFile__is_folder__String( Rogue_literal_strings[68] ))))
    {
      pattern_6 = ((RogueString*)(Rogue_literal_strings[188]));
    }
  }
  if (ROGUE_COND(!(!!(pattern_6))))
  {
    throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(RoguePackage__error__String( ROGUE_ARG(THIS), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[189] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG((RogueSystem__os())) ))) )))), Rogue_literal_strings[52] )))) )))) )))) ));
  }
  pattern_6 = ((RogueString*)(((RogueString*)(RogueString__replacing__String_String_Logical( pattern_6, Rogue_literal_strings[197], ROGUE_ARG((RogueSystem__os())), false )))));
  ROGUE_DEF_LOCAL_REF(RogueString_List*,exe_list_10,((RogueFile__listing__String_OptionalFilePattern_Logical_Logical_Logical_Logical_Logical_Logical( ROGUE_ARG((RogueString__operatorSLASH__String_String( ROGUE_ARG(THIS->archive_folder), pattern_6 ))), (RogueOptionalFilePattern__create()), false, false, false, false, false, false ))));
  {
    {
      {
        if (ROGUE_COND(((exe_list_10->count) == (1)))) goto _auto_1032;
        if (ROGUE_COND((RogueSystem__is_windows())))
        {
          RogueString_List__keep___Function_String_RETURNSLogical_( exe_list_10, ROGUE_ARG(((RogueClass_Function_String_RETURNSLogical_*)(((RogueClassFunction_1033*)ROGUE_SINGLETON(Function_1033))))) );
        }
        else if (ROGUE_COND(((RogueString_List__find___Function_String_RETURNSLogical_( exe_list_10, ROGUE_ARG(((RogueClass_Function_String_RETURNSLogical_*)(((RogueClassFunction_1039*)ROGUE_SINGLETON(Function_1039))))) ))).exists))
        {
          RogueString_List__keep___Function_String_RETURNSLogical_( exe_list_10, ROGUE_ARG(((RogueClass_Function_String_RETURNSLogical_*)(((RogueClassFunction_1040*)ROGUE_SINGLETON(Function_1040))))) );
        }
        else if (ROGUE_COND(((RogueString_List__find___Function_String_RETURNSLogical_( exe_list_10, ROGUE_ARG(((RogueClass_Function_String_RETURNSLogical_*)(((RogueClassFunction_1041*)(RogueFunction_1041__init__String( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassFunction_1041*,ROGUE_CREATE_OBJECT(Function_1041))), ROGUE_ARG(THIS->app_name) )))))) ))).exists))
        {
          RogueString_List__keep___Function_String_RETURNSLogical_( exe_list_10, ROGUE_ARG(((RogueClass_Function_String_RETURNSLogical_*)(((RogueClassFunction_1043*)(RogueFunction_1043__init__String( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassFunction_1043*,ROGUE_CREATE_OBJECT(Function_1043))), ROGUE_ARG(THIS->app_name) )))))) );
        }
        if (ROGUE_COND(((exe_list_10->count) == (1)))) goto _auto_1032;
        throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(RoguePackage__error__String( ROGUE_ARG(THIS), Rogue_literal_strings[198] )))) ));
        }
      _auto_1032:;
      goto _auto_1031;
    }
  }
  _auto_1031:;
  {
    ROGUE_DEF_LOCAL_REF(RogueString_List*,_auto_1045_0,(exe_list_10));
    RogueInt32 _auto_1046_0 = (0);
    RogueInt32 _auto_1047_0 = (((_auto_1045_0->count) - (1)));
    for (;ROGUE_COND(((_auto_1046_0) <= (_auto_1047_0)));++_auto_1046_0)
    {
      ROGUE_GC_CHECK;
      ROGUE_DEF_LOCAL_REF(RogueString*,_auto_876_0,(((RogueString*)(_auto_1045_0->data->as_objects[_auto_1046_0]))));
      RoguePackage__copy_executable__String_String( ROGUE_ARG(THIS), _auto_876_0, dest_filename_4 );
    }
  }
  if (ROGUE_COND(link_5))
  {
    RoguePackage__link( ROGUE_ARG(THIS) );
  }
}

void RoguePackage__link( RogueClassPackage* THIS )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueString_List*,exe_list_0,((RogueFile__listing__String_OptionalFilePattern_Logical_Logical_Logical_Logical_Logical_Logical( ROGUE_ARG((RogueString__operatorSLASH__String_String( ROGUE_ARG(THIS->bin_folder), Rogue_literal_strings[109] ))), (RogueOptionalFilePattern__create()), false, false, false, false, false, false ))));
  {
    ROGUE_DEF_LOCAL_REF(RogueString_List*,_auto_1051_0,(exe_list_0));
    RogueInt32 _auto_1052_0 = (0);
    RogueInt32 _auto_1053_0 = (((_auto_1051_0->count) - (1)));
    for (;ROGUE_COND(((_auto_1052_0) <= (_auto_1053_0)));++_auto_1052_0)
    {
      ROGUE_GC_CHECK;
      ROGUE_DEF_LOCAL_REF(RogueString*,exe_0,(((RogueString*)(_auto_1051_0->data->as_objects[_auto_1052_0]))));
      ROGUE_DEF_LOCAL_REF(RogueString*,launcher_1,((RogueString__operatorSLASH__String_String( ROGUE_ARG(THIS->launcher_folder), ROGUE_ARG(((RogueString*)(RogueString__before_last__String_Logical( ROGUE_ARG((RogueFile__filename__String( exe_0 ))), Rogue_literal_strings[199], true )))) ))));
      if (ROGUE_COND((RogueSystem__is_windows())))
      {
        launcher_1 = ((RogueString*)(((RogueString*)(RogueString__operatorPLUS__String( launcher_1, Rogue_literal_strings[90] )))));
      }
      RogueFile__delete__String( launcher_1 );
      if (ROGUE_COND((RogueSystem__is_windows())))
      {
        RogueFile__save__String_String_Logical( launcher_1, ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[207] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG((RogueFile__esc__String( ROGUE_ARG((RogueFile__conventional_filepath__String( exe_0 ))) ))) ))) )))), Rogue_literal_strings[208] )))) )))), false );
      }
      else
      {
        ROGUE_DEF_LOCAL_REF(RogueString*,cmd_2,(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[209] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], exe_0 ))) )))), Rogue_literal_strings[42] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], launcher_1 ))) )))) )))));
        RoguePackage__execute__String_Logical( ROGUE_ARG(THIS), cmd_2, true );
      }
    }
  }
}

void RoguePackage__release__String_Platforms_String( RogueClassPackage* THIS, RogueString* url_0, RogueClassPlatforms* platforms_1, RogueString* _auto_4026 )
{
  ROGUE_DEF_LOCAL_REF(RogueString*,version_2,_auto_4026);
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(!!(version_2))))
  {
    ROGUE_DEF_LOCAL_REF(RogueString*,lc_3,(((RogueString*)(RogueString__to_lowercase( url_0 )))));
    RogueOptionalSpan span_4 = (((RogueString__locate_pattern__String_Int32_Logical( lc_3, Rogue_literal_strings[134], 0, false ))));
    {
      {
        {
          if (ROGUE_COND(span_4.exists)) goto _auto_967;
          span_4 = ((RogueOptionalSpan)(((RogueString__locate_pattern__String_Int32_Logical( lc_3, Rogue_literal_strings[135], 0, false )))));
          if (ROGUE_COND(span_4.exists)) goto _auto_967;
          span_4 = ((RogueOptionalSpan)(((RogueString__locate_pattern__String_Int32_Logical( lc_3, Rogue_literal_strings[136], 0, false )))));
          if (ROGUE_COND(span_4.exists)) goto _auto_967;
          span_4 = ((RogueOptionalSpan)(((RogueString__locate_pattern__String_Int32_Logical( lc_3, Rogue_literal_strings[137], 0, false )))));
          if (ROGUE_COND(span_4.exists)) goto _auto_967;
          throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(RoguePackage__error__String( ROGUE_ARG(THIS), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[138] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], url_0 ))) )))), Rogue_literal_strings[97] )))) )))) )))) ));
          }
        _auto_967:;
        goto _auto_966;
      }
    }
    _auto_966:;
    version_2 = ((RogueString*)(((RogueString*)(RogueString__substring__Int32( url_0, ROGUE_ARG(span_4.value.index) )))));
    RogueInt32 separator_index_5 = (((span_4.value.index) + (span_4.value.count)));
    RogueCharacter separator_6 = (((((((separator_index_5) < (((RogueString__count( url_0 )))))))) ? (((RogueString__get__Int32( url_0, separator_index_5 )))) : (RogueCharacter)'.'));
    {
      ROGUE_DEF_LOCAL_REF(RogueException*,_auto_970_0,0);
      RogueLogical _auto_969_0 = 0;
      ROGUE_DEF_LOCAL_REF(RogueClassStringBuilderPool*,_auto_968_0,(RogueStringBuilder_pool));
      ROGUE_DEF_LOCAL_REF(RogueStringBuilder*,builder_0,(((RogueStringBuilder*)(RogueStringBuilderPool__on_use( _auto_968_0 )))));
      Rogue_ignore_unused(builder_0);

      ROGUE_TRY
      {
        RogueLogical found_numbers_7 = (false);
        {
          ROGUE_DEF_LOCAL_REF(RogueString*,_auto_972_0,(version_2));
          RogueInt32 _auto_973_0 = (0);
          RogueInt32 _auto_974_0 = (((((RogueString__count( _auto_972_0 )))) - (1)));
          for (;ROGUE_COND(((_auto_973_0) <= (_auto_974_0)));++_auto_973_0)
          {
            ROGUE_GC_CHECK;
            RogueCharacter ch_0 = (((RogueString__get__Int32( _auto_972_0, _auto_973_0 ))));
            if (ROGUE_COND(((ch_0) == (separator_6))))
            {
              RogueStringBuilder__print__String( builder_0, Rogue_literal_strings[52] );
            }
            else if (ROGUE_COND(((RogueCharacter__is_number__Int32( ch_0, 10 )))))
            {
              RogueStringBuilder__print__Character( builder_0, ch_0 );
              found_numbers_7 = ((RogueLogical)(true));
            }
            else if (ROGUE_COND(found_numbers_7))
            {
              goto _auto_975;
            }
          }
        }
        _auto_975:;
        version_2 = ((RogueString*)(((RogueString*)(RogueString__without_trailing__Character( ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( builder_0 )))), (RogueCharacter)'.' )))));
      }
      ROGUE_CATCH( RogueException,_auto_971_0 )
      {
        _auto_970_0 = ((RogueException*)(_auto_971_0));
      }
      ROGUE_END_TRY
      RogueStringBuilderPool__on_end_use__StringBuilder( _auto_968_0, builder_0 );
      if (ROGUE_COND(!!(_auto_970_0)))
      {
        throw ((RogueException*)Rogue_call_ROGUEM0( 16, _auto_970_0 ));
      }
      if (ROGUE_COND(_auto_969_0))
      {
        return;
      }
    }
  }
  ROGUE_DEF_LOCAL_REF(RogueClassValueTable*,_auto_976_0,(((RogueClassValueTable*)(RogueValueTable__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassValueTable*,ROGUE_CREATE_OBJECT(ValueTable))) )))));
  {
    Rogue_call_ROGUEM6( 144, ((RogueClassValue*)_auto_976_0), Rogue_literal_strings[122], ROGUE_ARG((RogueValue__create__String( version_2 ))) );
    Rogue_call_ROGUEM6( 144, ((RogueClassValue*)_auto_976_0), Rogue_literal_strings[139], ROGUE_ARG((RogueValue__create__String( url_0 ))) );
    Rogue_call_ROGUEM6( 144, ((RogueClassValue*)_auto_976_0), Rogue_literal_strings[140], ROGUE_ARG((RogueValue__create__String( ROGUE_ARG(((RogueString*)(RoguePlatforms__to_String( platforms_1 )))) ))) );
    Rogue_call_ROGUEM6( 144, ((RogueClassValue*)_auto_976_0), Rogue_literal_strings[141], ROGUE_ARG((RogueValue__create__String( ROGUE_ARG(((RogueString*)(RoguePackage__filename_for_url__String( ROGUE_ARG(THIS), url_0 )))) ))) );
  }
  Rogue_call_ROGUEM4( 19, ROGUE_ARG(THIS->releases), ROGUE_ARG(((RogueClassValue*)(_auto_976_0))) );
}

void RoguePackage__scan_repo_releases__String_String_Platforms( RogueClassPackage* THIS, RogueString* min_version_0, RogueString* max_version_1, RogueClassPlatforms* platforms_2 )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueString*,url_3,(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[123] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(THIS->provider) ))) )))), Rogue_literal_strings[51] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(THIS->repo) ))) )))), Rogue_literal_strings[124] )))) )))));
  ROGUE_DEF_LOCAL_REF(RogueString*,cmd_4,((RogueString__operatorPLUS__String_String( Rogue_literal_strings[125], url_3 ))));
  ROGUE_DEF_LOCAL_REF(RogueClassProcessResult*,process_5,((RogueProcess__run__String_Logical_Logical_Logical( cmd_4, false, false, false ))));
  if (ROGUE_COND(!(((RogueProcessResult__success( process_5 ))))))
  {
    throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(((RogueException*)Rogue_call_ROGUEM4( 13, ROGUE_ARG(((RogueException*)ROGUE_CREATE_REF(RogueClassError*,ROGUE_CREATE_OBJECT(Error)))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[126], url_3 ))) ))))) ));
  }
  ROGUE_DEF_LOCAL_REF(RogueClassValue*,info_6,((RogueJSON__parse__String_Logical( ROGUE_ARG(((RogueString*)(RogueProcessResult__output_string( process_5 )))), true ))));
  {
    ROGUE_DEF_LOCAL_REF(RogueClassValue*,_auto_955_0,(info_6));
    RogueInt32 _auto_956_0 = (0);
    RogueInt32 _auto_957_0 = ((((Rogue_call_ROGUEM2( 29, _auto_955_0 ))) - (1)));
    for (;ROGUE_COND(((_auto_956_0) <= (_auto_957_0)));++_auto_956_0)
    {
      ROGUE_GC_CHECK;
      ROGUE_DEF_LOCAL_REF(RogueClassValue*,release_info_0,(((RogueClassValue*)Rogue_call_ROGUEM3( 22, _auto_955_0, _auto_956_0 ))));
      RogueClassVersionNumber v_7 = (RogueClassVersionNumber( ((RogueString*)(RogueString__after_any__String_Logical( ROGUE_ARG(((RogueString*)Rogue_call_ROGUEM0( 8, ROGUE_ARG(((RogueObject*)((RogueClassValue*)Rogue_call_ROGUEM4( 41, release_info_0, Rogue_literal_strings[132] )))) ))), Rogue_literal_strings[133], false ))), true ));
      {
        {
          {
            if (ROGUE_COND(!!(THIS->specified_version)))
            {
              if (ROGUE_COND(((((RogueVersionNumber__operatorLTGT__String( v_7, ROGUE_ARG(THIS->specified_version) )))) == (0)))) goto _auto_964;
            }
            else
            {
              if (ROGUE_COND(((!!(min_version_0)) && (((((RogueVersionNumber__operatorLTGT__String( v_7, min_version_0 )))) < (0))))))
              {
                continue;
              }
              if (ROGUE_COND(((!!(max_version_1)) && (((((RogueVersionNumber__operatorLTGT__String( v_7, max_version_1 )))) > (0))))))
              {
                continue;
              }
            }
            }
          _auto_964:;
          {
            ROGUE_DEF_LOCAL_REF(RogueClassPlatforms*,_auto_877_8,(platforms_2));
            RoguePackage__release__String_Platforms_String( ROGUE_ARG(THIS), ROGUE_ARG(((RogueString*)Rogue_call_ROGUEM0( 8, ROGUE_ARG(((RogueObject*)((RogueClassValue*)Rogue_call_ROGUEM4( 41, release_info_0, Rogue_literal_strings[150] )))) ))), ROGUE_ARG(((((_auto_877_8))) ? (ROGUE_ARG((RogueClassPlatforms*)_auto_877_8)) : ROGUE_ARG((RoguePlatforms__unix())))), ROGUE_ARG(((RogueString*)(RogueVersionNumber__to_String( v_7 )))) );
            ROGUE_DEF_LOCAL_REF(RogueClassPlatforms*,_auto_878_9,(platforms_2));
            RoguePackage__release__String_Platforms_String( ROGUE_ARG(THIS), ROGUE_ARG(((RogueString*)Rogue_call_ROGUEM0( 8, ROGUE_ARG(((RogueObject*)((RogueClassValue*)Rogue_call_ROGUEM4( 41, release_info_0, Rogue_literal_strings[152] )))) ))), ROGUE_ARG(((((_auto_878_9))) ? (ROGUE_ARG((RogueClassPlatforms*)_auto_878_9)) : ROGUE_ARG(((RogueClassPlatforms*)(RoguePlatforms__operatorPLUS__Platforms( ROGUE_ARG((RoguePlatforms__unix())), ROGUE_ARG((RoguePlatforms__windows())) )))))), ROGUE_ARG(((RogueString*)(RogueVersionNumber__to_String( v_7 )))) );
            if (ROGUE_COND(((!!(THIS->specified_version)) && (((((RogueVersionNumber__operatorLTGT__String( v_7, ROGUE_ARG(THIS->specified_version) )))) == (0))))))
            {
              goto _auto_982;
            }
            }
          goto _auto_960;
        }
      }
      _auto_960:;
    }
  }
  _auto_982:;
}

void RoguePackage__select_version( RogueClassPackage* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((RogueValueList__is_empty( ROGUE_ARG(THIS->releases) )))))
  {
    throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(RoguePackage__error__String( ROGUE_ARG(THIS), Rogue_literal_strings[155] )))) ));
  }
  RogueCharacter platform_0 = ((((((RogueSystem__is_windows())))) ? ((RogueCharacter)'w') : (((((RogueSystem__is_macos())))) ? ((RogueCharacter)'m') : (RogueCharacter)'l')));
  if (ROGUE_COND((RogueString__exists__String( ROGUE_ARG(THIS->version) ))))
  {
    RogueClassBest_String_ best_1 = (RogueClassBest_String_( ((RogueClass_Function_String_String_RETURNSLogical_*)(((RogueClassFunction_983*)ROGUE_SINGLETON(Function_983)))), NULL, false ));
    {
      ROGUE_DEF_LOCAL_REF(RogueClassValueList*,_auto_984_0,(THIS->releases));
      RogueInt32 _auto_985_0 = (0);
      RogueInt32 _auto_986_0 = (((((RogueValueList__count( _auto_984_0 )))) - (1)));
      for (;ROGUE_COND(((_auto_985_0) <= (_auto_986_0)));++_auto_985_0)
      {
        ROGUE_GC_CHECK;
        ROGUE_DEF_LOCAL_REF(RogueClassValue*,release_0,(((RogueClassValue*)Rogue_call_ROGUEM3( 22, ((RogueClassValue*)_auto_984_0), _auto_985_0 ))));
        ROGUE_DEF_LOCAL_REF(RogueString*,v_2,(((RogueString*)Rogue_call_ROGUEM0( 8, ROGUE_ARG(((RogueObject*)((RogueClassValue*)Rogue_call_ROGUEM4( 41, release_0, Rogue_literal_strings[122] )))) ))));
        if (ROGUE_COND(((RogueVersionNumber__is_compatible_with__String( RogueClassVersionNumber( THIS->version, true ), v_2 )))))
        {
          RogueBest_String___consider__String( best_1, v_2 );
        }
      }
    }
    if (ROGUE_COND(!(best_1.exists)))
    {
      {
        ROGUE_DEF_LOCAL_REF(RogueException*,_auto_989_0,0);
        RogueLogical _auto_988_0 = 0;
        ROGUE_DEF_LOCAL_REF(RogueClassStringBuilderPool*,_auto_987_0,(RogueStringBuilder_pool));
        ROGUE_DEF_LOCAL_REF(RogueStringBuilder*,builder_0,(((RogueStringBuilder*)(RogueStringBuilderPool__on_use( _auto_987_0 )))));
        Rogue_ignore_unused(builder_0);

        ROGUE_TRY
        {
          RogueStringBuilder__println__String( builder_0, ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[156] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(THIS->version) ))) )))), Rogue_literal_strings[157] )))) )))) );
          ROGUE_DEF_LOCAL_REF(RogueClassValueTable*,_auto_991_0,(((RogueClassValueTable*)(RogueValueTable__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassValueTable*,ROGUE_CREATE_OBJECT(ValueTable))) )))));
          {
          }
          ROGUE_DEF_LOCAL_REF(RogueClassValueTable*,compatible_3,(_auto_991_0));
          {
            ROGUE_DEF_LOCAL_REF(RogueClassValueList*,_auto_992_0,(THIS->releases));
            RogueInt32 _auto_993_0 = (0);
            RogueInt32 _auto_994_0 = (((((RogueValueList__count( _auto_992_0 )))) - (1)));
            for (;ROGUE_COND(((_auto_993_0) <= (_auto_994_0)));++_auto_993_0)
            {
              ROGUE_GC_CHECK;
              ROGUE_DEF_LOCAL_REF(RogueClassValue*,release_0,(((RogueClassValue*)Rogue_call_ROGUEM3( 22, ((RogueClassValue*)_auto_992_0), _auto_993_0 ))));
              if (ROGUE_COND(((RogueString__contains__Character_Logical( ROGUE_ARG(((RogueString*)Rogue_call_ROGUEM0( 8, ROGUE_ARG(((RogueObject*)((RogueClassValue*)Rogue_call_ROGUEM4( 41, release_0, Rogue_literal_strings[140] )))) ))), platform_0, false )))))
              {
                Rogue_call_ROGUEM6( 144, ((RogueClassValue*)compatible_3), ROGUE_ARG(((RogueString*)Rogue_call_ROGUEM0( 8, ROGUE_ARG(((RogueObject*)((RogueClassValue*)Rogue_call_ROGUEM4( 41, release_0, Rogue_literal_strings[122] )))) ))), ROGUE_ARG(((RogueClassValue*)Rogue_call_ROGUEM4( 41, release_0, Rogue_literal_strings[122] ))) );
              }
            }
          }
          {
            ROGUE_DEF_LOCAL_REF(RogueString_List*,_auto_995_0,(([&]()->RogueString_List*{
              ROGUE_DEF_LOCAL_REF(RogueString_List*,_auto_879_0,(((RogueString_List*)(RogueValue__to_list_String_( ((RogueClassValue*)compatible_3) )))));
              RogueString_List__sort___Function_String_String_RETURNSLogical_( _auto_879_0, ROGUE_ARG(((RogueClass_Function_String_String_RETURNSLogical_*)(((RogueClassFunction_999*)ROGUE_SINGLETON(Function_999))))) );
               return _auto_879_0;
            }())
            ));
            ROGUE_DEF_LOCAL_REF(RogueString_List*,compatible_0,(_auto_995_0));
            {
              ROGUE_DEF_LOCAL_REF(RogueString_List*,_auto_1000_0,(compatible_0));
              RogueInt32 _auto_1001_0 = (0);
              RogueInt32 _auto_1002_0 = (((_auto_1000_0->count) - (1)));
              for (;ROGUE_COND(((_auto_1001_0) <= (_auto_1002_0)));++_auto_1001_0)
              {
                ROGUE_GC_CHECK;
                ROGUE_DEF_LOCAL_REF(RogueString*,_auto_880_0,(((RogueString*)(_auto_1000_0->data->as_objects[_auto_1001_0]))));
                RogueStringBuilder__println__String( builder_0, ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[76], _auto_880_0 ))) );
              }
            }
          }
          throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(RoguePackage__error__String( ROGUE_ARG(THIS), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( builder_0 )))) )))) ));
        }
        ROGUE_CATCH( RogueException,_auto_990_0 )
        {
          _auto_989_0 = ((RogueException*)(_auto_990_0));
        }
        ROGUE_END_TRY
        RogueStringBuilderPool__on_end_use__StringBuilder( _auto_987_0, builder_0 );
        if (ROGUE_COND(!!(_auto_989_0)))
        {
          throw ((RogueException*)Rogue_call_ROGUEM0( 16, _auto_989_0 ));
        }
        if (ROGUE_COND(_auto_988_0))
        {
          return;
        }
      }
    }
    THIS->version = best_1.value;
  }
  else
  {
    RogueClassBest_String_ best_4 = (RogueClassBest_String_( ((RogueClass_Function_String_String_RETURNSLogical_*)(((RogueClassFunction_1003*)ROGUE_SINGLETON(Function_1003)))), NULL, false ));
    {
      ROGUE_DEF_LOCAL_REF(RogueClassValueList*,_auto_1004_0,(THIS->releases));
      RogueInt32 _auto_1005_0 = (0);
      RogueInt32 _auto_1006_0 = (((((RogueValueList__count( _auto_1004_0 )))) - (1)));
      for (;ROGUE_COND(((_auto_1005_0) <= (_auto_1006_0)));++_auto_1005_0)
      {
        ROGUE_GC_CHECK;
        ROGUE_DEF_LOCAL_REF(RogueClassValue*,release_0,(((RogueClassValue*)Rogue_call_ROGUEM3( 22, ((RogueClassValue*)_auto_1004_0), _auto_1005_0 ))));
        ROGUE_DEF_LOCAL_REF(RogueString*,v_5,(((RogueString*)Rogue_call_ROGUEM0( 8, ROGUE_ARG(((RogueObject*)((RogueClassValue*)Rogue_call_ROGUEM4( 41, release_0, Rogue_literal_strings[122] )))) ))));
        RogueBest_String___consider__String( best_4, v_5 );
      }
    }
    THIS->version = best_4.value;
  }
  THIS->url = ((RogueString*)(NULL));
  {
    ROGUE_DEF_LOCAL_REF(RogueClassValueList*,_auto_1007_0,(THIS->releases));
    RogueInt32 _auto_1008_0 = (0);
    RogueInt32 _auto_1009_0 = (((((RogueValueList__count( _auto_1007_0 )))) - (1)));
    for (;ROGUE_COND(((_auto_1008_0) <= (_auto_1009_0)));++_auto_1008_0)
    {
      ROGUE_GC_CHECK;
      ROGUE_DEF_LOCAL_REF(RogueClassValue*,release_0,(((RogueClassValue*)Rogue_call_ROGUEM3( 22, ((RogueClassValue*)_auto_1007_0), _auto_1008_0 ))));
      if (ROGUE_COND(((((((RogueVersionNumber__operatorLTGT__String( RogueClassVersionNumber( ((RogueString*)Rogue_call_ROGUEM0( 8, ROGUE_ARG(((RogueObject*)((RogueClassValue*)Rogue_call_ROGUEM4( 41, release_0, Rogue_literal_strings[122] )))) )), true ), ROGUE_ARG(THIS->version) )))) == (0))) && (((RogueString__contains__Character_Logical( ROGUE_ARG(((RogueString*)Rogue_call_ROGUEM0( 8, ROGUE_ARG(((RogueObject*)((RogueClassValue*)Rogue_call_ROGUEM4( 41, release_0, Rogue_literal_strings[140] )))) ))), platform_0, false )))))))
      {
        THIS->url = ((RogueString*)Rogue_call_ROGUEM0( 8, ROGUE_ARG(((RogueObject*)((RogueClassValue*)Rogue_call_ROGUEM4( 41, release_0, Rogue_literal_strings[139] )))) ));
        THIS->archive_filename = ((RogueString*)Rogue_call_ROGUEM0( 8, ROGUE_ARG(((RogueObject*)((RogueClassValue*)Rogue_call_ROGUEM4( 41, release_0, Rogue_literal_strings[141] )))) ));
        if (ROGUE_COND(!((RogueSystem__is_windows()))))
        {
          if (ROGUE_COND(((RogueString__ends_with__String_Logical( ROGUE_ARG(THIS->url), Rogue_literal_strings[146], true )))))
          {
            goto _auto_1010;
          }
          if (ROGUE_COND(((RogueString__contains__String_Logical( ROGUE_ARG(THIS->url), Rogue_literal_strings[145], false )))))
          {
            goto _auto_1010;
          }
        }
      }
    }
  }
  _auto_1010:;
  THIS->install_folder = ((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(THIS->morlock_home) ))) )))), Rogue_literal_strings[101] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(THIS->provider) ))) )))), Rogue_literal_strings[51] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(THIS->app_name) ))) )))), Rogue_literal_strings[51] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(THIS->version) ))) )))) )));
  THIS->bin_folder = (RogueString__operatorSLASH__String_String( ROGUE_ARG(THIS->install_folder), Rogue_literal_strings[68] ));
}

void RoguePackage__unpack__String( RogueClassPackage* THIS, RogueString* destination_folder_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!((RogueFile__exists__String( ROGUE_ARG(THIS->archive_filename) )))))
  {
    throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(RoguePackage__error__String( ROGUE_ARG(THIS), Rogue_literal_strings[165] )))) ));
  }
  THIS->is_unpacked = ((RogueString*)(RogueLogical__to_String( true )));
  if (ROGUE_COND(((RogueString__ends_with__String_Logical( ROGUE_ARG(THIS->archive_filename), Rogue_literal_strings[148], true )))))
  {
    RogueZip__extract__File_Logical( ROGUE_ARG(((RogueClassZip*)(RogueZip__init__File_Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassZip*,ROGUE_CREATE_OBJECT(Zip))), ROGUE_ARG(((RogueClassFile*)(RogueFile__init__String( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassFile*,ROGUE_CREATE_OBJECT(File))), ROGUE_ARG(THIS->archive_filename) )))), 6 )))), ROGUE_ARG(((RogueClassFile*)(RogueFile__init__String( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassFile*,ROGUE_CREATE_OBJECT(File))), destination_folder_0 )))), false );
  }
  else if (ROGUE_COND(((RogueString__ends_with__String_Logical( ROGUE_ARG(THIS->archive_filename), Rogue_literal_strings[146], false )))))
  {
    RoguePackage__execute__String_Logical( ROGUE_ARG(THIS), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[170] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG((RogueFile__esc__String( destination_folder_0 ))) ))) )))), Rogue_literal_strings[171] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG((RogueFile__esc__String( ROGUE_ARG(THIS->archive_filename) ))) ))) )))) )))), false );
  }
  else
  {
    throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(RoguePackage__error__String( ROGUE_ARG(THIS), Rogue_literal_strings[172] )))) ));
  }
}

RogueClassPackageInfo* RoguePackageInfo__init_object( RogueClassPackageInfo* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  return (RogueClassPackageInfo*)(THIS);
}

RogueString* RoguePackageInfo__description( RogueClassPackageInfo* THIS )
{
  ROGUE_GC_CHECK;
  {
    ROGUE_DEF_LOCAL_REF(RogueException*,_auto_2621_0,0);
    ROGUE_DEF_LOCAL_REF(RogueClassStringBuilderPool*,_auto_2620_0,(RogueStringBuilder_pool));
    ROGUE_DEF_LOCAL_REF(RogueString*,_auto_2622_0,0);
    ROGUE_DEF_LOCAL_REF(RogueStringBuilder*,builder_0,(((RogueStringBuilder*)(RogueStringBuilderPool__on_use( _auto_2620_0 )))));
    Rogue_ignore_unused(builder_0);

    ROGUE_TRY
    {
      RogueStringBuilder__println__String( builder_0, ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[283], ROGUE_ARG(THIS->name) ))) );
      RogueStringBuilder__println__String( builder_0, ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[284], ROGUE_ARG(THIS->host) ))) );
      RogueStringBuilder__println__String( builder_0, ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[285], ROGUE_ARG(THIS->provider) ))) );
      RogueStringBuilder__println__String( builder_0, ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[286], ROGUE_ARG(THIS->repo) ))) );
      RogueStringBuilder__println__String( builder_0, ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[287], ROGUE_ARG(THIS->app_name) ))) );
      RogueStringBuilder__println__String( builder_0, ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[288], ROGUE_ARG(THIS->version) ))) );
      RogueStringBuilder__println__String( builder_0, ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[289], ROGUE_ARG(THIS->url) ))) );
      RogueStringBuilder__println__String( builder_0, ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[290], ROGUE_ARG(THIS->folder) ))) );
      RogueStringBuilder__println__String( builder_0, ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[291], ROGUE_ARG(THIS->filepath) ))) );
      RogueStringBuilder__println__String( builder_0, ROGUE_ARG(((RogueString*)(RogueString__operatorPLUS__Logical( ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], Rogue_literal_strings[292] ))), ROGUE_ARG(THIS->using_local_script) )))) );
      {
        _auto_2622_0 = ((RogueString*)(((RogueString*)(RogueStringBuilder__to_String( builder_0 )))));
        goto _auto_2624;
      }
    }
    ROGUE_CATCH( RogueException,_auto_2623_0 )
    {
      _auto_2621_0 = ((RogueException*)(_auto_2623_0));
    }
    ROGUE_END_TRY
    _auto_2624:;
    RogueStringBuilderPool__on_end_use__StringBuilder( _auto_2620_0, builder_0 );
    if (ROGUE_COND(!!(_auto_2621_0)))
    {
      throw ((RogueException*)Rogue_call_ROGUEM0( 16, _auto_2621_0 ));
    }
    return (RogueString*)(_auto_2622_0);
  }
}

RogueString* RoguePackageInfo__type_name( RogueClassPackageInfo* THIS )
{
  return (RogueString*)(Rogue_literal_strings[363]);
}

RogueClassPackageInfo* RoguePackageInfo__init__String_String( RogueClassPackageInfo* THIS, RogueString* _auto_885_0, RogueString* _auto_884_1 )
{
  ROGUE_GC_CHECK;
  THIS->morlock_home = _auto_885_0;
  THIS->url = _auto_884_1;
  if (ROGUE_COND(((RogueString__contains__Character_Logical( ROGUE_ARG(THIS->url), (RogueCharacter)'@', false )))))
  {
    THIS->version = ((RogueString*)(RogueString__after_last__Character_Logical( ROGUE_ARG(THIS->url), (RogueCharacter)'@', false )));
    THIS->url = ((RogueString*)(RogueString__before_last__Character_Logical( ROGUE_ARG(THIS->url), (RogueCharacter)'@', false )));
  }
  if (ROGUE_COND(((RogueString__contains__String_Logical( ROGUE_ARG(THIS->url), Rogue_literal_strings[92], false )))))
  {
    ROGUE_DEF_LOCAL_REF(RogueString_List*,parts_2,(((RogueString_List*)(RogueString__extract_strings__String_Logical( ROGUE_ARG(THIS->url), Rogue_literal_strings[93], false )))));
    if (ROGUE_COND(!(!!(parts_2))))
    {
      ROGUE_DEF_LOCAL_REF(RogueString*,sample_3,(Rogue_literal_strings[94]));
      throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(((RogueException*)Rogue_call_ROGUEM4( 13, ROGUE_ARG(((RogueException*)ROGUE_CREATE_REF(RogueClassError*,ROGUE_CREATE_OBJECT(Error)))), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[95] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(THIS->url) ))) )))), Rogue_literal_strings[96] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], sample_3 ))) )))), Rogue_literal_strings[97] )))) )))) ))))) ));
    }
    if (ROGUE_COND(!(((RogueString__ends_with__String_Logical( ROGUE_ARG(THIS->url), Rogue_literal_strings[98], true ))))))
    {
      THIS->url = ((RogueString*)(NULL));
    }
    THIS->host = ((RogueString*)(parts_2->data->as_objects[1]));
    ROGUE_DEF_LOCAL_REF(RogueString*,path_4,(((RogueString*)(parts_2->data->as_objects[2]))));
    THIS->app_name = ((RogueString*)(RogueString__before_last__Character_Logical( ROGUE_ARG((RogueFile__filename__String( path_4 ))), (RogueCharacter)'.', false )));
    if (ROGUE_COND(((RogueString__contains__Character_Logical( path_4, (RogueCharacter)'/', false )))))
    {
      THIS->provider = ((RogueString*)(RogueString__before_first__Character_Logical( path_4, (RogueCharacter)'/', false )));
      THIS->repo = ((RogueString*)(RogueString__before_first__Character_Logical( ROGUE_ARG(((RogueString*)(RogueString__after_first__Character_Logical( path_4, (RogueCharacter)'/', false )))), (RogueCharacter)'/', false )));
    }
    else
    {
      THIS->provider = THIS->app_name;
      THIS->repo = THIS->app_name;
    }
  }
  else
  {
    switch (((RogueString__count__Character( ROGUE_ARG(THIS->url), (RogueCharacter)'/' ))))
    {
      case 0:
      {
        THIS->provider = THIS->url;
        THIS->app_name = THIS->url;
        THIS->repo = THIS->url;
        THIS->url = ((RogueString*)(NULL));
        break;
      }
      case 1:
      {
        THIS->provider = ((RogueString*)(RogueString__before_first__Character_Logical( ROGUE_ARG(THIS->url), (RogueCharacter)'/', false )));
        THIS->app_name = ((RogueString*)(RogueString__after_first__Character_Logical( ROGUE_ARG(THIS->url), (RogueCharacter)'/', false )));
        THIS->repo = THIS->app_name;
        THIS->url = ((RogueString*)(NULL));
        break;
      }
      case 2:
      {
        ROGUE_DEF_LOCAL_REF(RogueString_List*,parts_5,(((RogueString_List*)(RogueString__split__Character_Logical( ROGUE_ARG(THIS->url), (RogueCharacter)'/', false )))));
        if (ROGUE_COND((((RogueString__operatorEQUALSEQUALS__String_String( ROGUE_ARG(((RogueString*)(parts_5->data->as_objects[0]))), Rogue_literal_strings[99] ))) || (((RogueString__contains__Character_Logical( ROGUE_ARG(((RogueString*)(parts_5->data->as_objects[0]))), (RogueCharacter)'.', false )))))))
        {
          THIS->host = ((RogueString*)(parts_5->data->as_objects[0]));
          THIS->provider = ((RogueString*)(parts_5->data->as_objects[1]));
        }
        else
        {
          THIS->provider = ((RogueString*)(parts_5->data->as_objects[0]));
          THIS->repo = ((RogueString*)(parts_5->data->as_objects[1]));
        }
        THIS->app_name = ((RogueString*)(parts_5->data->as_objects[2]));
        THIS->url = ((RogueString*)(NULL));
        break;
      }
      default:
      {
        RoguePackageInfo__init__String_String( ROGUE_ARG(THIS), ROGUE_ARG(THIS->morlock_home), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[100], ROGUE_ARG(THIS->url) ))) );
        return (RogueClassPackageInfo*)(THIS);
      }
    }
  }
  if (ROGUE_COND(!(!!(THIS->repo))))
  {
    THIS->repo = THIS->app_name;
  }
  THIS->name = (RogueString__operatorSLASH__String_String( ROGUE_ARG(THIS->provider), ROGUE_ARG(THIS->app_name) ));
  THIS->folder = ((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(THIS->morlock_home) ))) )))), Rogue_literal_strings[101] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(THIS->provider) ))) )))), Rogue_literal_strings[51] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(THIS->app_name) ))) )))) )));
  THIS->filepath = ((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(THIS->folder) ))) )))), Rogue_literal_strings[51] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(THIS->app_name) ))) )))), Rogue_literal_strings[98] )))) )));
  if (ROGUE_COND(!(!!(THIS->url))))
  {
    ROGUE_DEF_LOCAL_REF(RogueString*,url_filepath_6,((RogueString__operatorSLASH__String_String( ROGUE_ARG(THIS->folder), Rogue_literal_strings[102] ))));
    if (ROGUE_COND((RogueFile__exists__String( url_filepath_6 ))))
    {
      THIS->url = ((RogueString*)(RogueString__trimmed( ROGUE_ARG((RogueString__create__File_StringEncoding( ROGUE_ARG(((RogueClassFile*)(RogueFile__init__String( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassFile*,ROGUE_CREATE_OBJECT(File))), url_filepath_6 )))), RogueClassStringEncoding( 3 ) ))) )));
    }
  }
  if (ROGUE_COND(!!(THIS->host)))
  {
    {
      ROGUE_DEF_LOCAL_REF(RogueString*,_auto_886_0,(THIS->host));
      if (ROGUE_COND((RogueString__operatorEQUALSEQUALS__String_String( _auto_886_0, Rogue_literal_strings[99] ))))
      {
        THIS->host = Rogue_literal_strings[105];
      }
    }
  }
  else if (ROGUE_COND(!!(THIS->url)))
  {
    THIS->host = ((RogueString*)(RogueString__before_first__Character_Logical( ROGUE_ARG(((RogueString*)(RogueString__after_any__String_Logical( ROGUE_ARG(THIS->url), Rogue_literal_strings[92], false )))), (RogueCharacter)'/', false )));
  }
  else
  {
    THIS->host = Rogue_literal_strings[105];
  }
  THIS->installed_versions = (((((RogueFile__exists__String( ROGUE_ARG(THIS->folder) ))))) ? (ROGUE_ARG((RogueString_List*)(RogueFile__listing__String_OptionalFilePattern_Logical_Logical_Logical_Logical_Logical_Logical( ROGUE_ARG(THIS->folder), (RogueOptionalFilePattern__create()), false, false, true, false, true, false )))) : ROGUE_ARG(((RogueString_List*)(RogueString_List__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueString_List*,ROGUE_CREATE_OBJECT(String_List))) )))));
  RogueString_List__sort___Function_String_String_RETURNSLogical_( ROGUE_ARG(THIS->installed_versions), ROGUE_ARG(((RogueClass_Function_String_String_RETURNSLogical_*)(((RogueClassFunction_950*)ROGUE_SINGLETON(Function_950))))) );
  return (RogueClassPackageInfo*)(THIS);
}

void RoguePackageInfo__fetch_latest_script( RogueClassPackageInfo* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(THIS->using_local_script)))
  {
    RogueGlobal__println( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG((((((RogueFile__exists__String( ROGUE_ARG(THIS->filepath) ))))) ? (ROGUE_ARG((RogueString*)Rogue_literal_strings[232])) : ROGUE_ARG(Rogue_literal_strings[233]))) ))) )))), Rogue_literal_strings[42] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(THIS->app_name) ))) )))), Rogue_literal_strings[234] )))) )))) )))) );
    if (ROGUE_COND(!(!!(THIS->url))))
    {
      {
        ROGUE_DEF_LOCAL_REF(RogueString*,_auto_1089_0,(THIS->host));
        if (ROGUE_COND((RogueString__operatorEQUALSEQUALS__String_String( _auto_1089_0, Rogue_literal_strings[105] ))))
        {
          ROGUE_DEF_LOCAL_REF(RogueString*,cmd_0,(Rogue_literal_strings[235]));
          cmd_0 = ((RogueString*)(((RogueString*)(RogueString__operatorPLUS__String( cmd_0, ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[123] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(THIS->provider) ))) )))), Rogue_literal_strings[51] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(THIS->repo) ))) )))), Rogue_literal_strings[236] )))) )))) )))));
          ROGUE_DEF_LOCAL_REF(RogueClassProcessResult*,result_1,((RogueProcess__run__String_Logical_Logical_Logical( cmd_0, false, true, false ))));
          if (ROGUE_COND(!(((RogueProcessResult__success( result_1 ))))))
          {
            throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(((RogueException*)Rogue_call_ROGUEM4( 13, ROGUE_ARG(((RogueException*)ROGUE_CREATE_REF(RogueClassError*,ROGUE_CREATE_OBJECT(Error)))), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[237] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(THIS->provider) ))) )))), Rogue_literal_strings[51] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(THIS->repo) ))) )))), Rogue_literal_strings[238] )))) )))) ))))) ));
          }
          ROGUE_DEF_LOCAL_REF(RogueClassValue*,data_2,((RogueJSON__parse__String_Logical( ROGUE_ARG(((RogueString*)(RogueProcessResult__to_String( result_1 )))), true ))));
          if (ROGUE_COND(!((Rogue_call_ROGUEM5( 58, data_2 )))))
          {
            throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(((RogueException*)Rogue_call_ROGUEM4( 13, ROGUE_ARG(((RogueException*)ROGUE_CREATE_REF(RogueClassError*,ROGUE_CREATE_OBJECT(Error)))), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[239] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(THIS->provider) ))) )))), Rogue_literal_strings[51] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(THIS->repo) ))) )))) )))) ))))) ));
          }
          ROGUE_DEF_LOCAL_REF(RogueClassValue*,folder_info_3,(((RogueClassValue*)Rogue_call_ROGUEM4( 39, data_2, ROGUE_ARG(((RogueClass_Function_Value_RETURNSLogical_*)(((RogueClassFunction_1088*)ROGUE_SINGLETON(Function_1088))))) ))));
          if (ROGUE_COND(!((RogueOptionalValue__operator__Value( folder_info_3 )))))
          {
            throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(((RogueException*)Rogue_call_ROGUEM4( 13, ROGUE_ARG(((RogueException*)ROGUE_CREATE_REF(RogueClassError*,ROGUE_CREATE_OBJECT(Error)))), Rogue_literal_strings[240] ))))) ));
          }
          ROGUE_DEF_LOCAL_REF(RogueString*,branch_4,(((RogueString*)(RogueString__before_first__Character_Logical( ROGUE_ARG(((RogueString*)(RogueString__after_last__String_Logical( ROGUE_ARG(((RogueString*)(RogueString__after_last__Character_Logical( ROGUE_ARG(((RogueString*)Rogue_call_ROGUEM0( 8, ROGUE_ARG(((RogueObject*)((RogueClassValue*)Rogue_call_ROGUEM4( 41, folder_info_3, Rogue_literal_strings[139] )))) ))), (RogueCharacter)'?', false )))), Rogue_literal_strings[241], false )))), (RogueCharacter)'&', false )))));
          if (ROGUE_COND(!((RogueString__exists__String( branch_4 )))))
          {
            branch_4 = ((RogueString*)(Rogue_literal_strings[242]));
          }
          THIS->url = ((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[243] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(THIS->provider) ))) )))), Rogue_literal_strings[51] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(THIS->repo) ))) )))), Rogue_literal_strings[51] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], branch_4 ))) )))), Rogue_literal_strings[51] )))), ROGUE_ARG(((RogueString*)(RogueString__operatorPLUS__Object( ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], Rogue_literal_strings[0] ))), ROGUE_ARG(((RogueObject*)(((RogueClassValue*)Rogue_call_ROGUEM4( 41, folder_info_3, Rogue_literal_strings[224] ))))) )))) )))), Rogue_literal_strings[51] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(THIS->app_name) ))) )))), Rogue_literal_strings[98] )))) )));
        }
        else
        {
          throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(((RogueException*)Rogue_call_ROGUEM4( 13, ROGUE_ARG(((RogueException*)ROGUE_CREATE_REF(RogueClassError*,ROGUE_CREATE_OBJECT(Error)))), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[244] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(THIS->host) ))) )))), Rogue_literal_strings[245] )))) )))) ))))) ));
        }
      }
    }
    if (ROGUE_COND(!((RogueFile__is_folder__String( ROGUE_ARG(THIS->folder) )))))
    {
      RogueGlobal__println( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[246], ROGUE_ARG(THIS->folder) ))) )))) );
      RogueFile__create_folder__String( ROGUE_ARG(THIS->folder) );
    }
    RoguePackageInfo__execute__String_Logical_Logical_Logical( ROGUE_ARG(THIS), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[162] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(THIS->url) ))) )))), Rogue_literal_strings[163] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(THIS->filepath) ))) )))) )))), false, false, true );
    RogueFile__save__String_String_Logical( ROGUE_ARG((RogueString__operatorSLASH__String_String( ROGUE_ARG(THIS->folder), Rogue_literal_strings[102] ))), ROGUE_ARG(THIS->url), false );
  }
}

RogueLogical RoguePackageInfo__execute__String_Logical_Logical_Logical( RogueClassPackageInfo* THIS, RogueString* cmd_0, RogueLogical suppress_error_1, RogueLogical allow_sudo_2, RogueLogical bg_3 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(bg_3)))
  {
    RogueGlobal__println( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[58], cmd_0 ))) )))) );
  }
  if (ROGUE_COND(((0) == ((RogueSystem__run__String( cmd_0 ))))))
  {
    return (RogueLogical)(true);
  }
  if (ROGUE_COND(allow_sudo_2))
  {
    RogueGlobal__println( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[59] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], cmd_0 ))) )))), Rogue_literal_strings[60] )))) )))) )))) );
    return (RogueLogical)(((RoguePackageInfo__execute__String_Logical_Logical_Logical( ROGUE_ARG(THIS), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[61], cmd_0 ))), suppress_error_1, false, false ))));
  }
  if (ROGUE_COND(suppress_error_1))
  {
    return (RogueLogical)(false);
  }
  throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(((RogueException*)Rogue_call_ROGUEM4( 13, ROGUE_ARG(((RogueException*)ROGUE_CREATE_REF(RogueClassError*,ROGUE_CREATE_OBJECT(Error)))), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[62] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], cmd_0 ))) )))) )))) ))))) ));
}

RogueClassValue* RoguePackageInfo__package_args( RogueClassPackageInfo* THIS )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueClassValueTable*,_auto_951_0,(((RogueClassValueTable*)(RogueValueTable__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassValueTable*,ROGUE_CREATE_OBJECT(ValueTable))) )))));
  {
    Rogue_call_ROGUEM6( 144, ((RogueClassValue*)_auto_951_0), Rogue_literal_strings[119], ROGUE_ARG((RogueValue__create__String( ROGUE_ARG(THIS->morlock_home) ))) );
    Rogue_call_ROGUEM6( 144, ((RogueClassValue*)_auto_951_0), Rogue_literal_strings[122], ROGUE_ARG((RogueValue__create__String( ROGUE_ARG(THIS->version) ))) );
    Rogue_call_ROGUEM6( 144, ((RogueClassValue*)_auto_951_0), Rogue_literal_strings[112], ROGUE_ARG((RogueValue__create__String( ROGUE_ARG(THIS->filepath) ))) );
    Rogue_call_ROGUEM6( 144, ((RogueClassValue*)_auto_951_0), Rogue_literal_strings[120], ROGUE_ARG((RogueValue__create__String( ROGUE_ARG(THIS->host) ))) );
    Rogue_call_ROGUEM6( 144, ((RogueClassValue*)_auto_951_0), Rogue_literal_strings[121], ROGUE_ARG((RogueValue__create__String( ROGUE_ARG(THIS->repo) ))) );
  }
  return (RogueClassValue*)(((RogueClassValue*)(_auto_951_0)));
}

RogueClassFileListing* RogueFileListing__init_object( RogueClassFileListing* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  THIS->filepath_segments = ((RogueString_List*)(RogueString_List__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueString_List*,ROGUE_CREATE_OBJECT(String_List))) )));
  THIS->empty_segments = ((RogueString_List*)(RogueString_List__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueString_List*,ROGUE_CREATE_OBJECT(String_List))) )));
  THIS->results = ((RogueString_List*)(RogueString_List__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueString_List*,ROGUE_CREATE_OBJECT(String_List))) )));
  return (RogueClassFileListing*)(THIS);
}

RogueString* RogueFileListing__type_name( RogueClassFileListing* THIS )
{
  return (RogueString*)(Rogue_literal_strings[364]);
}

RogueClassFileListing* RogueFileListing__init__String_String_FileOptions( RogueClassFileListing* THIS, RogueString* _auto_892_0, RogueString* _auto_891_1, RogueClassFileOptions _auto_890_2 )
{
  ROGUE_GC_CHECK;
  THIS->folder = _auto_892_0;
  THIS->pattern = _auto_891_1;
  THIS->options = _auto_890_2;
  THIS->folder = ((RogueString*)(RogueFileListing__fix__String( ROGUE_ARG(THIS), ROGUE_ARG(THIS->folder) )));
  THIS->pattern = ((RogueString*)(RogueFileListing__fix__String( ROGUE_ARG(THIS), ROGUE_ARG(THIS->pattern) )));
  if (ROGUE_COND(!!(THIS->folder)))
  {
    THIS->folder = (RogueFile__expand_path__String( ROGUE_ARG(THIS->folder) ));
  }
  if (ROGUE_COND(!!(THIS->pattern)))
  {
    THIS->pattern = (RogueFile__expand_path__String( ROGUE_ARG(THIS->pattern) ));
  }
  if (ROGUE_COND((((RogueFile__exists__String( ROGUE_ARG(THIS->folder) ))) && (!((RogueFile__is_folder__String( ROGUE_ARG(THIS->folder) )))))))
  {
    ROGUE_DEF_LOCAL_REF(RogueString*,filepath_3,(THIS->folder));
    if (ROGUE_COND(((!!(((THIS->options.flags) & (32)))) && (((RogueString__begins_with__Character_Logical( filepath_3, (RogueCharacter)'.', false )))))))
    {
      return (RogueClassFileListing*)(THIS);
    }
    if (ROGUE_COND(((RogueFileOptions__keeping_files( THIS->options )))))
    {
      if (ROGUE_COND(!!(((THIS->options.flags) & (4)))))
      {
        filepath_3 = ((RogueString*)((RogueFile__absolute_filepath__String( filepath_3 ))));
      }
      if (ROGUE_COND(!!(((THIS->options.flags) & (2)))))
      {
        filepath_3 = ((RogueString*)((RogueFile__filename__String( filepath_3 ))));
      }
      RogueString_List__add__String( ROGUE_ARG(THIS->results), filepath_3 );
    }
    return (RogueClassFileListing*)(THIS);
  }
  THIS->callback = ((RogueClass_Function_String__*)(((RogueClassFileListing_collect_String_*)(RogueFileListing_collect_String___init__FileListing( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassFileListing_collect_String_*,ROGUE_CREATE_OBJECT(FileListing_collect_String_))), ROGUE_ARG(THIS) )))));
  if (ROGUE_COND(((void*)THIS->pattern) == ((void*)NULL)))
  {
    ROGUE_DEF_LOCAL_REF(RogueString_List*,path_segments_4,(((RogueString_List*)(RogueString__split__Character_Logical( ROGUE_ARG((RogueFile__without_trailing_separator__String( ROGUE_ARG((RogueFile__fix_slashes__String( ROGUE_ARG(THIS->folder) ))) ))), (RogueCharacter)'/', false )))));
    ROGUE_DEF_LOCAL_REF(RogueString_List*,pattern_segments_5,(((RogueString_List*)(RogueString_List__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueString_List*,ROGUE_CREATE_OBJECT(String_List))) )))));
    RogueOptionalInt32 first_wildcard_i_6 = (((RogueString_List__locate___Function_String_RETURNSLogical__Int32( path_segments_4, ROGUE_ARG(((RogueClass_Function_String_RETURNSLogical_*)(((RogueClassFunction_896*)ROGUE_SINGLETON(Function_896))))), 0 ))));
    if (ROGUE_COND(first_wildcard_i_6.exists))
    {
      {
        ROGUE_DEF_LOCAL_REF(RogueString_List*,_auto_899_0,(path_segments_4));
        RogueInt32 _auto_900_0 = (first_wildcard_i_6.value);
        RogueInt32 _auto_901_0 = (((_auto_899_0->count) - (1)));
        for (;ROGUE_COND(((_auto_900_0) <= (_auto_901_0)));++_auto_900_0)
        {
          ROGUE_GC_CHECK;
          ROGUE_DEF_LOCAL_REF(RogueString*,p_0,(((RogueString*)(_auto_899_0->data->as_objects[_auto_900_0]))));
          RogueString_List__add__String( pattern_segments_5, p_0 );
        }
      }
      RogueString_List__discard_from__Int32( path_segments_4, ROGUE_ARG(first_wildcard_i_6.value) );
    }
    if (ROGUE_COND(((RogueString_List__is_empty( path_segments_4 )))))
    {
      THIS->folder = Rogue_literal_strings[52];
    }
    else
    {
      THIS->folder = ((RogueString*)(RogueString_List__join__String( path_segments_4, Rogue_literal_strings[51] )));
    }
    if (ROGUE_COND(((RogueString_List__is_empty( pattern_segments_5 )))))
    {
      THIS->pattern = Rogue_literal_strings[109];
    }
    else
    {
      THIS->pattern = ((RogueString*)(RogueString_List__join__String( pattern_segments_5, Rogue_literal_strings[51] )));
    }
  }
  if (ROGUE_COND(!!(((THIS->options.flags) & (4)))))
  {
    THIS->folder = (RogueFile__absolute_filepath__String( ROGUE_ARG(THIS->folder) ));
  }
  THIS->folder = (RogueFile__ensure_ends_with_separator__String( ROGUE_ARG(THIS->folder) ));
  ROGUE_DEF_LOCAL_REF(RogueString*,filepath_pattern_7,((RogueString__operatorPLUS__String_String( ROGUE_ARG(THIS->folder), ROGUE_ARG(THIS->pattern) ))));
  RogueLogical file_exists_8 = (false);
  if (ROGUE_COND((RogueFile__exists__String( filepath_pattern_7 ))))
  {
    if (ROGUE_COND((RogueFile__is_folder__String( filepath_pattern_7 ))))
    {
      THIS->pattern = ((RogueString*)(RogueString__operatorPLUS__String( ROGUE_ARG(THIS->pattern), Rogue_literal_strings[110] )));
    }
    else if (ROGUE_COND(((((RogueString__count( ROGUE_ARG(THIS->pattern) )))) > (0))))
    {
      file_exists_8 = ((RogueLogical)(true));
    }
  }
  THIS->path_segments = ((RogueString_List*)(RogueString__split__Character_Logical( ROGUE_ARG((RogueFile__fix_slashes__String( ROGUE_ARG(THIS->folder) ))), (RogueCharacter)'/', false )));
  if (ROGUE_COND((RogueString__operatorEQUALSEQUALS__String_String( ROGUE_ARG(((RogueString*)(RogueString_List__first( ROGUE_ARG(THIS->path_segments) )))), Rogue_literal_strings[52] ))))
  {
    RogueString_List__remove_first( ROGUE_ARG(THIS->path_segments) );
  }
  if (ROGUE_COND(((!!(THIS->path_segments->count)) && ((RogueString__operatorEQUALSEQUALS__String_String( ROGUE_ARG(((RogueString*)(RogueString_List__last( ROGUE_ARG(THIS->path_segments) )))), Rogue_literal_strings[0] ))))))
  {
    RogueString_List__remove_last( ROGUE_ARG(THIS->path_segments) );
  }
  THIS->pattern_segments = ((RogueString_List*)(RogueString__split__Character_Logical( ROGUE_ARG((RogueFile__fix_slashes__String( ROGUE_ARG(THIS->pattern) ))), (RogueCharacter)'/', false )));
  if (ROGUE_COND(!(file_exists_8)))
  {
    RogueFile___listing__String__Function_String__( ROGUE_ARG(THIS->folder), ROGUE_ARG(THIS->callback) );
  }
  ([&]()->RogueString_List*{
    ROGUE_DEF_LOCAL_REF(RogueString_List*,_auto_888_0,(THIS->filepath_segments));
    RogueString_List__clear( _auto_888_0 );
    RogueString_List__add__String_List( _auto_888_0, ROGUE_ARG(THIS->path_segments) );
    RogueString_List__add__String_List( _auto_888_0, ROGUE_ARG(THIS->pattern_segments) );
     return _auto_888_0;
  }());
  THIS->pattern = ((RogueString*)(RogueString_List__join__String( ROGUE_ARG(THIS->filepath_segments), Rogue_literal_strings[51] )));
  ROGUE_DEF_LOCAL_REF(RogueString*,adjusted_folder_9,(THIS->folder));
  if (ROGUE_COND(((((!!(((THIS->options.flags) & (2)))) && (((RogueString__begins_with__String_Logical( ROGUE_ARG(THIS->folder), Rogue_literal_strings[111], false )))))) && (((((RogueString__count( ROGUE_ARG(THIS->folder) )))) > (2))))))
  {
    adjusted_folder_9 = ((RogueString*)(((RogueString*)(RogueString__rightmost__Int32( ROGUE_ARG(THIS->folder), -2 )))));
  }
  if (ROGUE_COND(file_exists_8))
  {
    ROGUE_DEF_LOCAL_REF(RogueString*,filepath_10,(filepath_pattern_7));
    if (ROGUE_COND(((!!(((THIS->options.flags) & (2)))) && (((RogueString__begins_with__String_Logical( filepath_10, adjusted_folder_9, false )))))))
    {
      filepath_10 = ((RogueString*)(((RogueString*)(RogueString__after_first__String_Logical( filepath_10, adjusted_folder_9, false )))));
    }
    RogueString_List__add__String( ROGUE_ARG(THIS->results), filepath_10 );
    return (RogueClassFileListing*)(THIS);
  }
  {
    ROGUE_DEF_LOCAL_REF(RogueClassListRewriter_String_*,writer_0,(((RogueClassListRewriter_String_*)(RogueString_List__rewriter( ROGUE_ARG(THIS->results) )))));
    while (ROGUE_COND(((RogueListRewriter_String___has_another( writer_0 )))))
    {
      ROGUE_GC_CHECK;
      ROGUE_DEF_LOCAL_REF(RogueString*,filepath_0,(((RogueString*)(RogueListRewriter_String___read( writer_0 )))));
      {
        {
          {
            if (ROGUE_COND(!!(((RogueString__count( ROGUE_ARG(THIS->pattern) ))))))
            {
              if ( !((RogueFile__matches_wildcard_pattern__String_String_Logical( filepath_0, ROGUE_ARG(THIS->pattern), false ))) ) goto _auto_947;
            }
            if (ROGUE_COND(((!!(((THIS->options.flags) & (2)))) && (((RogueString__begins_with__String_Logical( filepath_0, adjusted_folder_9, false )))))))
            {
              filepath_0 = ((RogueString*)(((RogueString*)(RogueString__after_first__String_Logical( filepath_0, adjusted_folder_9, false )))));
            }
            }
          {
            RogueListRewriter_String___write__String( writer_0, filepath_0 );
            }
          goto _auto_941;
        }
        _auto_947:;
      }
      _auto_941:;
    }
  }
  if (ROGUE_COND(!(!!(((THIS->options.flags) & (64))))))
  {
    RogueString_List__sort___Function_String_String_RETURNSLogical_( ROGUE_ARG(THIS->results), ROGUE_ARG(((RogueClass_Function_String_String_RETURNSLogical_*)(((RogueClassFunction_948*)ROGUE_SINGLETON(Function_948))))) );
  }
  return (RogueClassFileListing*)(THIS);
}

void RogueFileListing__collect__String( RogueClassFileListing* THIS, RogueString* filename_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND((((RogueLogical__create__Int32( ROGUE_ARG(((THIS->options.flags) & (32))) ))) && (((RogueString__begins_with__Character_Logical( filename_0, (RogueCharacter)'.', false )))))))
  {
    return;
  }
  if (ROGUE_COND(((!!(THIS->pattern_segments->count)) && (!((RogueFile__matches_wildcard_pattern__String_String_Logical( filename_0, ROGUE_ARG(((RogueString*)(RogueString_List__first( ROGUE_ARG(THIS->pattern_segments) )))), false )))))))
  {
    return;
  }
  ([&]()->RogueString_List*{
    ROGUE_DEF_LOCAL_REF(RogueString_List*,_auto_889_0,(THIS->filepath_segments));
    RogueString_List__clear( _auto_889_0 );
    RogueString_List__add__String_List( _auto_889_0, ROGUE_ARG(THIS->path_segments) );
    RogueString_List__add__String( _auto_889_0, filename_0 );
     return _auto_889_0;
  }());
  ROGUE_DEF_LOCAL_REF(RogueString*,filepath_1,(((RogueString*)(RogueString_List__join__String( ROGUE_ARG(THIS->filepath_segments), Rogue_literal_strings[51] )))));
  if (ROGUE_COND((RogueFile__is_folder__String( filepath_1 ))))
  {
    if (ROGUE_COND(((RogueFileOptions__keeping_folders( THIS->options )))))
    {
      RogueString_List__add__String( ROGUE_ARG(THIS->results), filepath_1 );
    }
    if (ROGUE_COND(!!(THIS->pattern_segments->count)))
    {
      RogueString_List__add__String( ROGUE_ARG(THIS->path_segments), filename_0 );
      if (ROGUE_COND(((RogueString__begins_with__String_Logical( ROGUE_ARG(((RogueString*)(RogueString_List__first( ROGUE_ARG(THIS->pattern_segments) )))), Rogue_literal_strings[106], false )))))
      {
        RogueClassFileOptions saved_options_2 = (THIS->options);
        THIS->options = RogueClassFileOptions( ((THIS->options.flags) | (1)) );
        ROGUE_DEF_LOCAL_REF(RogueString_List*,saved_segments_3,(THIS->pattern_segments));
        THIS->pattern_segments = THIS->empty_segments;
        RogueFile___listing__String__Function_String__( filepath_1, ROGUE_ARG(THIS->callback) );
        THIS->pattern_segments = saved_segments_3;
        THIS->options = saved_options_2;
      }
      else
      {
        ROGUE_DEF_LOCAL_REF(RogueString*,saved_segment_4,(((RogueString*)(RogueString_List__remove_first( ROGUE_ARG(THIS->pattern_segments) )))));
        RogueFile___listing__String__Function_String__( filepath_1, ROGUE_ARG(THIS->callback) );
        RogueString_List__insert__String_Int32( ROGUE_ARG(THIS->pattern_segments), saved_segment_4, 0 );
      }
      RogueString_List__remove_last( ROGUE_ARG(THIS->path_segments) );
    }
    else if (ROGUE_COND((RogueLogical__create__Int32( ROGUE_ARG(((THIS->options.flags) & (1))) ))))
    {
      RogueString_List__add__String( ROGUE_ARG(THIS->path_segments), filename_0 );
      RogueFile___listing__String__Function_String__( filepath_1, ROGUE_ARG(THIS->callback) );
      RogueString_List__remove_last( ROGUE_ARG(THIS->path_segments) );
    }
  }
  else if (ROGUE_COND(((RogueFileOptions__keeping_files( THIS->options )))))
  {
    RogueString_List__add__String( ROGUE_ARG(THIS->results), filepath_1 );
  }
}

RogueString* RogueFileListing__fix__String( RogueClassFileListing* THIS, RogueString* _auto_4027 )
{
  ROGUE_DEF_LOCAL_REF(RogueString*,pattern_0,_auto_4027);
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(((!!(pattern_0)) && (((RogueString__contains__String_Logical( pattern_0, Rogue_literal_strings[106], false ))))))))
  {
    return (RogueString*)(pattern_0);
  }
  ROGUE_DEF_LOCAL_REF(RogueString_List*,parts_1,(((RogueString_List*)(RogueString__split__String_Logical( pattern_0, Rogue_literal_strings[107], false )))));
  {
    ROGUE_DEF_LOCAL_REF(RogueString_List*,_auto_893_0,(parts_1));
    RogueInt32 index_0 = (0);
    RogueInt32 _auto_894_0 = (((_auto_893_0->count) - (1)));
    for (;ROGUE_COND(((index_0) <= (_auto_894_0)));++index_0)
    {
      ROGUE_GC_CHECK;
      ROGUE_DEF_LOCAL_REF(RogueString*,part_0,(((RogueString*)(_auto_893_0->data->as_objects[index_0]))));
      RogueString_List__set__Int32_String( parts_1, index_0, ROGUE_ARG(((RogueString*)(RogueString__replacing__String_String_Logical( part_0, Rogue_literal_strings[106], Rogue_literal_strings[108], false )))) );
    }
  }
  pattern_0 = ((RogueString*)(((RogueString*)(RogueString_List__join__String( parts_1, Rogue_literal_strings[107] )))));
  return (RogueString*)(pattern_0);
}

RogueClass_Function_String__* Rogue_Function_String____init_object( RogueClass_Function_String__* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  return (RogueClass_Function_String__*)(THIS);
}

RogueString* Rogue_Function_String____type_name( RogueClass_Function_String__* THIS )
{
  return (RogueString*)(Rogue_literal_strings[365]);
}

void Rogue_Function_String____call__String( RogueClass_Function_String__* THIS, RogueString* param1_0 )
{
}

RogueClassFileListing_collect_String_* RogueFileListing_collect_String___init_object( RogueClassFileListing_collect_String_* THIS )
{
  ROGUE_GC_CHECK;
  Rogue_Function_String____init_object( ROGUE_ARG(((RogueClass_Function_String__*)THIS)) );
  return (RogueClassFileListing_collect_String_*)(THIS);
}

RogueString* RogueFileListing_collect_String___type_name( RogueClassFileListing_collect_String_* THIS )
{
  return (RogueString*)(Rogue_literal_strings[437]);
}

void RogueFileListing_collect_String___call__String( RogueClassFileListing_collect_String_* THIS, RogueString* param1_0 )
{
  RogueFileListing__collect__String( ROGUE_ARG(THIS->context), param1_0 );
}

RogueClassFileListing_collect_String_* RogueFileListing_collect_String___init__FileListing( RogueClassFileListing_collect_String_* THIS, RogueClassFileListing* _auto_895_0 )
{
  ROGUE_GC_CHECK;
  THIS->context = _auto_895_0;
  return (RogueClassFileListing_collect_String_*)(THIS);
}

RogueClassFunction_896* RogueFunction_896__init_object( RogueClassFunction_896* THIS )
{
  ROGUE_GC_CHECK;
  Rogue_Function_String_RETURNSLogical___init_object( ROGUE_ARG(((RogueClass_Function_String_RETURNSLogical_*)THIS)) );
  return (RogueClassFunction_896*)(THIS);
}

RogueString* RogueFunction_896__type_name( RogueClassFunction_896* THIS )
{
  return (RogueString*)(Rogue_literal_strings[428]);
}

RogueLogical RogueFunction_896__call__String( RogueClassFunction_896* THIS, RogueString* p_0 )
{
  return (RogueLogical)(((((RogueString__contains__Character_Logical( p_0, (RogueCharacter)'*', false )))) || (((RogueString__contains__Character_Logical( p_0, (RogueCharacter)'?', false ))))));
}

RogueClassListRewriter_String_* RogueListRewriter_String___init_object( RogueClassListRewriter_String_* THIS )
{
  ROGUE_GC_CHECK;
  RogueGenericListRewriter__init_object( ROGUE_ARG(((RogueClassGenericListRewriter*)THIS)) );
  return (RogueClassListRewriter_String_*)(THIS);
}

RogueString* RogueListRewriter_String___type_name( RogueClassListRewriter_String_* THIS )
{
  return (RogueString*)(Rogue_literal_strings[438]);
}

RogueClassListRewriter_String_* RogueListRewriter_String___on_use( RogueClassListRewriter_String_* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueClassListRewriter_String_*)(THIS);
}

RogueException* RogueListRewriter_String___on_end_use__Exception( RogueClassListRewriter_String_* THIS, RogueException* err_0 )
{
  ROGUE_GC_CHECK;
  RogueListRewriter_String___finish( ROGUE_ARG(THIS) );
  if (ROGUE_COND(!(!!(RogueListRewriter_String__pool))))
  {
    RogueListRewriter_String__pool = ((RogueGenericListRewriter_List*)(RogueGenericListRewriter_List__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueGenericListRewriter_List*,ROGUE_CREATE_OBJECT(GenericListRewriter_List))) )));
  }
  RogueGenericListRewriter_List__add__GenericListRewriter( ROGUE_ARG(RogueListRewriter_String__pool), ROGUE_ARG(((RogueClassGenericListRewriter*)(THIS))) );
  return (RogueException*)(err_0);
}

void RogueListRewriter_String___write__String( RogueClassListRewriter_String_* THIS, RogueString* value_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((THIS->write_index) == (THIS->read_index))))
  {
    RogueString_List__reserve__Int32( ROGUE_ARG(THIS->list), 1 );
    RogueInt32 unread_count_1 = (((THIS->list->count) - (THIS->read_index)));
    RogueArray_set(THIS->list->data,((((RogueString_List__capacity( ROGUE_ARG(THIS->list) )))) - (unread_count_1)),((RogueArray*)(THIS->list->data)),THIS->read_index,unread_count_1);
    THIS->read_index += ((((RogueString_List__capacity( ROGUE_ARG(THIS->list) )))) - (THIS->list->count));
    THIS->list->count = ((RogueString_List__capacity( ROGUE_ARG(THIS->list) )));
  }
  if (ROGUE_COND(((THIS->write_index) == (THIS->list->count))))
  {
    RogueString_List__add__String( ROGUE_ARG(THIS->list), value_0 );
  }
  else
  {
    RogueString_List__set__Int32_String( ROGUE_ARG(THIS->list), ROGUE_ARG(THIS->write_index), value_0 );
  }
  ++THIS->write_index;
}

RogueClassListRewriter_String_* RogueListRewriter_String___init__String_List( RogueClassListRewriter_String_* THIS, RogueString_List* _auto_937_0 )
{
  ROGUE_GC_CHECK;
  THIS->list = _auto_937_0;
  return (RogueClassListRewriter_String_*)(THIS);
}

void RogueListRewriter_String___finish( RogueClassListRewriter_String_* THIS )
{
  ROGUE_GC_CHECK;
  RogueString_List__discard_from__Int32( ROGUE_ARG(THIS->list), ROGUE_ARG(THIS->write_index) );
}

RogueLogical RogueListRewriter_String___has_another( RogueClassListRewriter_String_* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((THIS->read_index) < (THIS->list->count))))
  {
    return (RogueLogical)(true);
  }
  else
  {
    RogueListRewriter_String___finish( ROGUE_ARG(THIS) );
    return (RogueLogical)(false);
  }
}

RogueString* RogueListRewriter_String___read( RogueClassListRewriter_String_* THIS )
{
  ROGUE_GC_CHECK;
  ++THIS->read_index;
  return (RogueString*)(((RogueString*)(THIS->list->data->as_objects[((THIS->read_index) - (1))])));
}

void RogueListRewriter_String___reset__String_List( RogueClassListRewriter_String_* THIS, RogueString_List* new_list_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!!(new_list_0)))
  {
    THIS->list = new_list_0;
  }
  THIS->read_index = 0;
  THIS->write_index = 0;
}

RogueClassGenericListRewriter* RogueGenericListRewriter__init_object( RogueClassGenericListRewriter* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  return (RogueClassGenericListRewriter*)(THIS);
}

RogueString* RogueGenericListRewriter__type_name( RogueClassGenericListRewriter* THIS )
{
  return (RogueString*)(Rogue_literal_strings[366]);
}

RogueGenericListRewriter_List* RogueGenericListRewriter_List__init_object( RogueGenericListRewriter_List* THIS )
{
  ROGUE_GC_CHECK;
  RogueGenericList__init_object( ROGUE_ARG(((RogueClassGenericList*)THIS)) );
  return (RogueGenericListRewriter_List*)(THIS);
}

RogueGenericListRewriter_List* RogueGenericListRewriter_List__init( RogueGenericListRewriter_List* THIS )
{
  ROGUE_GC_CHECK;
  RogueGenericListRewriter_List__init__Int32( ROGUE_ARG(THIS), 0 );
  return (RogueGenericListRewriter_List*)(THIS);
}

RogueString* RogueGenericListRewriter_List__to_String( RogueGenericListRewriter_List* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[293] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(((RogueString*)(RogueGenericListRewriter_List__join__String( ROGUE_ARG(THIS), Rogue_literal_strings[15] )))) ))) )))), Rogue_literal_strings[294] )))) ))));
}

RogueString* RogueGenericListRewriter_List__type_name( RogueGenericListRewriter_List* THIS )
{
  return (RogueString*)(Rogue_literal_strings[381]);
}

RogueGenericListRewriter_List* RogueGenericListRewriter_List__init__Int32( RogueGenericListRewriter_List* THIS, RogueInt32 initial_capacity_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((initial_capacity_0) != (((RogueGenericListRewriter_List__capacity( ROGUE_ARG(THIS) )))))))
  {
    THIS->data = RogueType_create_array( initial_capacity_0, sizeof(RogueClassGenericListRewriter*), true, 91 );
  }
  RogueGenericListRewriter_List__clear( ROGUE_ARG(THIS) );
  return (RogueGenericListRewriter_List*)(THIS);
}

void RogueGenericListRewriter_List__add__GenericListRewriter( RogueGenericListRewriter_List* THIS, RogueClassGenericListRewriter* value_0 )
{
  ROGUE_GC_CHECK;
  RogueGenericListRewriter_List__set__Int32_GenericListRewriter( ROGUE_ARG(((RogueGenericListRewriter_List*)(RogueGenericListRewriter_List__reserve__Int32( ROGUE_ARG(THIS), 1 )))), ROGUE_ARG(THIS->count), value_0 );
  THIS->count = ((THIS->count) + (1));
}

RogueInt32 RogueGenericListRewriter_List__capacity( RogueGenericListRewriter_List* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(!!(THIS->data))))
  {
    return (RogueInt32)(0);
  }
  return (RogueInt32)(THIS->data->count);
}

void RogueGenericListRewriter_List__clear( RogueGenericListRewriter_List* THIS )
{
  ROGUE_GC_CHECK;
  RogueGenericListRewriter_List__discard_from__Int32( ROGUE_ARG(THIS), 0 );
}

void RogueGenericListRewriter_List__discard_from__Int32( RogueGenericListRewriter_List* THIS, RogueInt32 index_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!!(THIS->data)))
  {
    RogueInt32 n_1 = (((THIS->count) - (index_0)));
    if (ROGUE_COND(((n_1) > (0))))
    {
      RogueArray__zero__Int32_Int32( ROGUE_ARG(((RogueArray*)THIS->data)), index_0, n_1 );
      THIS->count = index_0;
    }
  }
}

RogueClassGenericListRewriter* RogueGenericListRewriter_List__get__Int32( RogueGenericListRewriter_List* THIS, RogueInt32 index_0 )
{
  return (RogueClassGenericListRewriter*)(((RogueClassGenericListRewriter*)(THIS->data->as_objects[index_0])));
}

RogueString* RogueGenericListRewriter_List__join__String( RogueGenericListRewriter_List* THIS, RogueString* separator_0 )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueStringBuilder*,builder_1,(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))));
  RogueLogical first_2 = (true);
  {
    ROGUE_DEF_LOCAL_REF(RogueGenericListRewriter_List*,_auto_2634_0,(THIS));
    RogueInt32 _auto_2635_0 = (0);
    RogueInt32 _auto_2636_0 = (((_auto_2634_0->count) - (1)));
    for (;ROGUE_COND(((_auto_2635_0) <= (_auto_2636_0)));++_auto_2635_0)
    {
      ROGUE_GC_CHECK;
      ROGUE_DEF_LOCAL_REF(RogueClassGenericListRewriter*,item_0,(((RogueClassGenericListRewriter*)(_auto_2634_0->data->as_objects[_auto_2635_0]))));
      if (ROGUE_COND(first_2))
      {
        first_2 = ((RogueLogical)(false));
      }
      else
      {
        RogueStringBuilder__print__String( builder_1, separator_0 );
      }
      RogueStringBuilder__print__Object( builder_1, ROGUE_ARG(((RogueObject*)(item_0))) );
    }
  }
  return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( builder_1 ))));
}

RogueGenericListRewriter_List* RogueGenericListRewriter_List__reserve__Int32( RogueGenericListRewriter_List* THIS, RogueInt32 additional_elements_0 )
{
  ROGUE_GC_CHECK;
  RogueInt32 required_capacity_1 = (((THIS->count) + (additional_elements_0)));
  if (ROGUE_COND(((required_capacity_1) == (0))))
  {
    return (RogueGenericListRewriter_List*)(THIS);
  }
  if (ROGUE_COND(!(!!(THIS->data))))
  {
    if (ROGUE_COND(((required_capacity_1) == (1))))
    {
      required_capacity_1 = ((RogueInt32)(10));
    }
    THIS->data = RogueType_create_array( required_capacity_1, sizeof(RogueClassGenericListRewriter*), true, 91 );
  }
  else if (ROGUE_COND(((required_capacity_1) > (THIS->data->count))))
  {
    RogueInt32 cap_2 = (((RogueGenericListRewriter_List__capacity( ROGUE_ARG(THIS) ))));
    if (ROGUE_COND(((required_capacity_1) < (((cap_2) + (cap_2))))))
    {
      required_capacity_1 = ((RogueInt32)(((cap_2) + (cap_2))));
    }
    ROGUE_DEF_LOCAL_REF(RogueArray*,new_data_3,(RogueType_create_array( required_capacity_1, sizeof(RogueClassGenericListRewriter*), true, 91 )));
    RogueArray_set(new_data_3,0,((RogueArray*)(THIS->data)),0,-1);
    THIS->data = new_data_3;
  }
  return (RogueGenericListRewriter_List*)(THIS);
}

RogueClassGenericListRewriter* RogueGenericListRewriter_List__remove_at__Int32( RogueGenericListRewriter_List* THIS, RogueInt32 index_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((RogueLogical)((((unsigned int)index_0) >= (unsigned int)THIS->count)))))
  {
    throw ((RogueException*)(RogueOutOfBoundsError___throw( ROGUE_ARG(((RogueClassOutOfBoundsError*)(RogueOutOfBoundsError__init__Int32_Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassOutOfBoundsError*,ROGUE_CREATE_OBJECT(OutOfBoundsError))), index_0, ROGUE_ARG(THIS->count) )))) )));
  }
  ROGUE_DEF_LOCAL_REF(RogueClassGenericListRewriter*,result_1,(((RogueClassGenericListRewriter*)(THIS->data->as_objects[index_0]))));
  RogueArray_set(THIS->data,index_0,((RogueArray*)(THIS->data)),((index_0) + (1)),-1);
  ROGUE_DEF_LOCAL_REF(RogueClassGenericListRewriter*,zero_value_2,0);
  THIS->count = ((THIS->count) + (-1));
  THIS->data->as_objects[THIS->count] = zero_value_2;
  return (RogueClassGenericListRewriter*)(result_1);
}

RogueClassGenericListRewriter* RogueGenericListRewriter_List__remove_last( RogueGenericListRewriter_List* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueClassGenericListRewriter*)(((RogueClassGenericListRewriter*)(RogueGenericListRewriter_List__remove_at__Int32( ROGUE_ARG(THIS), ROGUE_ARG(((THIS->count) - (1))) ))));
}

void RogueGenericListRewriter_List__set__Int32_GenericListRewriter( RogueGenericListRewriter_List* THIS, RogueInt32 index_0, RogueClassGenericListRewriter* new_value_1 )
{
  ROGUE_GC_CHECK;
  THIS->data->as_objects[index_0] = new_value_1;
}

RogueString* RogueArray_GenericListRewriter___type_name( RogueArray* THIS )
{
  return (RogueString*)(Rogue_literal_strings[392]);
}

RogueClass_Function_String_String_RETURNSLogical_* Rogue_Function_String_String_RETURNSLogical___init_object( RogueClass_Function_String_String_RETURNSLogical_* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  return (RogueClass_Function_String_String_RETURNSLogical_*)(THIS);
}

RogueString* Rogue_Function_String_String_RETURNSLogical___type_name( RogueClass_Function_String_String_RETURNSLogical_* THIS )
{
  return (RogueString*)(Rogue_literal_strings[367]);
}

RogueLogical Rogue_Function_String_String_RETURNSLogical___call__String_String( RogueClass_Function_String_String_RETURNSLogical_* THIS, RogueString* param1_0, RogueString* param2_1 )
{
  return (RogueLogical)(false);
}

RogueClassFunction_948* RogueFunction_948__init_object( RogueClassFunction_948* THIS )
{
  ROGUE_GC_CHECK;
  Rogue_Function_String_String_RETURNSLogical___init_object( ROGUE_ARG(((RogueClass_Function_String_String_RETURNSLogical_*)THIS)) );
  return (RogueClassFunction_948*)(THIS);
}

RogueString* RogueFunction_948__type_name( RogueClassFunction_948* THIS )
{
  return (RogueString*)(Rogue_literal_strings[439]);
}

RogueLogical RogueFunction_948__call__String_String( RogueClassFunction_948* THIS, RogueString* a_0, RogueString* b_1 )
{
  return (RogueLogical)(((((RogueString__compare_to__String_Logical( a_0, b_1, true )))) < (0)));
}

RogueClassQuicksort_String_* RogueQuicksort_String___init_object( RogueClassQuicksort_String_* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  return (RogueClassQuicksort_String_*)(THIS);
}

RogueString* RogueQuicksort_String___type_name( RogueClassQuicksort_String_* THIS )
{
  return (RogueString*)(Rogue_literal_strings[368]);
}

RogueClassFunction_950* RogueFunction_950__init_object( RogueClassFunction_950* THIS )
{
  ROGUE_GC_CHECK;
  Rogue_Function_String_String_RETURNSLogical___init_object( ROGUE_ARG(((RogueClass_Function_String_String_RETURNSLogical_*)THIS)) );
  return (RogueClassFunction_950*)(THIS);
}

RogueString* RogueFunction_950__type_name( RogueClassFunction_950* THIS )
{
  return (RogueString*)(Rogue_literal_strings[440]);
}

RogueLogical RogueFunction_950__call__String_String( RogueClassFunction_950* THIS, RogueString* a_0, RogueString* b_1 )
{
  return (RogueLogical)(((((RogueVersionNumber__operatorLTGT__String( RogueClassVersionNumber( a_0, true ), b_1 )))) > (0)));
}

RogueClassPlatforms* RoguePlatforms__init_object( RogueClassPlatforms* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  return (RogueClassPlatforms*)(THIS);
}

RogueString* RoguePlatforms__to_String( RogueClassPlatforms* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(THIS->combined);
}

RogueString* RoguePlatforms__type_name( RogueClassPlatforms* THIS )
{
  return (RogueString*)(Rogue_literal_strings[369]);
}

RogueClassPlatforms* RoguePlatforms__init__String_Logical_Logical_Logical( RogueClassPlatforms* THIS, RogueString* _auto_954_0, RogueLogical windows_1, RogueLogical macos_2, RogueLogical linux_3 )
{
  ROGUE_GC_CHECK;
  THIS->combined = _auto_954_0;
  if (ROGUE_COND(!(!!(THIS->combined))))
  {
    THIS->combined = Rogue_literal_strings[0];
  }
  if (ROGUE_COND(((windows_1) && (!(((RogueString__contains__Character_Logical( ROGUE_ARG(THIS->combined), (RogueCharacter)'w', false ))))))))
  {
    THIS->combined = ((RogueString*)(RogueString__operatorPLUS__Character( ROGUE_ARG(THIS->combined), (RogueCharacter)'w' )));
  }
  if (ROGUE_COND(((macos_2) && (!(((RogueString__contains__Character_Logical( ROGUE_ARG(THIS->combined), (RogueCharacter)'m', false ))))))))
  {
    THIS->combined = ((RogueString*)(RogueString__operatorPLUS__Character( ROGUE_ARG(THIS->combined), (RogueCharacter)'m' )));
  }
  if (ROGUE_COND(((linux_3) && (!(((RogueString__contains__Character_Logical( ROGUE_ARG(THIS->combined), (RogueCharacter)'l', false ))))))))
  {
    THIS->combined = ((RogueString*)(RogueString__operatorPLUS__Character( ROGUE_ARG(THIS->combined), (RogueCharacter)'l' )));
  }
  return (RogueClassPlatforms*)(THIS);
}

RogueClassPlatforms* RoguePlatforms__operatorPLUS__Platforms( RogueClassPlatforms* THIS, RogueClassPlatforms* other_0 )
{
  ROGUE_GC_CHECK;
  return (RogueClassPlatforms*)(((RogueClassPlatforms*)(RoguePlatforms__operatorOR__Platforms( ROGUE_ARG(THIS), other_0 ))));
}

RogueClassPlatforms* RoguePlatforms__operatorOR__Platforms( RogueClassPlatforms* THIS, RogueClassPlatforms* other_0 )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueString*,combo_1,(THIS->combined));
  {
    ROGUE_DEF_LOCAL_REF(RogueString*,_auto_979_0,(other_0->combined));
    RogueInt32 _auto_980_0 = (0);
    RogueInt32 _auto_981_0 = (((((RogueString__count( _auto_979_0 )))) - (1)));
    for (;ROGUE_COND(((_auto_980_0) <= (_auto_981_0)));++_auto_980_0)
    {
      ROGUE_GC_CHECK;
      RogueCharacter ch_0 = (((RogueString__get__Int32( _auto_979_0, _auto_980_0 ))));
      if (ROGUE_COND(!(((RogueString__contains__Character_Logical( combo_1, ch_0, false ))))))
      {
        combo_1 = ((RogueString*)(((RogueString*)(RogueString__operatorPLUS__Character( combo_1, ch_0 )))));
      }
    }
  }
  return (RogueClassPlatforms*)(((RogueClassPlatforms*)(RoguePlatforms__init__String_Logical_Logical_Logical( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassPlatforms*,ROGUE_CREATE_OBJECT(Platforms))), combo_1, false, false, false ))));
}

RogueClassFunction_983* RogueFunction_983__init_object( RogueClassFunction_983* THIS )
{
  ROGUE_GC_CHECK;
  Rogue_Function_String_String_RETURNSLogical___init_object( ROGUE_ARG(((RogueClass_Function_String_String_RETURNSLogical_*)THIS)) );
  return (RogueClassFunction_983*)(THIS);
}

RogueString* RogueFunction_983__type_name( RogueClassFunction_983* THIS )
{
  return (RogueString*)(Rogue_literal_strings[441]);
}

RogueLogical RogueFunction_983__call__String_String( RogueClassFunction_983* THIS, RogueString* a_0, RogueString* b_1 )
{
  return (RogueLogical)(((((RogueVersionNumber__operatorLTGT__String( RogueClassVersionNumber( a_0, true ), b_1 )))) > (0)));
}

RogueClassFunction_999* RogueFunction_999__init_object( RogueClassFunction_999* THIS )
{
  ROGUE_GC_CHECK;
  Rogue_Function_String_String_RETURNSLogical___init_object( ROGUE_ARG(((RogueClass_Function_String_String_RETURNSLogical_*)THIS)) );
  return (RogueClassFunction_999*)(THIS);
}

RogueString* RogueFunction_999__type_name( RogueClassFunction_999* THIS )
{
  return (RogueString*)(Rogue_literal_strings[442]);
}

RogueLogical RogueFunction_999__call__String_String( RogueClassFunction_999* THIS, RogueString* a_0, RogueString* b_1 )
{
  return (RogueLogical)(((((RogueVersionNumber__operatorLTGT__String( RogueClassVersionNumber( a_0, true ), b_1 )))) > (0)));
}

RogueClassFunction_1003* RogueFunction_1003__init_object( RogueClassFunction_1003* THIS )
{
  ROGUE_GC_CHECK;
  Rogue_Function_String_String_RETURNSLogical___init_object( ROGUE_ARG(((RogueClass_Function_String_String_RETURNSLogical_*)THIS)) );
  return (RogueClassFunction_1003*)(THIS);
}

RogueString* RogueFunction_1003__type_name( RogueClassFunction_1003* THIS )
{
  return (RogueString*)(Rogue_literal_strings[443]);
}

RogueLogical RogueFunction_1003__call__String_String( RogueClassFunction_1003* THIS, RogueString* a_0, RogueString* b_1 )
{
  return (RogueLogical)(((((RogueVersionNumber__operatorLTGT__String( RogueClassVersionNumber( a_0, true ), b_1 )))) > (0)));
}

RogueClassZip* RogueZip__init_object( RogueClassZip* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  return (RogueClassZip*)(THIS);
}

RogueString* RogueZip__description( RogueClassZip* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(THIS->filepath);
}

RogueString* RogueZip__type_name( RogueClassZip* THIS )
{
  return (RogueString*)(Rogue_literal_strings[370]);
}

RogueClassZip* RogueZip__init__File_Int32( RogueClassZip* THIS, RogueClassFile* file_0, RogueInt32 compression_1 )
{
  ROGUE_GC_CHECK;
  RogueZip__open__String_Int32_Int32( ROGUE_ARG(THIS), ROGUE_ARG(file_0->filepath), compression_1, 48 );
  return (RogueClassZip*)(THIS);
}

void RogueZip__close( RogueClassZip* THIS )
{
  ROGUE_GC_CHECK;
  if (THIS->zip)
  {
    zip_close( THIS->zip );
    THIS->zip = 0;
  }

  THIS->mode = 48;
}

RogueInt32 RogueZip__count( RogueClassZip* THIS )
{
  ROGUE_GC_CHECK;
  RogueZip__set_mode__Int32( ROGUE_ARG(THIS), 114 );
  return (RogueInt32)(((RogueInt32)(zip_entries_total(THIS->zip))));
}

void RogueZip__extract__File_Logical( RogueClassZip* THIS, RogueClassFile* folder_0, RogueLogical verbose_1 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(((RogueFile__is_folder( folder_0 ))))))
  {
    throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(((RogueException*)Rogue_call_ROGUEM4( 13, ROGUE_ARG(((RogueException*)ROGUE_CREATE_REF(RogueClassError*,ROGUE_CREATE_OBJECT(Error)))), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[167] )))), ROGUE_ARG(((RogueString*)(RogueString__operatorPLUS__Object( ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], Rogue_literal_strings[0] ))), ROGUE_ARG(((RogueObject*)(folder_0))) )))) )))) )))) ))))) ));
  }
  {
    ROGUE_DEF_LOCAL_REF(RogueClassZip*,_auto_1016_0,(THIS));
    RogueInt32 _auto_1017_0 = (0);
    RogueInt32 _auto_1018_0 = (((((RogueZip__count( _auto_1016_0 )))) - (1)));
    for (;ROGUE_COND(((_auto_1017_0) <= (_auto_1018_0)));++_auto_1017_0)
    {
      ROGUE_GC_CHECK;
      RogueClassZipEntry entry_0 = (((RogueZip__get__Int32( _auto_1016_0, _auto_1017_0 ))));
      if (ROGUE_COND(verbose_1))
      {
        RogueGlobal__println( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__String( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[168] )))), ROGUE_ARG(((RogueString*)(RogueString__operatorPLUS__Object( ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], Rogue_literal_strings[0] ))), ROGUE_ARG(((RogueObject*)(((RogueClassFile*)(RogueFile__operatorSLASH__String( folder_0, ROGUE_ARG(entry_0.name) )))))) )))) )))) )))) )))) );
      }
      RogueZipEntry__extract__File( entry_0, folder_0 );
    }
  }
}

RogueClassZipEntry RogueZip__get__Int32( RogueClassZip* THIS, RogueInt32 index_0 )
{
  ROGUE_GC_CHECK;
  RogueZip__set_mode__Int32( ROGUE_ARG(THIS), 114 );
  ROGUE_DEF_LOCAL_REF(RogueString*,name_1,0);
  RogueLogical is_folder_2 = 0;
  RogueInt64 size_3 = 0;
  RogueInt32 crc32_4 = 0;
  zip_entry_openbyindex( THIS->zip, index_0 );
  name_1 = RogueString_create_from_utf8( zip_entry_name(THIS->zip), -1 );
  is_folder_2 = (RogueLogical) zip_entry_isdir( THIS->zip );
  size_3 = (RogueInt64) zip_entry_size( THIS->zip );
  crc32_4 = (RogueInt32) zip_entry_crc32( THIS->zip );
  zip_entry_close( THIS->zip );

  return (RogueClassZipEntry)(RogueClassZipEntry( THIS, name_1, is_folder_2, size_3, crc32_4 ));
}

void RogueZip__on_cleanup( RogueClassZip* THIS )
{
  ROGUE_GC_CHECK;
  RogueZip__close( ROGUE_ARG(THIS) );
}

void RogueZip__open__String_Int32_Int32( RogueClassZip* THIS, RogueString* _auto_1015_0, RogueInt32 _auto_1014_1, RogueInt32 mode_2 )
{
  ROGUE_GC_CHECK;
  THIS->filepath = _auto_1015_0;
  RogueZip__set_compression__Int32( ROGUE_ARG(THIS), _auto_1014_1 );
  THIS->filepath = (RogueFile__expand_path__String( ROGUE_ARG(THIS->filepath) ));
  RogueZip__close( ROGUE_ARG(THIS) );
  if (ROGUE_COND(((mode_2) == (48))))
  {
    return;
  }
  THIS->mode = mode_2;
  THIS->zip = zip_open( THIS->filepath->utf8, (int)THIS->compression, (char)mode_2 );

  if (ROGUE_COND(((RogueLogical)(!THIS->zip))))
  {
    throw ((RogueException*)Rogue_call_ROGUEM0( 16, ROGUE_ARG(((RogueClassError*)(((RogueException*)Rogue_call_ROGUEM4( 13, ROGUE_ARG(((RogueException*)ROGUE_CREATE_REF(RogueClassError*,ROGUE_CREATE_OBJECT(Error)))), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[166] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(THIS->filepath) ))) )))), Rogue_literal_strings[97] )))) )))) ))))) ));
  }
}

void RogueZip__set_compression__Int32( RogueClassZip* THIS, RogueInt32 new_compression_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((THIS->compression) != (new_compression_0))))
  {
    RogueZip__close( ROGUE_ARG(THIS) );
    THIS->compression = new_compression_0;
  }
}

void RogueZip__set_mode__Int32( RogueClassZip* THIS, RogueInt32 new_mode_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((THIS->mode) != (new_mode_0))))
  {
    if (ROGUE_COND(((new_mode_0) == (48))))
    {
      RogueZip__close( ROGUE_ARG(THIS) );
    }
    else
    {
      RogueZip__open__String_Int32_Int32( ROGUE_ARG(THIS), ROGUE_ARG(THIS->filepath), ROGUE_ARG(THIS->compression), new_mode_0 );
    }
    THIS->mode = new_mode_0;
  }
}

RogueClassFunction_1033* RogueFunction_1033__init_object( RogueClassFunction_1033* THIS )
{
  ROGUE_GC_CHECK;
  Rogue_Function_String_RETURNSLogical___init_object( ROGUE_ARG(((RogueClass_Function_String_RETURNSLogical_*)THIS)) );
  return (RogueClassFunction_1033*)(THIS);
}

RogueString* RogueFunction_1033__type_name( RogueClassFunction_1033* THIS )
{
  return (RogueString*)(Rogue_literal_strings[429]);
}

RogueLogical RogueFunction_1033__call__String( RogueClassFunction_1033* THIS, RogueString* f_0 )
{
  return (RogueLogical)(((RogueString__ends_with__String_Logical( f_0, Rogue_literal_strings[199], true ))));
}

RogueClassFunction_1039* RogueFunction_1039__init_object( RogueClassFunction_1039* THIS )
{
  ROGUE_GC_CHECK;
  Rogue_Function_String_RETURNSLogical___init_object( ROGUE_ARG(((RogueClass_Function_String_RETURNSLogical_*)THIS)) );
  return (RogueClassFunction_1039*)(THIS);
}

RogueString* RogueFunction_1039__type_name( RogueClassFunction_1039* THIS )
{
  return (RogueString*)(Rogue_literal_strings[430]);
}

RogueLogical RogueFunction_1039__call__String( RogueClassFunction_1039* THIS, RogueString* f_0 )
{
  return (RogueLogical)(((RogueString__ends_with__String_Logical( f_0, ROGUE_ARG((RogueSystem__os())), true ))));
}

RogueClassFunction_1040* RogueFunction_1040__init_object( RogueClassFunction_1040* THIS )
{
  ROGUE_GC_CHECK;
  Rogue_Function_String_RETURNSLogical___init_object( ROGUE_ARG(((RogueClass_Function_String_RETURNSLogical_*)THIS)) );
  return (RogueClassFunction_1040*)(THIS);
}

RogueString* RogueFunction_1040__type_name( RogueClassFunction_1040* THIS )
{
  return (RogueString*)(Rogue_literal_strings[431]);
}

RogueLogical RogueFunction_1040__call__String( RogueClassFunction_1040* THIS, RogueString* f_0 )
{
  return (RogueLogical)(((RogueString__ends_with__String_Logical( f_0, ROGUE_ARG((RogueSystem__os())), true ))));
}

RogueClassFunction_1041* RogueFunction_1041__init_object( RogueClassFunction_1041* THIS )
{
  ROGUE_GC_CHECK;
  Rogue_Function_String_RETURNSLogical___init_object( ROGUE_ARG(((RogueClass_Function_String_RETURNSLogical_*)THIS)) );
  return (RogueClassFunction_1041*)(THIS);
}

RogueString* RogueFunction_1041__type_name( RogueClassFunction_1041* THIS )
{
  return (RogueString*)(Rogue_literal_strings[432]);
}

RogueLogical RogueFunction_1041__call__String( RogueClassFunction_1041* THIS, RogueString* f_0 )
{
  return (RogueLogical)((RogueString__operatorEQUALSEQUALS__String_String( ROGUE_ARG((RogueFile__filename__String( f_0 ))), ROGUE_ARG(THIS->app_name) )));
}

RogueClassFunction_1041* RogueFunction_1041__init__String( RogueClassFunction_1041* THIS, RogueString* _auto_1042_0 )
{
  ROGUE_GC_CHECK;
  THIS->app_name = _auto_1042_0;
  return (RogueClassFunction_1041*)(THIS);
}

RogueClassFunction_1043* RogueFunction_1043__init_object( RogueClassFunction_1043* THIS )
{
  ROGUE_GC_CHECK;
  Rogue_Function_String_RETURNSLogical___init_object( ROGUE_ARG(((RogueClass_Function_String_RETURNSLogical_*)THIS)) );
  return (RogueClassFunction_1043*)(THIS);
}

RogueString* RogueFunction_1043__type_name( RogueClassFunction_1043* THIS )
{
  return (RogueString*)(Rogue_literal_strings[433]);
}

RogueLogical RogueFunction_1043__call__String( RogueClassFunction_1043* THIS, RogueString* f_0 )
{
  return (RogueLogical)((RogueString__operatorEQUALSEQUALS__String_String( ROGUE_ARG((RogueFile__filename__String( f_0 ))), ROGUE_ARG(THIS->app_name) )));
}

RogueClassFunction_1043* RogueFunction_1043__init__String( RogueClassFunction_1043* THIS, RogueString* _auto_1044_0 )
{
  ROGUE_GC_CHECK;
  THIS->app_name = _auto_1044_0;
  return (RogueClassFunction_1043*)(THIS);
}

RogueClassExtendedASCIIReader* RogueExtendedASCIIReader__init_object( RogueClassExtendedASCIIReader* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  return (RogueClassExtendedASCIIReader*)(THIS);
}

RogueString* RogueExtendedASCIIReader__to_String( RogueClassExtendedASCIIReader* THIS )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueCharacter_List*,buffer_0,(((RogueCharacter_List*)(RogueCharacter_List__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueCharacter_List*,ROGUE_CREATE_OBJECT(Character_List))) )))));
  while (ROGUE_COND(((RogueExtendedASCIIReader__has_another( ROGUE_ARG(THIS) )))))
  {
    ROGUE_GC_CHECK;
    RogueCharacter_List__add__Character( buffer_0, ROGUE_ARG(((RogueExtendedASCIIReader__read( ROGUE_ARG(THIS) )))) );
  }
  if (ROGUE_COND((false)))
  {
  }
  else
  {
    return (RogueString*)(((RogueString*)(RogueCharacter_List__to_String( buffer_0 ))));
  }
}

RogueString* RogueExtendedASCIIReader__type_name( RogueClassExtendedASCIIReader* THIS )
{
  return (RogueString*)(Rogue_literal_strings[371]);
}

RogueLogical RogueExtendedASCIIReader__has_another( RogueClassExtendedASCIIReader* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(((THIS->next.exists) || ((RogueReader_Byte___has_another( ROGUE_ARG(((((RogueObject*)THIS->byte_reader)))) )))));
}

RogueCharacter RogueExtendedASCIIReader__peek( RogueClassExtendedASCIIReader* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(THIS->next.exists))
  {
    return (RogueCharacter)(THIS->next.value);
  }
  if (ROGUE_COND(!((RogueReader_Byte___has_another( ROGUE_ARG(((((RogueObject*)THIS->byte_reader)))) )))))
  {
    return (RogueCharacter)((RogueCharacter)0);
  }
  RogueCharacter ch_0 = ((RogueCharacter__create__Byte( ROGUE_ARG((RogueReader_Byte___read( ROGUE_ARG(((((RogueObject*)THIS->byte_reader)))) ))) )));
  THIS->next = RogueOptionalCharacter( ch_0, true );
  return (RogueCharacter)(ch_0);
}

RogueCharacter RogueExtendedASCIIReader__read( RogueClassExtendedASCIIReader* THIS )
{
  ROGUE_GC_CHECK;
  RogueCharacter result_0 = (((RogueExtendedASCIIReader__peek( ROGUE_ARG(THIS) ))));
  THIS->next = (RogueOptionalCharacter__create());
  THIS->position = ((THIS->position) + (1));
  return (RogueCharacter)(result_0);
}

RogueClassExtendedASCIIReader* RogueExtendedASCIIReader__init__Reader_Byte_( RogueClassExtendedASCIIReader* THIS, RogueClassReader_Byte_* _auto_1070_0 )
{
  ROGUE_GC_CHECK;
  THIS->byte_reader = _auto_1070_0;
  THIS->next = (RogueOptionalCharacter__create());
  return (RogueClassExtendedASCIIReader*)(THIS);
}

RogueClassUTF8Reader* RogueUTF8Reader__init_object( RogueClassUTF8Reader* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  return (RogueClassUTF8Reader*)(THIS);
}

RogueString* RogueUTF8Reader__to_String( RogueClassUTF8Reader* THIS )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueCharacter_List*,buffer_0,(((RogueCharacter_List*)(RogueCharacter_List__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueCharacter_List*,ROGUE_CREATE_OBJECT(Character_List))) )))));
  while (ROGUE_COND(((RogueUTF8Reader__has_another( ROGUE_ARG(THIS) )))))
  {
    ROGUE_GC_CHECK;
    RogueCharacter_List__add__Character( buffer_0, ROGUE_ARG(((RogueUTF8Reader__read( ROGUE_ARG(THIS) )))) );
  }
  if (ROGUE_COND((false)))
  {
  }
  else
  {
    return (RogueString*)(((RogueString*)(RogueCharacter_List__to_String( buffer_0 ))));
  }
}

RogueString* RogueUTF8Reader__type_name( RogueClassUTF8Reader* THIS )
{
  return (RogueString*)(Rogue_literal_strings[372]);
}

RogueLogical RogueUTF8Reader__has_another( RogueClassUTF8Reader* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(((THIS->next.exists) || ((RogueReader_Byte___has_another( ROGUE_ARG(((((RogueObject*)THIS->byte_reader)))) )))));
}

RogueCharacter RogueUTF8Reader__peek( RogueClassUTF8Reader* THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(THIS->next.exists))
  {
    return (RogueCharacter)(THIS->next.value);
  }
  if (ROGUE_COND(!((RogueReader_Byte___has_another( ROGUE_ARG(((((RogueObject*)THIS->byte_reader)))) )))))
  {
    return (RogueCharacter)((RogueCharacter)0);
  }
  RogueCharacter ch_0 = (((RogueCharacter)((RogueReader_Byte___read( ROGUE_ARG(((((RogueObject*)THIS->byte_reader)))) )))));
  if (ROGUE_COND(!!(((((RogueInt32)(ch_0))) & (128)))))
  {
    if (ROGUE_COND(!!(((((RogueInt32)(ch_0))) & (32)))))
    {
      if (ROGUE_COND(!!(((((RogueInt32)(ch_0))) & (16)))))
      {
        ch_0 = ((RogueCharacter)(((RogueCharacter)(((((((((RogueInt32)(ch_0))) & (7))) << (18))) | (((((((RogueInt32)((RogueReader_Byte___read( ROGUE_ARG(((((RogueObject*)THIS->byte_reader)))) ))))) & (63))) << (12))))))));
        ch_0 |= ((((((RogueInt32)((RogueReader_Byte___read( ROGUE_ARG(((((RogueObject*)THIS->byte_reader)))) ))))) & (63))) << (6));
        ch_0 |= ((((RogueInt32)((RogueReader_Byte___read( ROGUE_ARG(((((RogueObject*)THIS->byte_reader)))) ))))) & (63));
      }
      else
      {
        ch_0 = ((RogueCharacter)(((RogueCharacter)(((((((((RogueInt32)(ch_0))) & (15))) << (12))) | (((((((RogueInt32)((RogueReader_Byte___read( ROGUE_ARG(((((RogueObject*)THIS->byte_reader)))) ))))) & (63))) << (6))))))));
        ch_0 |= ((((RogueInt32)((RogueReader_Byte___read( ROGUE_ARG(((((RogueObject*)THIS->byte_reader)))) ))))) & (63));
      }
    }
    else
    {
      ch_0 = ((RogueCharacter)(((RogueCharacter)(((((((((RogueInt32)(ch_0))) & (31))) << (6))) | (((((RogueInt32)((RogueReader_Byte___read( ROGUE_ARG(((((RogueObject*)THIS->byte_reader)))) ))))) & (63))))))));
    }
  }
  THIS->next = RogueOptionalCharacter( ch_0, true );
  return (RogueCharacter)(ch_0);
}

RogueCharacter RogueUTF8Reader__read( RogueClassUTF8Reader* THIS )
{
  ROGUE_GC_CHECK;
  RogueCharacter result_0 = (((RogueUTF8Reader__peek( ROGUE_ARG(THIS) ))));
  THIS->next = (RogueOptionalCharacter__create());
  THIS->position = ((THIS->position) + (1));
  return (RogueCharacter)(result_0);
}

RogueClassUTF8Reader* RogueUTF8Reader__init__Reader_Byte_( RogueClassUTF8Reader* THIS, RogueClassReader_Byte_* _auto_1073_0 )
{
  ROGUE_GC_CHECK;
  THIS->byte_reader = _auto_1073_0;
  THIS->next = (RogueOptionalCharacter__create());
  if (ROGUE_COND(((((RogueInt32)(((RogueUTF8Reader__peek( ROGUE_ARG(THIS) )))))) == (65279))))
  {
    RogueUTF8Reader__read( ROGUE_ARG(THIS) );
  }
  return (RogueClassUTF8Reader*)(THIS);
}

RogueClassFunction_1088* RogueFunction_1088__init_object( RogueClassFunction_1088* THIS )
{
  ROGUE_GC_CHECK;
  Rogue_Function_Value_RETURNSLogical___init_object( ROGUE_ARG(((RogueClass_Function_Value_RETURNSLogical_*)THIS)) );
  return (RogueClassFunction_1088*)(THIS);
}

RogueString* RogueFunction_1088__type_name( RogueClassFunction_1088* THIS )
{
  return (RogueString*)(Rogue_literal_strings[418]);
}

RogueLogical RogueFunction_1088__call__Value( RogueClassFunction_1088* THIS, RogueClassValue* value_0 )
{
  return (RogueLogical)(((RogueString__equals__String_Logical( ROGUE_ARG(((RogueString*)Rogue_call_ROGUEM0( 8, ROGUE_ARG(((RogueObject*)((RogueClassValue*)Rogue_call_ROGUEM4( 41, value_0, Rogue_literal_strings[224] )))) ))), Rogue_literal_strings[419], true ))));
}

RogueWeakReference* RogueWeakReference__init_object( RogueWeakReference* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  return (RogueWeakReference*)(THIS);
}

RogueString* RogueWeakReference__type_name( RogueWeakReference* THIS )
{
  return (RogueString*)(Rogue_literal_strings[374]);
}

void RogueWeakReference__on_cleanup( RogueWeakReference* THIS )
{
  ROGUE_GC_CHECK;
  if (Rogue_weak_references == THIS)
  {
    Rogue_weak_references = THIS->next_weak_reference;
  }
  else
  {
    RogueWeakReference* cur = Rogue_weak_references;
    while (cur && cur->next_weak_reference != THIS)
    {
      cur = cur->next_weak_reference;
    }
    if (cur) cur->next_weak_reference = cur->next_weak_reference->next_weak_reference;
  }

}

RogueClassFunction_2437* RogueFunction_2437__init_object( RogueClassFunction_2437* THIS )
{
  ROGUE_GC_CHECK;
  Rogue_Function_____init_object( ROGUE_ARG(((RogueClass_Function___*)THIS)) );
  return (RogueClassFunction_2437*)(THIS);
}

RogueString* RogueFunction_2437__type_name( RogueClassFunction_2437* THIS )
{
  return (RogueString*)(Rogue_literal_strings[396]);
}

void RogueFunction_2437__call( RogueClassFunction_2437* THIS )
{
  RogueConsole__reset_io_mode( ROGUE_ARG(THIS->console) );
}

RogueClassFunction_2437* RogueFunction_2437__init__Console( RogueClassFunction_2437* THIS, RogueClassConsole* _auto_2438_0 )
{
  ROGUE_GC_CHECK;
  THIS->console = _auto_2438_0;
  return (RogueClassFunction_2437*)(THIS);
}

void RogueObject__init_object( RogueObject* THIS )
{
  ROGUE_GC_CHECK;
}

RogueObject* RogueObject__init( RogueObject* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueObject*)(THIS);
}

RogueString* RogueObject__description( RogueObject* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[113] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(((RogueString*)Rogue_call_ROGUEM0( 12, ROGUE_ARG(THIS) ))) ))) )))), Rogue_literal_strings[115] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(((RogueString*)(RogueInt64__to_hex_string__Int32( ROGUE_ARG(((RogueObject__object_id( ROGUE_ARG(THIS) )))), 16 )))) ))) )))), Rogue_literal_strings[116] )))) ))));
}

RogueInt64 RogueObject__object_id( RogueObject* THIS )
{
  ROGUE_GC_CHECK;
  RogueInt64 addr_0 = 0;
  addr_0 = (RogueInt64)(intptr_t)THIS;

  return (RogueInt64)(addr_0);
}

RogueString* RogueObject__to_String( RogueObject* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)Rogue_call_ROGUEM0( 2, ROGUE_ARG(THIS) )));
}

RogueString* RogueObject__type_name( RogueObject* THIS )
{
  return (RogueString*)(Rogue_literal_strings[114]);
}

RogueException* RogueException__init_object( RogueException* THIS )
{
  ROGUE_GC_CHECK;
  RogueObject__init_object( ROGUE_ARG(((RogueObject*)THIS)) );
  THIS->stack_trace = ((RogueClassStackTrace*)(RogueStackTrace__init__Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueClassStackTrace*,ROGUE_CREATE_OBJECT(StackTrace))), 1 )));
  return (RogueException*)(THIS);
}

RogueException* RogueException__init( RogueException* THIS )
{
  ROGUE_GC_CHECK;
  THIS->message = ((RogueString*)Rogue_call_ROGUEM0( 12, ROGUE_ARG(THIS) ));
  return (RogueException*)(THIS);
}

RogueString* RogueException__description( RogueException* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(THIS->message);
}

RogueString* RogueException__to_String( RogueException* THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(((RogueString*)Rogue_call_ROGUEM0( 2, ROGUE_ARG(THIS) )));
}

RogueString* RogueException__type_name( RogueException* THIS )
{
  return (RogueString*)(Rogue_literal_strings[13]);
}

RogueException* RogueException__init__String( RogueException* THIS, RogueString* _auto_200_0 )
{
  ROGUE_GC_CHECK;
  THIS->message = _auto_200_0;
  return (RogueException*)(THIS);
}

void RogueException__display( RogueException* THIS )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueStringBuilder*,builder_0,(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))));
  RogueInt32 w_1 = (((RogueInt32__or_smaller__Int32( ROGUE_ARG(((RogueConsole__width( ((RogueClassConsole*)ROGUE_SINGLETON(Console)) )))), 80 ))));
  RogueStringBuilder__println__String( builder_0, ROGUE_ARG((RogueString__operatorTIMES__String_Int32( Rogue_literal_strings[14], w_1 ))) );
  RogueStringBuilder__println__String( builder_0, ROGUE_ARG(((RogueString*)Rogue_call_ROGUEM0( 12, ROGUE_ARG(THIS) ))) );
  RogueStringBuilder__println__String( builder_0, ROGUE_ARG(((RogueString*)(RogueString_List__join__String( ROGUE_ARG(((RogueString_List*)(RogueString__word_wrap__Int32_String( ROGUE_ARG(THIS->message), w_1, Rogue_literal_strings[15] )))), Rogue_literal_strings[16] )))) );
  if (ROGUE_COND(((!!(THIS->stack_trace)) && (!!(THIS->stack_trace->entries->count)))))
  {
    RogueStringBuilder__println( builder_0 );
    RogueStringBuilder__println__String( builder_0, ROGUE_ARG(((RogueString*)(RogueString__trimmed( ROGUE_ARG(((RogueString*)(RogueStackTrace__to_String( ROGUE_ARG(THIS->stack_trace) )))) )))) );
  }
  RogueStringBuilder__println__String( builder_0, ROGUE_ARG((RogueString__operatorTIMES__String_Int32( Rogue_literal_strings[14], w_1 ))) );
  RogueGlobal__println( ROGUE_ARG(((RogueClassGlobal*)(RogueGlobal__print__StringBuilder( ((RogueClassGlobal*)ROGUE_SINGLETON(Global)), builder_0 )))) );
}

RogueString* RogueException__format( RogueException* THIS )
{
  ROGUE_GC_CHECK;
  ROGUE_DEF_LOCAL_REF(RogueString*,st_0,(((((THIS->stack_trace))) ? (ROGUE_ARG((RogueString*)((RogueString*)(RogueStackTrace__to_String( ROGUE_ARG(THIS->stack_trace) ))))) : ROGUE_ARG(Rogue_literal_strings[17]))));
  st_0 = ((RogueString*)(((RogueString*)(RogueString__trimmed( st_0 )))));
  if (ROGUE_COND(!!(((RogueString__count( st_0 ))))))
  {
    st_0 = ((RogueString*)((RogueString__operatorPLUS__String_String( Rogue_literal_strings[16], st_0 ))));
  }
  return (RogueString*)((RogueString__operatorPLUS__String_String( ROGUE_ARG(((((THIS))) ? (ROGUE_ARG((RogueString*)((RogueString*)(RogueException__to_String( ROGUE_ARG(THIS) ))))) : ROGUE_ARG(Rogue_literal_strings[18]))), st_0 )));
}

RogueException* RogueException___throw( RogueException* THIS )
{
  ROGUE_GC_CHECK;
  throw THIS;

}

RogueClassError* RogueError__init_object( RogueClassError* THIS )
{
  ROGUE_GC_CHECK;
  RogueException__init_object( ROGUE_ARG(((RogueException*)THIS)) );
  return (RogueClassError*)(THIS);
}

RogueString* RogueError__type_name( RogueClassError* THIS )
{
  return (RogueString*)(Rogue_literal_strings[402]);
}

RogueException* RogueError___throw( RogueClassError* THIS )
{
  ROGUE_GC_CHECK;
  throw THIS;

}

RogueClassOutOfBoundsError* RogueOutOfBoundsError__init_object( RogueClassOutOfBoundsError* THIS )
{
  ROGUE_GC_CHECK;
  RogueError__init_object( ROGUE_ARG(((RogueClassError*)THIS)) );
  return (RogueClassOutOfBoundsError*)(THIS);
}

RogueString* RogueOutOfBoundsError__type_name( RogueClassOutOfBoundsError* THIS )
{
  return (RogueString*)(Rogue_literal_strings[420]);
}

RogueClassOutOfBoundsError* RogueOutOfBoundsError__init__String( RogueClassOutOfBoundsError* THIS, RogueString* _auto_469_0 )
{
  ROGUE_GC_CHECK;
  THIS->message = _auto_469_0;
  return (RogueClassOutOfBoundsError*)(THIS);
}

RogueException* RogueOutOfBoundsError___throw( RogueClassOutOfBoundsError* THIS )
{
  ROGUE_GC_CHECK;
  throw THIS;

}

RogueClassOutOfBoundsError* RogueOutOfBoundsError__init__Int32_Int32( RogueClassOutOfBoundsError* THIS, RogueInt32 index_0, RogueInt32 limit_1 )
{
  ROGUE_GC_CHECK;
  switch (limit_1)
  {
    case 0:
    {
      RogueOutOfBoundsError__init__String( ROGUE_ARG(THIS), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[21] )))), ROGUE_ARG(((RogueString*)(RogueString__operatorPLUS__Int32( ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], Rogue_literal_strings[0] ))), index_0 )))) )))), Rogue_literal_strings[23] )))) )))) );
      break;
    }
    case 1:
    {
      RogueOutOfBoundsError__init__String( ROGUE_ARG(THIS), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[21] )))), ROGUE_ARG(((RogueString*)(RogueString__operatorPLUS__Int32( ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], Rogue_literal_strings[0] ))), index_0 )))) )))), Rogue_literal_strings[24] )))) )))) );
      break;
    }
    default:
    {
      RogueOutOfBoundsError__init__String( ROGUE_ARG(THIS), ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[21] )))), ROGUE_ARG(((RogueString*)(RogueString__operatorPLUS__Int32( ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], Rogue_literal_strings[0] ))), index_0 )))) )))), Rogue_literal_strings[25] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(((RogueString*)(RogueString__pluralized__Int32( Rogue_literal_strings[26], limit_1 )))) ))) )))), Rogue_literal_strings[30] )))), ROGUE_ARG(((RogueString*)(RogueString__operatorPLUS__Int32( ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], Rogue_literal_strings[0] ))), 0 )))) )))), Rogue_literal_strings[31] )))), ROGUE_ARG(((RogueString*)(RogueString__operatorPLUS__Int32( ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], Rogue_literal_strings[0] ))), ROGUE_ARG(((limit_1) - (1))) )))) )))), Rogue_literal_strings[32] )))) )))) );
    }
  }
  return (RogueClassOutOfBoundsError*)(THIS);
}

RogueClassJSONParseError* RogueJSONParseError__init_object( RogueClassJSONParseError* THIS )
{
  ROGUE_GC_CHECK;
  RogueError__init_object( ROGUE_ARG(((RogueClassError*)THIS)) );
  return (RogueClassJSONParseError*)(THIS);
}

RogueString* RogueJSONParseError__type_name( RogueClassJSONParseError* THIS )
{
  return (RogueString*)(Rogue_literal_strings[421]);
}

RogueClassJSONParseError* RogueJSONParseError__init__String( RogueClassJSONParseError* THIS, RogueString* _auto_479_0 )
{
  ROGUE_GC_CHECK;
  THIS->message = _auto_479_0;
  return (RogueClassJSONParseError*)(THIS);
}

RogueException* RogueJSONParseError___throw( RogueClassJSONParseError* THIS )
{
  ROGUE_GC_CHECK;
  throw THIS;

}

RogueClassIOError* RogueIOError__init_object( RogueClassIOError* THIS )
{
  ROGUE_GC_CHECK;
  RogueError__init_object( ROGUE_ARG(((RogueClassError*)THIS)) );
  return (RogueClassIOError*)(THIS);
}

RogueString* RogueIOError__type_name( RogueClassIOError* THIS )
{
  return (RogueString*)(Rogue_literal_strings[422]);
}

RogueException* RogueIOError___throw( RogueClassIOError* THIS )
{
  ROGUE_GC_CHECK;
  throw THIS;

}

RogueClassPackageError* RoguePackageError__init_object( RogueClassPackageError* THIS )
{
  ROGUE_GC_CHECK;
  RogueError__init_object( ROGUE_ARG(((RogueClassError*)THIS)) );
  return (RogueClassPackageError*)(THIS);
}

RogueString* RoguePackageError__description( RogueClassPackageError* THIS )
{
  ROGUE_GC_CHECK;
  {
    ROGUE_DEF_LOCAL_REF(RogueException*,_auto_2734_0,0);
    ROGUE_DEF_LOCAL_REF(RogueClassStringBuilderPool*,_auto_2733_0,(RogueStringBuilder_pool));
    ROGUE_DEF_LOCAL_REF(RogueString*,_auto_2735_0,0);
    ROGUE_DEF_LOCAL_REF(RogueStringBuilder*,builder_0,(((RogueStringBuilder*)(RogueStringBuilderPool__on_use( _auto_2733_0 )))));
    Rogue_ignore_unused(builder_0);

    ROGUE_TRY
    {
      RogueInt32 w_0 = (((RogueInt32__or_smaller__Int32( ROGUE_ARG(((RogueInt32__or_larger__Int32( ROGUE_ARG(((RogueString__longest_line( ROGUE_ARG(THIS->message) )))), 80 )))), ROGUE_ARG(((RogueConsole__width( ((RogueClassConsole*)ROGUE_SINGLETON(Console)) )))) ))));
      ROGUE_DEF_LOCAL_REF(RogueString*,hr_1,((RogueString__operatorTIMES__String_Int32( Rogue_literal_strings[14], w_0 ))));
      RogueStringBuilder__println__String( builder_0, hr_1 );
      RogueStringBuilder__println__String( builder_0, ROGUE_ARG(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), Rogue_literal_strings[400] )))), ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], ROGUE_ARG(THIS->package_name) ))) )))), Rogue_literal_strings[401] )))) )))) );
      RogueStringBuilder__println__String( builder_0, ROGUE_ARG(THIS->message) );
      RogueStringBuilder__println__String( builder_0, hr_1 );
      {
        _auto_2735_0 = ((RogueString*)(((RogueString*)(RogueStringBuilder__to_String( builder_0 )))));
        goto _auto_2737;
      }
    }
    ROGUE_CATCH( RogueException,_auto_2736_0 )
    {
      _auto_2734_0 = ((RogueException*)(_auto_2736_0));
    }
    ROGUE_END_TRY
    _auto_2737:;
    RogueStringBuilderPool__on_end_use__StringBuilder( _auto_2733_0, builder_0 );
    if (ROGUE_COND(!!(_auto_2734_0)))
    {
      throw ((RogueException*)Rogue_call_ROGUEM0( 16, _auto_2734_0 ));
    }
    return (RogueString*)(_auto_2735_0);
  }
}

RogueString* RoguePackageError__type_name( RogueClassPackageError* THIS )
{
  return (RogueString*)(Rogue_literal_strings[423]);
}

RogueException* RoguePackageError___throw( RogueClassPackageError* THIS )
{
  ROGUE_GC_CHECK;
  throw THIS;

}

RogueClassPackageError* RoguePackageError__init__String_String( RogueClassPackageError* THIS, RogueString* _auto_953_0, RogueString* _auto_952_1 )
{
  ROGUE_GC_CHECK;
  THIS->package_name = _auto_953_0;
  THIS->message = _auto_952_1;
  return (RogueClassPackageError*)(THIS);
}

RogueString* RogueStringEncoding__description( RogueClassStringEncoding THIS )
{
  switch (THIS.value)
  {
    case 0:
    {
      return (RogueString*)(Rogue_literal_strings[8]);
    }
    case 1:
    {
      return (RogueString*)(Rogue_literal_strings[9]);
    }
    case 2:
    {
      return (RogueString*)(Rogue_literal_strings[10]);
    }
    case 3:
    {
      return (RogueString*)(Rogue_literal_strings[11]);
    }
    default:
    {
      return (RogueString*)(Rogue_literal_strings[12]);
    }
  }
}

RogueString* RogueStringEncoding__to_String( RogueClassStringEncoding THIS )
{
  return (RogueString*)(((RogueString*)(RogueStringEncoding__description( THIS ))));
}

RogueLogical RogueStringEncoding__operatorEQUALSEQUALS__StringEncoding( RogueClassStringEncoding THIS, RogueClassStringEncoding other_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((THIS.value) != (other_0.value))))
  {
    return (RogueLogical)(false);
  }
  return (RogueLogical)(true);
}

RogueOptionalString RogueTableKeysIterator_String_Value___read_another( RogueClassTableKeysIterator_String_Value_& THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(!!(THIS.cur))))
  {
    return (RogueOptionalString)((RogueOptionalString__create()));
  }
  ROGUE_DEF_LOCAL_REF(RogueString*,result_0,(THIS.cur->key));
  THIS.cur = THIS.cur->next_entry;
  return (RogueOptionalString)(RogueOptionalString( result_0, true ));
}

RogueInt32 RogueConsoleEventType__to_Int32( RogueClassConsoleEventType THIS )
{
  return (RogueInt32)(THIS.value);
}

RogueString* RogueConsoleEventType__description( RogueClassConsoleEventType THIS )
{
  switch (THIS.value)
  {
    case 0:
    {
      return (RogueString*)(Rogue_literal_strings[317]);
    }
    case 1:
    {
      return (RogueString*)(Rogue_literal_strings[318]);
    }
    case 2:
    {
      return (RogueString*)(Rogue_literal_strings[319]);
    }
    case 3:
    {
      return (RogueString*)(Rogue_literal_strings[320]);
    }
    case 4:
    {
      return (RogueString*)(Rogue_literal_strings[321]);
    }
    case 5:
    {
      return (RogueString*)(Rogue_literal_strings[322]);
    }
    case 6:
    {
      return (RogueString*)(Rogue_literal_strings[323]);
    }
    default:
    {
      return (RogueString*)(Rogue_literal_strings[12]);
    }
  }
}

RogueString* RogueConsoleEventType__to_String( RogueClassConsoleEventType THIS )
{
  return (RogueString*)(((RogueString*)(RogueConsoleEventType__description( THIS ))));
}

RogueLogical RogueConsoleEventType__operatorEQUALSEQUALS__ConsoleEventType( RogueClassConsoleEventType THIS, RogueClassConsoleEventType other_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((THIS.value) != (other_0.value))))
  {
    return (RogueLogical)(false);
  }
  return (RogueLogical)(true);
}

RogueLogical RogueConsoleEvent__is_character( RogueClassConsoleEvent THIS )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(((RogueConsoleEventType__operatorEQUALSEQUALS__ConsoleEventType( THIS._type, RogueClassConsoleEventType( 0 ) ))));
}

RogueString* RogueConsoleEvent__to_String( RogueClassConsoleEvent THIS )
{
  ROGUE_GC_CHECK;
  switch (THIS._type.value)
  {
    case 0:
    {
      switch ((RogueInt32__create__Int32( ROGUE_ARG(THIS.x) )))
      {
        case 8:
        {
          return (RogueString*)(Rogue_literal_strings[308]);
        }
        case (RogueCharacter)9:
        {
          return (RogueString*)(Rogue_literal_strings[309]);
        }
        case 10:
        {
          return (RogueString*)(Rogue_literal_strings[310]);
        }
        case 27:
        {
          return (RogueString*)(Rogue_literal_strings[311]);
        }
        case 17:
        {
          return (RogueString*)(Rogue_literal_strings[312]);
        }
        case 18:
        {
          return (RogueString*)(Rogue_literal_strings[313]);
        }
        case 19:
        {
          return (RogueString*)(Rogue_literal_strings[314]);
        }
        case 20:
        {
          return (RogueString*)(Rogue_literal_strings[315]);
        }
        case 127:
        {
          return (RogueString*)(Rogue_literal_strings[316]);
        }
        default:
        {
          return (RogueString*)(((RogueString*)(RogueString__operatorPLUS__Character( ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], Rogue_literal_strings[0] ))), ROGUE_ARG((RogueCharacter__create__Int32( ROGUE_ARG(THIS.x) ))) ))));
        }
      }
      break;
    }
    case 5:
    case 6:
    {
      return (RogueString*)(((RogueString*)(RogueConsoleEventType__to_String( THIS._type ))));
    }
  }
  return (RogueString*)(((RogueString*)(RogueStringBuilder__to_String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__print__String( ROGUE_ARG(((RogueStringBuilder*)(RogueStringBuilder__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueStringBuilder*,ROGUE_CREATE_OBJECT(StringBuilder))) )))), ROGUE_ARG(((RogueString*)(RogueString__operatorPLUS__String( ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], Rogue_literal_strings[0] ))), ROGUE_ARG(((RogueString*)(RogueConsoleEventType__to_String( THIS._type )))) )))) )))), Rogue_literal_strings[113] )))), ROGUE_ARG(((RogueString*)(RogueString__operatorPLUS__Int32( ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], Rogue_literal_strings[0] ))), ROGUE_ARG(THIS.x) )))) )))), Rogue_literal_strings[15] )))), ROGUE_ARG(((RogueString*)(RogueString__operatorPLUS__Int32( ROGUE_ARG((RogueString__operatorPLUS__String_String( Rogue_literal_strings[0], Rogue_literal_strings[0] ))), ROGUE_ARG(THIS.y) )))) )))), Rogue_literal_strings[116] )))) ))));
}

RogueOptionalString RogueTableKeysIterator_String_String___read_another( RogueClassTableKeysIterator_String_String_& THIS )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(!!(THIS.cur))))
  {
    return (RogueOptionalString)((RogueOptionalString__create()));
  }
  ROGUE_DEF_LOCAL_REF(RogueString*,result_0,(THIS.cur->key));
  THIS.cur = THIS.cur->next_entry;
  return (RogueOptionalString)(RogueOptionalString( result_0, true ));
}

RogueString_List* RogueTableKeysIterator_String_String___to_list__String_List( RogueClassTableKeysIterator_String_String_ THIS, RogueString_List* _auto_4028 )
{
  ROGUE_DEF_LOCAL_REF(RogueString_List*,result_0,_auto_4028);
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(!!(result_0))))
  {
    result_0 = ((RogueString_List*)(((RogueString_List*)(RogueString_List__init( ROGUE_ARG(ROGUE_CREATE_REF(RogueString_List*,ROGUE_CREATE_OBJECT(String_List))) )))));
  }
  {
    RogueClassTableKeysIterator_String_String_ _auto_861_0 = (THIS);
    RogueOptionalString _auto_862_0 = (((RogueTableKeysIterator_String_String___read_another( _auto_861_0 ))));
    while (ROGUE_COND(_auto_862_0.exists))
    {
      ROGUE_GC_CHECK;
      ROGUE_DEF_LOCAL_REF(RogueString*,_auto_860_0,(_auto_862_0.value));
      _auto_862_0 = ((RogueOptionalString)(((RogueTableKeysIterator_String_String___read_another( _auto_861_0 )))));
      RogueString_List__add__String( result_0, _auto_860_0 );
    }
  }
  return (RogueString_List*)(result_0);
}

RogueString* RogueFilePattern__to_String( RogueClassFilePattern THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(THIS.pattern);
}

RogueLogical RogueFileOptions__keeping_files( RogueClassFileOptions THIS )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(((!!(((THIS.flags) & (8)))) || (!(!!(((THIS.flags) & (24)))))));
}

RogueLogical RogueFileOptions__keeping_folders( RogueClassFileOptions THIS )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(((!!(((THIS.flags) & (16)))) || (!(!!(((THIS.flags) & (24)))))));
}

RogueInt32 RogueVersionNumber__count( RogueClassVersionNumber THIS )
{
  ROGUE_GC_CHECK;
  return (RogueInt32)(((((RogueString__count__Character( ROGUE_ARG(THIS.version), (RogueCharacter)'.' )))) + (1)));
}

RogueLogical RogueVersionNumber__is_compatible_with__String( RogueClassVersionNumber THIS, RogueString* other_0 )
{
  ROGUE_GC_CHECK;
  return (RogueLogical)(((RogueVersionNumber__is_compatible_with__VersionNumber( THIS, RogueClassVersionNumber( other_0, true ) ))));
}

RogueLogical RogueVersionNumber__is_compatible_with__VersionNumber( RogueClassVersionNumber THIS, RogueClassVersionNumber other_0 )
{
  ROGUE_GC_CHECK;
  RogueInt32 min_parts_1 = (((RogueInt32__or_smaller__Int32( ROGUE_ARG(((RogueVersionNumber__count( THIS )))), ROGUE_ARG(((RogueVersionNumber__count( other_0 )))) ))));
  {
    RogueInt32 n_2 = (0);
    RogueInt32 _auto_958_3 = (min_parts_1);
    for (;ROGUE_COND(((n_2) < (_auto_958_3)));++n_2)
    {
      ROGUE_GC_CHECK;
      if (ROGUE_COND(((((RogueVersionNumber__part__Int32( THIS, n_2 )))) != (((RogueVersionNumber__part__Int32( other_0, n_2 )))))))
      {
        return (RogueLogical)(false);
      }
    }
  }
  return (RogueLogical)(true);
}

RogueInt32 RogueVersionNumber__operatorLTGT__String( RogueClassVersionNumber THIS, RogueString* other_0 )
{
  ROGUE_GC_CHECK;
  return (RogueInt32)(((RogueVersionNumber__operatorLTGT__VersionNumber( THIS, RogueClassVersionNumber( other_0, true ) ))));
}

RogueInt32 RogueVersionNumber__operatorLTGT__VersionNumber( RogueClassVersionNumber THIS, RogueClassVersionNumber other_0 )
{
  ROGUE_GC_CHECK;
  RogueInt32 max_parts_1 = (((RogueInt32__or_larger__Int32( ROGUE_ARG(((RogueVersionNumber__count( THIS )))), ROGUE_ARG(((RogueVersionNumber__count( other_0 )))) ))));
  {
    RogueInt32 n_3 = (0);
    RogueInt32 _auto_959_4 = (max_parts_1);
    for (;ROGUE_COND(((n_3) < (_auto_959_4)));++n_3)
    {
      ROGUE_GC_CHECK;
      RogueInt32 delta_2 = (((((RogueVersionNumber__part__Int32( THIS, n_3 )))) - (((RogueVersionNumber__part__Int32( other_0, n_3 ))))));
      if (ROGUE_COND(((delta_2) != (0))))
      {
        return (RogueInt32)(((RogueInt32__sign( delta_2 ))));
      }
    }
  }
  return (RogueInt32)(0);
}

RogueInt32 RogueVersionNumber__part__Int32( RogueClassVersionNumber THIS, RogueInt32 n_0 )
{
  ROGUE_GC_CHECK;
  RogueInt32 cur_p_1 = (0);
  RogueInt32 result_2 = (0);
  {
    ROGUE_DEF_LOCAL_REF(RogueString*,_auto_961_0,(THIS.version));
    RogueInt32 _auto_962_0 = (0);
    RogueInt32 _auto_963_0 = (((((RogueString__count( _auto_961_0 )))) - (1)));
    for (;ROGUE_COND(((_auto_962_0) <= (_auto_963_0)));++_auto_962_0)
    {
      ROGUE_GC_CHECK;
      RogueCharacter ch_0 = (((RogueString__get__Int32( _auto_961_0, _auto_962_0 ))));
      if (ROGUE_COND(((ch_0) == ((RogueCharacter)'.'))))
      {
        ++cur_p_1;
        if (ROGUE_COND(((cur_p_1) > (n_0))))
        {
          return (RogueInt32)(result_2);
        }
      }
      else if (ROGUE_COND(((((cur_p_1) == (n_0))) && (((RogueCharacter__is_number__Int32( ch_0, 10 )))))))
      {
        result_2 = ((RogueInt32)(((((result_2) * (10))) + (((RogueCharacter__to_number__Int32( ch_0, 10 )))))));
      }
    }
  }
  return (RogueInt32)(result_2);
}

RogueString* RogueVersionNumber__to_String( RogueClassVersionNumber THIS )
{
  ROGUE_GC_CHECK;
  return (RogueString*)(THIS.version);
}

RogueLogical RogueBest_String___consider__String( RogueClassBest_String_& THIS, RogueString* candidate_value_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(((!(THIS.exists)) || ((Rogue_call_ROGUEM11( 13, ROGUE_ARG(THIS.better_fn), candidate_value_0, ROGUE_ARG(THIS.value) ))))))
  {
    THIS.exists = true;
    THIS.value = candidate_value_0;
    return (RogueLogical)(true);
  }
  else
  {
    return (RogueLogical)(false);
  }
}

RogueByte_List* RogueZipEntry__extract__Byte_List( RogueClassZipEntry THIS, RogueByte_List* _auto_4029 )
{
  ROGUE_DEF_LOCAL_REF(RogueByte_List*,buffer_0,_auto_4029);
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(!!(buffer_0))))
  {
    buffer_0 = ((RogueByte_List*)(((RogueByte_List*)(RogueByte_List__init__Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueByte_List*,ROGUE_CREATE_OBJECT(Byte_List))), ROGUE_ARG(((RogueInt32)(THIS.size))) )))));
  }
  RogueInt32 start_1 = (buffer_0->count);
  RogueByte_List__expand__Int32( buffer_0, ROGUE_ARG(((RogueInt32)(THIS.size))) );
  RogueZip__set_mode__Int32( ROGUE_ARG(THIS.zip), 114 );
  zip_entry_open( THIS.zip->zip, THIS.name->utf8 );
  zip_entry_noallocread( THIS.zip->zip, buffer_0->data->as_bytes+start_1, THIS.size );
  zip_entry_close( THIS.zip->zip );

  return (RogueByte_List*)(buffer_0);
}

void RogueZipEntry__extract__File( RogueClassZipEntry THIS, RogueClassFile* file_0 )
{
  ROGUE_GC_CHECK;
  if (ROGUE_COND(!(!!(RogueZipEntry_buffer))))
  {
    RogueZipEntry_buffer = ((RogueByte_List*)(RogueByte_List__init__Int32( ROGUE_ARG(ROGUE_CREATE_REF(RogueByte_List*,ROGUE_CREATE_OBJECT(Byte_List))), ROGUE_ARG(((RogueInt32)(THIS.size))) )));
  }
  RogueZipEntry__extract__Byte_List( THIS, ROGUE_ARG(([&]()->RogueByte_List*{
    ROGUE_DEF_LOCAL_REF(RogueByte_List*,_auto_1019_0,(RogueZipEntry_buffer));
    RogueByte_List__clear( _auto_1019_0 );
     return _auto_1019_0;
  }())
  ) );
  if (ROGUE_COND(((RogueFile__is_folder( file_0 )))))
  {
    ROGUE_DEF_LOCAL_REF(RogueString*,output_filepath_1,((RogueString__operatorSLASH__String_String( ROGUE_ARG(file_0->filepath), ROGUE_ARG(THIS.name) ))));
    if (ROGUE_COND(THIS.is_folder))
    {
      RogueFile__create_folder__String( output_filepath_1 );
    }
    else
    {
      if (ROGUE_COND(((RogueString__contains__Character_Logical( output_filepath_1, (RogueCharacter)'/', false )))))
      {
        RogueFile__create_folder__String( ROGUE_ARG((RogueFile__folder__String( output_filepath_1 ))) );
      }
      RogueFile__save__String_Byte_List( output_filepath_1, ROGUE_ARG(RogueZipEntry_buffer) );
    }
  }
  else if (ROGUE_COND(THIS.is_folder))
  {
    RogueFile__create_folder( file_0 );
  }
  else
  {
    if (ROGUE_COND(((RogueString__contains__Character_Logical( ROGUE_ARG(file_0->filepath), (RogueCharacter)'/', false )))))
    {
      RogueFile__create_folder__String( ROGUE_ARG(((RogueString*)(RogueFile__folder( file_0 )))) );
    }
    RogueFile__save__Byte_List( file_0, ROGUE_ARG(RogueZipEntry_buffer) );
  }
}


void Rogue_configure( int argc, const char* argv[] )
{
  if (Rogue_configured) return;
  Rogue_configured = 1;

  Rogue_argc = argc;
  Rogue_argv = argv;

  Rogue_thread_register();
  Rogue_configure_gc();
  Rogue_configure_types();
  set_terminate( Rogue_terminate_handler );
  for (int i=0; i<Rogue_accessible_type_count; ++i)
  {
    *(Rogue_accessible_type_pointers[i]) = &Rogue_types[Rogue_accessible_type_indices[i]];
  }

  for (int i=0; i<Rogue_literal_string_count; ++i)
  {
    Rogue_define_literal_string( i, Rogue_literal_c_strings[i], -1 );
  }

}

void Rogue_init_thread()
{
  ROGUE_THREAD_LOCALS_INIT(RogueStringBuilder_work_bytes, RogueListRewriter_String__pool);
  RogueStringBuilder__init_class_thread_local();
}

void Rogue_deinit_thread()
{
  ROGUE_THREAD_LOCALS_DEINIT(RogueStringBuilder_work_bytes, RogueListRewriter_String__pool);
}

#ifdef ROGUE_AUTO_LAUNCH
__attribute__((constructor))
#endif
void Rogue_launch()
{
  Rogue_configure(0, NULL);
  RogueRuntime__init_class();
  RogueSystem__init_class();
  RogueStringValue__init_class();
  RogueLogicalValue__init_class();
  RogueStringEncoding__init_class();
  RogueConsoleEventType__init_class();
  RogueUnixConsoleMouseEventType__init_class();

  Rogue_init_thread();

  for (int i=1; i<Rogue_argc; ++i)
  {
    RogueString_List__add__String( RogueSystem_command_line_arguments,
        RogueString_create_from_utf8( Rogue_argv[i], -1 ) );
  }

  // Instantiate essential singletons
  ROGUE_SINGLETON( Global );

  RogueGlobal__on_launch( (RogueClassGlobal*) (ROGUE_SINGLETON(Global)) );
  Rogue_collect_garbage();
}

bool Rogue_update_tasks()
{
  // Returns true if any tasks are still active
  try
  {
    Rogue_collect_garbage();
    return false;
  }
  catch (RogueException* err)
  {
    printf( "Uncaught exception\n" );
    RogueException__display( err );
    return false;
  }
}

int main( int argc, const char* argv[] )
{
  try
  {
    Rogue_configure( argc, argv );
    Rogue_launch();

    while (Rogue_update_tasks()) {}

    Rogue_quit();
  }
  catch (RogueException* err)
  {
    printf( "Uncaught exception\n" );
    RogueException__display( err );
    return 1;
  }

  return 0;
}
