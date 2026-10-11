/* iOS-Madeira: userspace ntsync. See linux/ntsync.h in this directory.
 *
 * WHY: with the JIT no longer thrashing, a heavily threaded game still ran at
 * ~10 FPS with no thread saturated. The wineserver was answering ~18,000
 * requests a second, 2/3 of them `select` and most of the rest event_op /
 * release_semaphore: every wait and every wake of the game's job system was a
 * pipe write, a server-thread wakeup, a reply and a client wakeup. ~1,750 of
 * those per frame is the frame time. This removes the round trip: the object
 * state lives here and both sides operate on it directly.
 *
 * MODEL: one global mutex guards all object state; each blocked waiter owns a
 * condition variable on its own stack. At tens of thousands of operations a
 * second a single uncontended lock costs far less than one pipe round trip,
 * and one lock makes wait-all atomic without any ordering protocol.
 *
 * Semantics follow Documentation/userspace-api/ntsync.rst: semaphores, mutexes
 * (recursive, owner-tracked, abandonment reported once as EOWNERDEAD), manual /
 * auto-reset events, pulse, wait-any / wait-all with an optional alert event
 * reported as index == count, absolute timeouts on MONOTONIC or REALTIME. */
#define MADSYNC_IMPLEMENTATION
#include "linux/ntsync.h"
#include "../madeira_cfg.h"   /* ml1095 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <mach/mach.h>
#include <mach/semaphore.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { MS_SEM = 1, MS_MUTEX, MS_EVENT };

struct ms_waiter;
struct ms_link { struct ms_link *next, *prev; struct ms_waiter *w; };

struct ms_obj
{
    int       type, refs;
    uint32_t  a, b;          /* sem: count,max  mutex: owner,count  event: manual,signaled */
    int       ownerdead;     /* mutex only */
    struct ms_link head;     /* circular list of waiters */
};

struct ms_waiter
{
    semaphore_t    sem;          /* the owning thread's wake semaphore */
    int            fds[NTSYNC_MAX_WAIT_COUNT + 1];
    struct ms_obj *objs[NTSYNC_MAX_WAIT_COUNT + 1];
    struct ms_link links[NTSYNC_MAX_WAIT_COUNT + 1];
    unsigned       count;        /* real objects (alert excluded) */
    int            all, has_alert, done, ownerdead;
    int            kicked;       /* ml1259: the server queued a system APC for this thread */
    uint32_t       owner, index;
    uint64_t       wake_t;       /* ml1122: mach_absolute_time when a waker satisfied us */
};

static pthread_mutex_t  g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct ms_obj  **g_tab;
static unsigned         g_cap;
static unsigned        *g_freelist; static unsigned g_nfree, g_freecap;
static unsigned long long g_waits, g_blocked, g_wakes, g_creates, g_polls;
/* ml1122: how long a satisfied waiter takes to run again, lock contention, and
 * the madsync-spin-us experiment (spin lock-free before sleeping). */
static volatile unsigned long long g_lat_n, g_lat_ticks, g_lat_hist[6], g_contended, g_lock_wait_ticks, g_spin_tries, g_spin_hits;
static uint64_t g_spin_ticks; static int g_spin_loaded;
static double ms_ticks_per_us(void)
{
    static double v;
    if (!v) { mach_timebase_info_data_t tb; mach_timebase_info( &tb ); v = 1000.0 * tb.denom / tb.numer; }
    return v;
}
static void ms_lat_note( uint64_t ticks )
{
    double us = ticks / ms_ticks_per_us();
    int b = us < 5 ? 0 : us < 20 ? 1 : us < 50 ? 2 : us < 100 ? 3 : us < 500 ? 4 : 5;
    __sync_fetch_and_add( &g_lat_n, 1 ); __sync_fetch_and_add( &g_lat_ticks, ticks ); __sync_fetch_and_add( &g_lat_hist[b], 1 );
}

/* ---- locking -----------------------------------------------------------------
 * A Wine thread can be TERMINATED from a signal handler (pthread_exit). If that
 * happened while it held g_lock every other thread would block forever, so all
 * signals are masked for the (short) time the lock is held and the mask is put
 * back before a thread actually sleeps. A thread that dies while asleep is
 * cleaned up by the TSD destructor below: its waiter is per-thread HEAP state,
 * never stack, so a waker can never touch memory a dead thread left behind. */
static void ms_lock( sigset_t *old )
{
    sigset_t all;
    sigfillset( &all );
    pthread_sigmask( SIG_BLOCK, &all, old );
    if (pthread_mutex_trylock( &g_lock ))   /* ml1122: count and time contention */
    {
        /* ml1124: the lock is held for well under a microsecond; a contended
         * pthread_mutex_lock sleeps in the kernel (__psynch_mutexwait, ~20 us
         * measured on average in ph-rdr74, 3.4 k times a second). Spin on
         * trylock first. madeira.cfg madsync-lock-spin = N tries (default 256,
         * 0 = off). */
        static int spin = -1;
        int k;
        uint64_t t0 = mach_absolute_time();
        if (spin < 0) { long long v = madeira_cfg_int( "madsync-lock-spin", 256 ); spin = v < 0 ? 0 : v > 100000 ? 100000 : (int)v; }
        for (k = 0; k < spin; k++)
        {
            __asm__ __volatile__( "yield" );
            if (!pthread_mutex_trylock( &g_lock )) goto got;
        }
        pthread_mutex_lock( &g_lock );
    got:
        g_contended++; g_lock_wait_ticks += mach_absolute_time() - t0;
    }
}
static void ms_unlock( const sigset_t *old )
{
    pthread_mutex_unlock( &g_lock );
    pthread_sigmask( SIG_SETMASK, old, NULL );
}

/* ---- enable switch ------------------------------------------------------- */
int madsync_enabled(void)
{
    static int state = -1;
    if (state < 0)
    {
        /* This first runs inside the wineserver thread on its first object and
         * the answer is kept for the whole app run, so the log also says what
         * was read and from where (see madeira_cfg__dir for the directory rules). */
        char v[32];
        int set = madeira_cfg_get( "inproc-sync", v, sizeof v );
        /* On only when madeira.cfg selects it (inproc-sync = 1); fastsync is the default engine. */
        int on = madeira_cfg_sync_engine() == MADEIRA_SYNC_MADSYNC;
        /* Madeira Dock: the app sets MADEIRA_MADSYNC_SESSION=0 only for a session that runs a
         * game's one-time installers first (Wine's services.exe never answered its RPC clients
         * under madsync on device). Unset, or any other value: unchanged. Read once, like the
         * cfg key, so it holds for the whole session. */
        const char *session = getenv( "MADEIRA_MADSYNC_SESSION" );
        if (on && session && !strcmp( session, "0" ))
        {
            on = 0;
            dprintf( 2, "[madsync] off for this session (MADEIRA_MADSYNC_SESSION=0: one-time installs)\n" );
        }
        state = on;
        dprintf( 2, "[madsync] ml1058 in-process synchronisation %s (madeira.cfg inproc-sync = 1 enables; fastsync is the default)\n",
                 on ? "ENABLED" : "disabled" );
        dprintf( 2, "[madsync] config inproc-sync=%s cfg=%s dir=%s (MADEIRA_CFG_EARLY_DOCS=0 restores the old lookup)\n",
                 set ? (v[0] ? v : "(empty)") : "unset",
                 madeira_cfg_present() ? "present" : "absent", madeira_cfg_dir_source() );
    }
    return state;
}

/* ---- table ---------------------------------------------------------------- */
static struct ms_obj *obj_of( int fd )
{
    unsigned idx, cap;
    struct ms_obj **tab;
    if (!MADSYNC_IS_FD( fd )) return NULL;
    idx = (unsigned)fd & 0x0fffffffu;
    /* ml1063: readable without the lock. Growth publishes the new table BEFORE the
     * new capacity and never frees the old one, so (cap, tab) read in this order
     * always index a live table. */
    cap = *(volatile unsigned *)&g_cap;
    __sync_synchronize();
    tab = *(struct ms_obj *volatile *volatile *)&g_tab;
    return idx < cap ? tab[idx] : NULL;
}

static int obj_new( int type, uint32_t a, uint32_t b )
{
    sigset_t ms_old;
    struct ms_obj *o = calloc( 1, sizeof(*o) );
    unsigned idx;
    if (!o) { errno = ENOMEM; return -1; }
    o->type = type; o->refs = 1; o->a = a; o->b = b;
    o->head.next = o->head.prev = &o->head;
    ms_lock( &ms_old );
    if (g_nfree) idx = g_freelist[--g_nfree];
    else
    {
        static unsigned next = 1;          /* index 0 stays unused: alert == 0 means "none" */
        if (next >= g_cap)
        {
            unsigned ncap = g_cap ? g_cap * 2 : 4096;
            struct ms_obj **nt = calloc( ncap, sizeof(*nt) );   /* the old table is deliberately leaked: lock-free readers may hold it */
            if (!nt) { ms_unlock( &ms_old ); free( o ); errno = ENOMEM; return -1; }
            if (g_tab) memcpy( nt, g_tab, g_cap * sizeof(*nt) );
            g_tab = nt;
            __sync_synchronize();
            g_cap = ncap;
        }
        idx = next++;
    }
    g_tab[idx] = o;
    g_creates++;
    ms_unlock( &ms_old );
    return MADSYNC_FD_BASE | (int)idx;
}

/* lock held */
static void obj_unref_locked( unsigned idx )
{
    struct ms_obj *o = g_tab[idx];
    if (!o || --o->refs > 0) return;
    g_tab[idx] = NULL;
    if (g_nfree == g_freecap)
    {
        unsigned ncap = g_freecap ? g_freecap * 2 : 1024;
        unsigned *nf = realloc( g_freelist, ncap * sizeof(*nf) );
        if (nf) { g_freelist = nf; g_freecap = ncap; }
    }
    if (g_nfree < g_freecap) g_freelist[g_nfree++] = idx;
    free( o );
}

int madsync_ref( int fd )
{
    sigset_t ms_old;
    struct ms_obj *o;
    int ret = -1;
    ms_lock( &ms_old );
    if ((o = obj_of( fd ))) { o->refs++; ret = fd; }
    ms_unlock( &ms_old );
    return ret;
}

int madsync_close( int fd )
{
    sigset_t ms_old;
    if (fd == MADSYNC_DEVICE_FD) return 0;
    if (!MADSYNC_IS_FD( fd )) return close( fd );
    ms_lock( &ms_old );
    if (obj_of( fd )) obj_unref_locked( (unsigned)fd & 0x0fffffffu );
    ms_unlock( &ms_old );
    return 0;
}

/* ---- server -> client hand-off ------------------------------------------- */
static struct { unsigned pid, handle; int fd; } g_post[256];

void madsync_post( unsigned int pid, unsigned int handle, int fd )
{
    sigset_t ms_old;
    unsigned i;
    ms_lock( &ms_old );
    for (i = 0; i < 256; i++) if (!g_post[i].fd) break;
    if (i < 256)
    {
        struct ms_obj *o = obj_of( fd );
        if (o) { o->refs++; g_post[i].pid = pid; g_post[i].handle = handle; g_post[i].fd = fd; }
    }
    else dprintf( 2, "[madsync] hand-off table full; pid %04x handle %#x dropped\n", pid, handle );
    ms_unlock( &ms_old );
}

int madsync_take( unsigned int pid, unsigned int handle )
{
    sigset_t ms_old;
    unsigned i; int fd = -1;
    ms_lock( &ms_old );
    for (i = 0; i < 256; i++)
        if (g_post[i].fd && g_post[i].pid == pid && g_post[i].handle == handle)
        { fd = g_post[i].fd; g_post[i].fd = 0; break; }
    ms_unlock( &ms_old );
    return fd;
}

/* ---- signalling core (lock held) ------------------------------------------ */
static int obj_ready( const struct ms_obj *o, uint32_t owner )
{
    switch (o->type)
    {
    case MS_SEM:   return o->a > 0;
    case MS_MUTEX: return o->a == 0 || o->a == owner;
    default:       return o->b != 0;
    }
}

static void obj_take( struct ms_obj *o, struct ms_waiter *w )
{
    switch (o->type)
    {
    case MS_SEM:   o->a--; break;
    case MS_MUTEX:
        if (o->ownerdead) { w->ownerdead = 1; o->ownerdead = 0; }
        o->a = w->owner; o->b++;
        break;
    default:       if (!o->a) o->b = 0; break;       /* auto-reset consumes; manual stays set */
    }
}

static void waiter_unlink( struct ms_waiter *w )
{
    unsigned i, n = w->count + (w->has_alert ? 1 : 0);
    for (i = 0; i < n; i++)
    {
        struct ms_link *l = &w->links[i];
        if (l->next) { l->prev->next = l->next; l->next->prev = l->prev; l->next = l->prev = NULL; }
    }
}

/* Can this waiter complete right now? If so consume and record the index. */
static int waiter_try( struct ms_waiter *w )
{
    unsigned i;
    if (w->done) return 1;
    if (w->all)
    {
        for (i = 0; i < w->count; i++) if (!obj_ready( w->objs[i], w->owner )) break;
        if (i == w->count && w->count)
        {
            for (i = 0; i < w->count; i++) obj_take( w->objs[i], w );
            w->index = 0; w->done = 1; return 1;
        }
    }
    else
    {
        for (i = 0; i < w->count; i++)
            if (obj_ready( w->objs[i], w->owner )) { obj_take( w->objs[i], w ); w->index = i; w->done = 1; return 1; }
    }
    if (w->has_alert && obj_ready( w->objs[w->count], w->owner ))
    {
        obj_take( w->objs[w->count], w );
        w->index = w->count; w->done = 1; return 1;
    }
    return 0;
}

/* An object's state rose: satisfy its waiters in FIFO order while it stays ready. */
static void obj_wake( struct ms_obj *o )
{
    struct ms_link *l = o->head.next;
    while (l != &o->head)
    {
        struct ms_link *next = l->next;
        struct ms_waiter *w = l->w;
        if (!w->done && waiter_try( w ))
        {
            waiter_unlink( w );
            g_wakes++;
            w->wake_t = mach_absolute_time();   /* ml1122 */
            semaphore_signal( w->sem );
            next = o->head.next;               /* the list changed under us */
        }
        l = next;
    }
}

static uint64_t now_ns( clockid_t c )
{
    struct timespec ts;
    clock_gettime( c, &ts );
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- per-thread wait state ------------------------------------------------- */
struct ms_thread
{
    struct ms_waiter  w;
    int               waiting;
    uint32_t          tid;                 /* ml1259: owner of this thread's last wait */
    struct ms_thread *snext, *sprev;       /* ml1259: on g_sleepers while waiting (lock held) */
};
static pthread_key_t  g_key;
static pthread_once_t g_key_once = PTHREAD_ONCE_INIT;

/* ---- system APC kicks (ml1259) ----------------------------------------------
 * The server runs the client half of async I/O (APC_ASYNC_IO), and other system
 * APCs, on the thread that queued them. Upstream interrupts a thread that is not
 * waiting in the server with SIGUSR1. On iOS that signal can't be sent, so
 * queue_apc handed the APC to some other thread waiting in the server, or
 * dropped it. With madsync almost no thread waits in the server: the APC was
 * dropped, the async completed with nothing written to the caller's IOSB, and
 * ws2_32's connect() read an uninitialised io.Status. Every Denuvo activation
 * connect failed with 12029 that way (it passed with fastsync), and it is the
 * likely cause of the services.exe RPC hang behind MADEIRA_MADSYNC_SESSION=0.
 *
 * Now the server's SIGUSR1 (mach_ios.c send_thread_signal) is a kick instead:
 * it wakes the thread's madsync wait, or records the tid so its next wait sees
 * it. A kicked thread leaves the wait, runs its queued system APCs through the
 * runner ntdll registered (a zero-timeout interruptible server select, which
 * is what usr1_handler's wait_suspend would have done), and waits again with
 * the same deadline. The APC stays queued on the thread that issued the I/O. */
#define MS_KICKED      (-2)
#define MS_KICK_SLOTS  1024u                    /* power of two */
#define MS_KICK_TOMB   1u                       /* Windows tids are multiples of 4, never 1 */
static volatile uint32_t g_kick[MS_KICK_SLOTS];  /* pending tids; 0 = never used, TOMB = taken */
static volatile unsigned g_nkick;                /* live entries; 0 lets every wait skip the table */
static struct ms_thread *g_sleepers;             /* threads asleep in do_wait */
static void (*g_apc_runner)(void);
static unsigned long long g_kicks, g_kick_wakes, g_kick_runs;

void madsync_set_apc_runner( void (*fn)(void) ) { g_apc_runner = fn; }

/* Safe without the lock: slots are single words, and a stale read only delays a
 * kick to the next wait. */
static int ms_kick_find( uint32_t tid )
{
    unsigned i, h = (tid >> 2) & (MS_KICK_SLOTS - 1);
    for (i = 0; i < MS_KICK_SLOTS; i++, h = (h + 1) & (MS_KICK_SLOTS - 1))
    {
        uint32_t v = g_kick[h];
        if (v == tid) return (int)h;
        if (!v) return -1;
    }
    return -1;
}

/* lock held */
static void ms_kick_add_locked( uint32_t tid )
{
    unsigned i, h = (tid >> 2) & (MS_KICK_SLOTS - 1);
    if (tid <= MS_KICK_TOMB || ms_kick_find( tid ) >= 0) return;
    for (i = 0; i < MS_KICK_SLOTS; i++, h = (h + 1) & (MS_KICK_SLOTS - 1))
    {
        if (g_kick[h] > MS_KICK_TOMB) continue;
        g_kick[h] = tid;
        g_nkick++;
        return;
    }
    dprintf( 2, "[madsync] ml1259 kick table full; tid %04x runs its APC at its next server wait\n", tid );
}

/* lock held */
static int ms_kick_take_locked( uint32_t tid )
{
    int h = ms_kick_find( tid );
    if (h < 0) return 0;
    g_kick[h] = MS_KICK_TOMB;
    if (!--g_nkick) memset( (void *)g_kick, 0, sizeof(g_kick) );   /* drop the tombstones */
    return 1;
}

/* lock held */
static void ms_sleepers_add( struct ms_thread *t )
{
    t->sprev = NULL;
    t->snext = g_sleepers;
    if (g_sleepers) g_sleepers->sprev = t;
    g_sleepers = t;
}

/* lock held */
static void ms_sleepers_del( struct ms_thread *t )
{
    if (t->sprev) t->sprev->snext = t->snext;
    else if (g_sleepers == t) g_sleepers = t->snext;
    if (t->snext) t->snext->sprev = t->sprev;
    t->snext = t->sprev = NULL;
}

/* Called by the server (mach_ios.c) where upstream sends SIGUSR1 for a system APC. */
int madsync_kick( unsigned int tid )
{
    sigset_t old;
    struct ms_thread *t;
    unsigned long long n;
    int woke = 0;

    ms_lock( &old );
    for (t = g_sleepers; t; t = t->snext)
    {
        if (t->w.owner != tid || t->w.done) continue;
        t->w.kicked = 1;
        semaphore_signal( t->w.sem );
        woke = 1;
        break;
    }
    if (!woke) ms_kick_add_locked( tid );
    n = ++g_kicks;
    if (woke) g_kick_wakes++;
    ms_unlock( &old );
    if (n <= 16 || !(n & 1023))
        dprintf( 2, "[madsync] ml1259 system APC kick #%llu tid %04x: %s\n",
                 n, tid, woke ? "woke its in-process wait" : "pending until its next wait" );
    return 1;
}

static void ms_run_system_apcs(void)
{
    int saved = errno;
    if (g_apc_runner)
    {
        g_apc_runner();
        __sync_fetch_and_add( &g_kick_runs, 1 );
    }
    errno = saved;
}

static void ms_release_refs_locked( struct ms_waiter *w )
{
    unsigned i, n = w->count + (w->has_alert ? 1 : 0);
    for (i = 0; i < n; i++) obj_unref_locked( (unsigned)w->fds[i] & 0x0fffffffu );   /* our reference pinned the slot */
}

static void ms_thread_destroy( void *arg )
{
    struct ms_thread *t = arg;
    sigset_t old;
    ms_lock( &old );
    if (t->waiting)                       /* died asleep: nobody may find this waiter again */
    {
        waiter_unlink( &t->w );
        ms_release_refs_locked( &t->w );
        ms_sleepers_del( t );
        t->waiting = 0;
    }
    if (g_nkick && t->tid) ms_kick_take_locked( t->tid );   /* ml1259: no stale kick for a dead tid */
    ms_unlock( &old );
    semaphore_destroy( mach_task_self(), t->w.sem );
    free( t );
}
static void ms_key_init(void) { pthread_key_create( &g_key, ms_thread_destroy ); }

static struct ms_thread *ms_thread_get(void)
{
    struct ms_thread *t;
    pthread_once( &g_key_once, ms_key_init );
    if ((t = pthread_getspecific( g_key ))) return t;
    if (!(t = calloc( 1, sizeof(*t) ))) return NULL;
    if (semaphore_create( mach_task_self(), &t->w.sem, SYNC_POLICY_FIFO, 0 ) != KERN_SUCCESS) { free( t ); return NULL; }
    pthread_setspecific( g_key, t );
    return t;
}

/* One wait. Returns MS_KICKED, without consuming anything, when a system APC is
 * queued for this thread and nothing completed the wait; *kicked says the APCs
 * need running either way. */
static int do_wait_once( struct ntsync_wait_args *args, int all, int *kicked )
{
    struct ms_thread *t;
    struct ms_waiter *w;
    const int *fds = (const int *)(uintptr_t)args->objs;
    semaphore_t sem;
    sigset_t old;
    unsigned i, n;
    int ret = 0;

    if (args->count > NTSYNC_MAX_WAIT_COUNT) { errno = EINVAL; return -1; }
    /* ml1063: LOCK-FREE POLL. The dominant call is WaitForSingleObject(h, 0) in a
     * spin loop (19-290M per run) and almost always finds nothing. Reading the
     * object state without the lock is safe here: the caller's cached descriptor
     * holds a reference (Wine's inproc cache), so the object cannot be freed
     * under us, the state words are 32-bit and read atomically, and a stale read
     * can only make us take the locked path, which re-checks. Only a poll whose
     * deadline has ALREADY passed and finds NOTHING ready skips the lock. */
    if (args->timeout != ~(uint64_t)0 &&
        now_ns( (args->flags & NTSYNC_WAIT_REALTIME) ? CLOCK_REALTIME : CLOCK_MONOTONIC ) >= args->timeout)
    {
        unsigned i; int any = 0;
        for (i = 0; i < args->count && !any; i++)
        {
            struct ms_obj *o = obj_of( fds[i] );
            if (!o) { any = 1; break; }                       /* let the locked path report EBADF */
            any = obj_ready( o, args->owner );
        }
        if (!any && args->alert)
        {
            struct ms_obj *o = obj_of( (int)args->alert );
            any = !o || obj_ready( o, args->owner );
        }
        if (!any && !(g_nkick && ms_kick_find( args->owner ) >= 0))   /* ml1259: a kick takes the locked path */
        {
            __sync_fetch_and_add( &g_polls, 1 );
            errno = ETIMEDOUT;
            return -1;
        }
    }
    if (!(t = ms_thread_get())) { errno = ENOMEM; return -1; }
    w = &t->w;
    sem = w->sem;
    memset( w, 0, sizeof(*w) );
    w->sem = sem;
    w->count = args->count; w->all = all; w->owner = args->owner; w->index = ~0u;

    ms_lock( &old );
    t->tid = args->owner;
    if (g_nkick && ms_kick_take_locked( args->owner ))   /* ml1259: run queued system APCs first */
    {
        ms_unlock( &old );
        *kicked = 1;
        return MS_KICKED;
    }
    g_waits++;
    for (i = 0; i < w->count; i++)
    {
        w->fds[i] = fds[i];
        if (!(w->objs[i] = obj_of( fds[i] ))) { ms_unlock( &old ); errno = EBADF; return -1; }
    }
    if (args->alert)
    {
        w->fds[w->count] = (int)args->alert;
        if (!(w->objs[w->count] = obj_of( (int)args->alert ))) { ms_unlock( &old ); errno = EBADF; return -1; }
        w->has_alert = 1;
    }
    n = w->count + (w->has_alert ? 1 : 0);

    /* A poll (deadline already past) that found nothing: 30.5M of 30.9M waits in one
     * run were these. No need to link and unlink a waiter just to report a timeout. */
    if (!waiter_try( w ) && args->timeout != ~(uint64_t)0 &&
        now_ns( (args->flags & NTSYNC_WAIT_REALTIME) ? CLOCK_REALTIME : CLOCK_MONOTONIC ) >= args->timeout)
    {
        g_polls++;
        ms_unlock( &old );
        errno = ETIMEDOUT;
        return -1;
    }
    if (!g_spin_loaded)   /* ml1122: madeira.cfg madsync-spin-us (default 0 = sleep at once) */
    {
        long long us = madeira_cfg_int( "madsync-spin-us", 0 );
        if (us < 0) us = 0; if (us > 200) us = 200;
        g_spin_ticks = (uint64_t)(us * ms_ticks_per_us());
        g_spin_loaded = 1;
        dprintf( 2, "[madsync] ml1122 spin before sleeping: %lld us\n", us );
    }
    if (!w->done && g_spin_ticks)
    {
        /* Lock-free, like the ml1063 poll: every object is pinned by the
         * caller's cached descriptor, and a stale read only costs a re-check. */
        uint64_t end = mach_absolute_time() + g_spin_ticks;
        int ready = 0;
        g_spin_tries++;
        ms_unlock( &old );
        while (!ready && mach_absolute_time() < end)
        {
            for (i = 0; i < n && !ready; i++) ready = obj_ready( w->objs[i], w->owner );
            if (!ready) __asm__ __volatile__( "yield" );
        }
        ms_lock( &old );
        if (ready && waiter_try( w )) g_spin_hits++;
    }
    if (!w->done && g_nkick && ms_kick_take_locked( w->owner ))   /* ml1259: kicked while we spun */
    {
        ms_unlock( &old );
        *kicked = 1;
        return MS_KICKED;
    }
    if (!w->done)
    {
        const int realtime = (args->flags & NTSYNC_WAIT_REALTIME) != 0;
        uint64_t woke_t = 0;
        for (i = 0; i < n; i++)
        {
            struct ms_obj *o = w->objs[i];
            struct ms_link *l = &w->links[i];
            o->refs++;                                   /* the object outlives a close during the wait */
            l->w = w; l->next = &o->head; l->prev = o->head.prev;
            o->head.prev->next = l; o->head.prev = l;
        }
        t->waiting = 1;
        ms_sleepers_add( t );
        g_blocked++;
        while (!w->done && !w->kicked)
        {
            kern_return_t kr;
            if (args->timeout == ~(uint64_t)0)
            {
                ms_unlock( &old );                       /* asleep with NO lock held and signals deliverable */
                kr = semaphore_wait( sem );
                woke_t = mach_absolute_time();
            }
            else
            {
                uint64_t now = now_ns( realtime ? CLOCK_REALTIME : CLOCK_MONOTONIC ), left;
                mach_timespec_t rel;
                if (now >= args->timeout) break;
                left = args->timeout - now;
                rel.tv_sec  = (unsigned int)(left / 1000000000ull > 0x7fffffffu ? 0x7fffffffu : left / 1000000000ull);
                rel.tv_nsec = (clock_res_t)(left % 1000000000ull);
                ms_unlock( &old );
                kr = semaphore_timedwait( sem, rel );
                woke_t = mach_absolute_time();
            }
            (void)kr;                                    /* ABORTED, TIMED_OUT, a stale post: all re-checked under the lock */
            ms_lock( &old );
        }
        if (w->done && w->wake_t && woke_t > w->wake_t) ms_lat_note( woke_t - w->wake_t );   /* ml1122 */
        if (!w->done) waiter_try( w );                   /* a last look before declaring a timeout */
        waiter_unlink( w );
        ms_release_refs_locked( w );
        ms_sleepers_del( t );
        t->waiting = 0;
        if (w->kicked)
        {
            *kicked = 1;
            if (!w->done) { ms_unlock( &old ); return MS_KICKED; }
        }
    }

    if (w->done)
    {
        args->index = w->index;
        if (w->ownerdead) { errno = EOWNERDEAD; ret = -1; }
    }
    else { errno = ETIMEDOUT; ret = -1; }
    ms_unlock( &old );
    return ret;
}

/* ml1259: a kicked wait runs the thread's system APCs, then waits again with the
 * same absolute deadline (one of them may be what sets the object it waits on). */
static int do_wait( struct ntsync_wait_args *args, int all )
{
    for (;;)
    {
        int kicked = 0, ret = do_wait_once( args, all, &kicked );
        if (kicked) ms_run_system_apcs();
        if (ret != MS_KICKED) return ret;
    }
}

/* ---- the "driver" entry point --------------------------------------------- */
int madsync_ioctl( int fd, unsigned long req, void *arg )
{
    sigset_t ms_old;
    struct ms_obj *o;
    int ret = 0;

    if ((req & 0xffff0000ul) != 0x4d530000ul) return ioctl( fd, req, arg );

    switch (req)
    {
    case NTSYNC_IOC_CREATE_SEM:
    {
        struct ntsync_sem_args *a = arg;
        if (a->count > a->max) { errno = EINVAL; return -1; }
        return obj_new( MS_SEM, a->count, a->max );
    }
    case NTSYNC_IOC_CREATE_MUTEX:
    {
        struct ntsync_mutex_args *a = arg;
        if (!a->owner != !a->count) { errno = EINVAL; return -1; }
        return obj_new( MS_MUTEX, a->owner, a->count );
    }
    case NTSYNC_IOC_CREATE_EVENT:
    {
        struct ntsync_event_args *a = arg;
        return obj_new( MS_EVENT, a->manual, a->signaled );
    }
    case NTSYNC_IOC_WAIT_ANY: return do_wait( arg, 0 );
    case NTSYNC_IOC_WAIT_ALL: return do_wait( arg, 1 );
    }

    ms_lock( &ms_old );
    if (!(o = obj_of( fd ))) { ms_unlock( &ms_old ); errno = EBADF; return -1; }
    switch (req)
    {
    case NTSYNC_IOC_SEM_RELEASE:
    {
        uint32_t *count = arg, prev = o->a;
        if (o->type != MS_SEM) { errno = EINVAL; ret = -1; break; }
        if ((uint64_t)prev + *count > o->b) { errno = EOVERFLOW; ret = -1; break; }
        o->a = prev + *count; *count = prev;
        obj_wake( o );
        break;
    }
    case NTSYNC_IOC_SEM_READ:
    {
        struct ntsync_sem_args *a = arg;
        if (o->type != MS_SEM) { errno = EINVAL; ret = -1; break; }
        a->count = o->a; a->max = o->b;
        break;
    }
    case NTSYNC_IOC_MUTEX_UNLOCK:
    {
        struct ntsync_mutex_args *a = arg;
        if (o->type != MS_MUTEX) { errno = EINVAL; ret = -1; break; }
        if (!a->owner) { errno = EINVAL; ret = -1; break; }
        if (o->a != a->owner) { errno = EPERM; ret = -1; break; }
        a->count = o->b;
        if (!--o->b) { o->a = 0; obj_wake( o ); }
        break;
    }
    case NTSYNC_IOC_MUTEX_KILL:
    {
        uint32_t owner = *(uint32_t *)arg;
        if (o->type != MS_MUTEX) { errno = EINVAL; ret = -1; break; }
        if (!owner) { errno = EINVAL; ret = -1; break; }
        if (o->a != owner) { errno = EPERM; ret = -1; break; }
        o->ownerdead = 1; o->a = 0; o->b = 0;
        obj_wake( o );
        break;
    }
    case NTSYNC_IOC_MUTEX_READ:
    {
        struct ntsync_mutex_args *a = arg;
        if (o->type != MS_MUTEX) { errno = EINVAL; ret = -1; break; }
        a->owner = o->a; a->count = o->b;
        if (o->ownerdead) { errno = EOWNERDEAD; ret = -1; }
        break;
    }
    case NTSYNC_IOC_EVENT_SET:
    case NTSYNC_IOC_EVENT_PULSE:
    {
        uint32_t prev = o->b;
        if (o->type != MS_EVENT) { errno = EINVAL; ret = -1; break; }
        o->b = 1;
        obj_wake( o );
        if (req == NTSYNC_IOC_EVENT_PULSE) o->b = 0;
        *(uint32_t *)arg = prev;
        break;
    }
    case NTSYNC_IOC_EVENT_RESET:
    {
        uint32_t prev = o->b;
        if (o->type != MS_EVENT) { errno = EINVAL; ret = -1; break; }
        o->b = 0;
        *(uint32_t *)arg = prev;
        break;
    }
    case NTSYNC_IOC_EVENT_READ:
    {
        struct ntsync_event_args *a = arg;
        if (o->type != MS_EVENT) { errno = EINVAL; ret = -1; break; }
        a->manual = o->a; a->signaled = o->b;
        break;
    }
    default: errno = ENOTTY; ret = -1; break;
    }
    if ((g_waits & 0x3ffff) == 0x3ffff)
    {
        g_waits++;
        dprintf( 2, "[madsync] ml1058 %llu waits (%llu slept, %llu empty polls), %llu wakes, %llu objects created, table %u\n",
                 g_waits, g_blocked, g_polls, g_wakes, g_creates, g_cap );
        dprintf( 2, "[madsync] ml1122 wake latency: %llu samples, avg %.1f us, <5us %llu, <20 %llu, <50 %llu, <100 %llu, <500 %llu, >=500 %llu; "
                 "lock contended %llu times, %.1f ms waiting; spin %llu tries, %llu hits\n",
                 g_lat_n, g_lat_n ? g_lat_ticks / ms_ticks_per_us() / g_lat_n : 0.0,
                 g_lat_hist[0], g_lat_hist[1], g_lat_hist[2], g_lat_hist[3], g_lat_hist[4], g_lat_hist[5],
                 g_contended, g_lock_wait_ticks / ms_ticks_per_us() / 1000.0, g_spin_tries, g_spin_hits );
        dprintf( 2, "[madsync] ml1259 system APC kicks %llu (%llu woke a wait), %llu runs, %u pending\n",
                 g_kicks, g_kick_wakes, g_kick_runs, g_nkick );
    }
    ms_unlock( &ms_old );
    return ret;
}
