/* Keep Windows thread priorities from taking guest threads off the
 * performance cores on iOS.
 *
 * Wine's server maps every Windows thread priority onto Mach scheduling
 * policies (apply_thread_priority in server/thread.c): latency and throughput
 * tiers, THREAD_EXTENDED_POLICY and THREAD_PRECEDENCE_POLICY. It does so for
 * every thread at init_thread, again for all of a process's threads once its
 * task port arrives, and on every SetThreadPriority. XNU's thread_policy_set
 * removes a thread's QoS class before it applies any of those flavors
 * ("Threads without static_param set reset their QoS when other policies are
 * applied", osfmk/kern/thread_policy.c). So the USER_INTERACTIVE class that
 * start_thread (thread_ios.c) and wine_process_thread (WineProcessBridge.m)
 * give each guest thread lasts only until the thread's own init_thread
 * request, and the game then runs as a QoS-less thread that the scheduler
 * mostly keeps on the efficiency cores. Stellar Blade's log showed 65 to 77%
 * of every guest thread's CPU time on E-cores.
 *
 * Every server file is compiled with this header force-included, the same
 * way as wineserver_ios_kill.h, so the one call site in the submodule's
 * thread.c reaches madeira_server_thread_policy_set (mach_ios.c) without
 * patching the submodule. That function drops the flavors that would strip
 * the QoS class and passes everything else through. madeira.cfg
 * wine-thread-policy = 1 restores Wine's own behaviour. */
#ifndef MADEIRA_THREAD_POLICY_IOS_H
#define MADEIRA_THREAD_POLICY_IOS_H
#ifdef WINE_IOS
/* Object-like, so the SDK's own prototype in <mach/thread_act.h> becomes
 * the prototype of the replacement too. */
#define thread_policy_set madeira_server_thread_policy_set
#endif
#endif
