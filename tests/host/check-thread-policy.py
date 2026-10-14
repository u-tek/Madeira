#!/usr/bin/env python3
"""Windows thread priorities leave a guest thread's QoS class alone; no Wine runs.

XNU removes a thread's QoS class before it applies any other thread policy, so
Wine's server, which maps every Windows priority onto Mach policies, took the
USER_INTERACTIVE class away from every guest thread at its init_thread. Checks:
  - every server file is compiled with thread_policy_ios.h force-included, and
    the header points thread_policy_set at madeira_server_thread_policy_set;
  - the submodule's thread.c still applies the four flavors that are dropped;
  - compiled against stubs, the replacement drops the latency, throughput,
    extended and precedence flavors, passes a time constraint through, and
    passes everything through with madeira.cfg wine-thread-policy = 1.
Needs python3 and a C compiler.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
build = (root / 'build/wineserver/build.sh').read_text()
header = (root / 'build/wineserver/thread_policy_ios.h').read_text()
mach = (root / 'build/wineserver/mach_ios.c').read_text()

flags = build[build.index('CC_FLAGS=('):build.index('compile_one()')]
assert '-include "$BUILD_DIR/thread_policy_ios.h"' in flags
assert '#define thread_policy_set madeira_server_thread_policy_set' in header

thread_c = root / 'wine/server/thread.c'
if thread_c.exists():
    apple = thread_c.read_text()
    apple = apple[apple.index('#elif defined(__APPLE__)'):]
    for flavor in ('THREAD_LATENCY_QOS_POLICY', 'THREAD_THROUGHPUT_QOS_POLICY',
                   'THREAD_EXTENDED_POLICY', 'THREAD_PRECEDENCE_POLICY'):
        assert f'thread_policy_set( thread_port, {flavor},' in apple, flavor
    print('ok   thread.c applies the four flavors through thread_policy_set')
else:
    print('skip thread.c check: the wine submodule is not checked out')

start = mach.index('#ifdef WINE_IOS\n/* The replacement thread_policy_ios.h')
fn = mach[start:mach.index('#endif  /* WINE_IOS */', start)]
fn = fn[fn.index('#undef thread_policy_set'):]

code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef unsigned thread_act_t; typedef int thread_policy_flavor_t;
typedef int *thread_policy_t; typedef unsigned mach_msg_type_number_t;
typedef int kern_return_t;
#define KERN_SUCCESS 0
#define THREAD_EXTENDED_POLICY 1
#define THREAD_TIME_CONSTRAINT_POLICY 2
#define THREAD_PRECEDENCE_POLICY 3
#define THREAD_LATENCY_QOS_POLICY 7
#define THREAD_THROUGHPUT_QOS_POLICY 8
static int cfg_value, calls, last_flavor;
static int madeira_cfg_bool( const char *key, int dflt )
{ assert( !strcmp( key, "wine-thread-policy" ) && !dflt ); return cfg_value; }
kern_return_t thread_policy_set( thread_act_t t, thread_policy_flavor_t f, thread_policy_t p, mach_msg_type_number_t c )
{ (void)t; (void)p; (void)c; calls++; last_flavor = f; return 5; }
#include "thread_policy_ios.h"
''' + fn + r'''
int main( int argc, char **argv )
{
    int x = 0, f;
    static const int dropped[] = { THREAD_LATENCY_QOS_POLICY, THREAD_THROUGHPUT_QOS_POLICY,
                                   THREAD_EXTENDED_POLICY, THREAD_PRECEDENCE_POLICY };
    cfg_value = argc > 1;
    for (f = 0; f < 4; f++)
        assert( madeira_server_thread_policy_set( 1, dropped[f], &x, 1 ) == (cfg_value ? 5 : KERN_SUCCESS) );
    assert( calls == (cfg_value ? 4 : 0) );
    assert( madeira_server_thread_policy_set( 1, THREAD_TIME_CONSTRAINT_POLICY, &x, 4 ) == 5 );
    assert( last_flavor == THREAD_TIME_CONSTRAINT_POLICY );
    return 0;
}
'''

with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 't.c'
    exe = Path(tmp) / 't'
    src.write_text(code)
    subprocess.run(['cc', '-Wall', '-Werror', '-Wno-unused-function', '-DWINE_IOS=1',
                    '-I', str(root / 'build/wineserver'), str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, capture_output=True)
    print('ok   the four flavors are dropped, a time constraint goes through')
    subprocess.run([str(exe), 'wine'], check=True, capture_output=True)
    print('ok   wine-thread-policy = 1 passes every flavor through')
print('PASS')
