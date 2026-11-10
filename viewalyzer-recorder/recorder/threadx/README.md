# ThreadX adapter

Supported baselines are ST `stm32-mw-threadx` tag `v6.4.0_260327` and Eclipse
ThreadX tag `v6.5.2.202603_rel`, using the single-core GNU Cortex-M4 and Cortex-M7
ports. Cortex-M33 is included in build checks, but hardware capture validation
is not claimed for that port. Older releases and SMP are outside this adapter's
support scope.

1. Add `core/` and `threadx/` to the include directories of the application,
   ThreadX library, and assembler. Compile `core/ViewAlyzer.c`,
   `core/viewalyzer_cobs.c`, and `threadx/VA_Adapter_ThreadX.c`.
2. Set `TX_INCLUDE_USER_DEFINE_FILE` on every target. Include
   `ViewAlyzerThreadXHook.h` at the end of the project's `tx_user.h`.
   The supplied `tx_api_extension.h` uses ThreadX's official extension point.
   If the project already has this file, include the recorder extension from
   that file explicitly and retain its existing extensions.
3. Set `VA_RTOS_SELECT=VA_RTOS_THREADX`, the device header, and the usual recorder
   settings consistently on all targets. `VA_CONFIG_HEADER` is recommended.
   Native-hook and public-wrapper symbols encode the selected categories,
   transport, clock source and registry layout; inconsistent instrumented builds
   fail to link. Use an integer literal for `VA_MAX_TASK_NAME_LEN`.
   Rebuild the kernel after changing settings. A prebuilt kernel without these
   hooks cannot provide automatic tracing.
4. Call `VA_Init` **before** `tx_kernel_enter`, after clocks are configured.
   This includes the kernel's timer thread in creation metadata. Service
   `VA_TickOverflowCheck()` regularly from a thread and `VA_Drain()` there when
   using buffered output. Never call either from a suspended/locked kernel path.
5. For application ISR handlers, call `VA_ThreadX_ISR_Enter()` and
   `VA_ThreadX_ISR_Exit()` once at entry/exit. The supplied ThreadX low-level
   SysTick assembly commonly already calls `_tx_execution_isr_enter/exit`; do
   not add a second pair there. Assembly must see `tx_user.h`. SysTick maps to
   recorder ISR ID 1, PendSV to ID 2, and other maskable exceptions to IPSR
   (up to 255). NMI and HardFault hooks are ignored because PRIMASK cannot
   serialize them against interrupted recorder writes. Do not call other
   recorder APIs from these handlers either.
6. Call `VA_ThreadX_TimerEnter(&timer)` and `VA_ThreadX_TimerExit(&timer)` around
   each application timer callback. ThreadX has no callback entry/exit hook;
   omitted calls mean callback spans/fires are unavailable, not inferred.
   Internal thread-timeout timers are never reported as application timers.

Use `VA_TRANSPORT=ARM_ITM`, `RAM_BUFFER`, or `JLINK_RTT` with their normal core
configuration. RTT also requires the SEGGER source/license in the application.
On Cortex-M7 keep the RAM buffer in memory coherent with debug reads, or disable
D-cache for the test firmware. The recorder uses PRIMASK critical sections even
when ThreadX uses BASEPRI. Stack usage scans the configured fill pattern; it is
unavailable when ThreadX stack filling is disabled.
It is also unavailable with ST 6.4.0 random fill plus stack checking: that
kernel initializes the fill pattern and then zeroes its saved pattern field.
The normal constant-fill configuration and Eclipse 6.5.2 random fill are supported.

Enable `VA_TRACE_QUEUE_DETAILS=1` and `VA_TRACE_EVENT_FLAG_DETAILS=1` for precise
IPC attribution, ordering, occupancy and flag-mask views. A direct queue transfer
can happen while the sending or receiving thread is suspended: detailed events
carry its task ID explicitly. Legacy queue packets carry only the queue ID.
Block pools use `VA_TRACE_MEM_SLABS`; byte pools use `VA_TRACE_RTOS_HEAPS`.
ThreadX mutexes use the recursive-mutex type and report first acquisition/final
release. ThreadX priorities count down and creation stack sizes are bytes.
Hosts predating this adapter interpret those priorities and sizes incorrectly;
use the matching updated ViewAlyzer-RS host.

## Event ownership

| Events | Source and semantics |
| --- | --- |
| Task create/delete | Native validated kernel lifecycle hooks |
| Task switch/idle | Execution-change hooks; real task metadata and an Idle actor |
| Block/ready/sleep | Internal suspend/resume with cancellation/delayed-suspend handling; successful terminate/reset closes pending sleep and records suspended |
| Priority | Changed values only; native explicit set (1) before preemption, inheritance (2), disinheritance (3), including switch-in reconciliation |
| Queue successful send/receive | Native validated branch observations before wake; direct handoff and blocked front/back send ordering are preserved |
| Queue failures/flush | Public API results; rejected sends/receives never become transfers |
| Semaphore give / final mutex release | Native successful branch before wake |
| Semaphore/mutex take | Native validated immediate acquisitions; blocked acquisitions use the successful public return for waiter attribution |
| Semaphore/mutex failures | Public API results, including timeouts, aborts and deleted-object waits with identity retained across deletion/reuse |
| Mutex contention | Valid mutex get with a different owner and nonzero wait |
| Event flag set/clear | Native mutation intent before wake; actual returned bits and snapshots from public results |
| Byte/block alloc/free/fail | Public API results; release pool located by address range without dereferencing user memory |
| Timer arm/stop | Public successful API results, ticks converted to milliseconds |
| Timer callback / ISR spans | Explicit application boundary hooks, or existing port ISR callbacks |

The 39 wrappers cover thread delete/terminate/reset/resume/suspend/sleep/priority,
queue create/delete/send/front-send/receive/flush, mutex create/delete/get/put,
semaphore create/delete/get/put/ceiling-put, event-flags create/delete/get/set,
timer create/delete/activate/deactivate/change, and byte/block pool
create/delete/allocate/release. Read-only queries, notification callback
registration, time APIs and preemption-threshold changes produce no events.
Native thread creation brings the covered public-operation count to 40.
Object creation emits metadata; successful deletion retires its recorder registry
entry without a dedicated deletion packet. A timer change is observable in its
next arm event. Blocked mutex/semaphore take timestamps mark the API return;
their legacy packets have no field for a different thread's identity.
Kernel-internal `_tx_*` calls and prebuilt libraries bypass public result
wrappers; native scheduler and committed give/queue hooks still apply.

Blocking wrappers retain their original object ID on the calling thread's stack.
A `TX_DELETED` result therefore identifies the deleted object even if the waiter
resumes after registry retirement or same-address reuse, without dereferencing
the retired control block. Queue and flag details retain this identity too;
queue occupancy is unavailable after deletion. Initialize the recorder before
kernel entry as described above; restarting it with `VA_Init` while instrumented
API calls are outstanding is unsupported because that resets recording IDs.

The adapter owns execution-change callbacks and the inline event-trace macro.
Do not link execution profiling or enable a TraceX buffer (`tx_trace_enable`)
at the same time. MISRA/safety wrappers and SMP are rejected. This adapter does
not modify vendor kernel source or install background service threads.
The live `VA_Init` path references the kernel trace configuration even under
section garbage collection and LTO, rejecting normal prebuilt kernels
without trace support. A prebuilt TraceX-enabled kernel is still unsuitable:
rebuild every kernel object with this adapter's `tx_user.h` and configuration.

Host contract tests accept pinned source trees through `VA_THREADX_KERNELS`
(separated by the platform path separator):

```text
python -m unittest discover -s tests/recorder -p test_threadx.py
```

They compile the recorder/core category selections, every common kernel C source,
normal Cortex-M4/M7/M33 assembly and a C++ application, and check native event
ownership against real ThreadX Cortex-M4/M7 public types. M4 build checks use
the G474's FPv4-SP hard-float configuration. CI fetches both exact supported kernel commits
outside the checkout, uses a checksum-pinned Arm 14.3.rel1 compiler and sets
`VA_THREADX_REQUIRED=1`, so missing dependencies fail rather than skip.
Host runtime fixtures retain ASan/UBSan in CI. These are host tests;
firmware scheduling and transport acceptance require the board capture matrix.
