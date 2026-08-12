# ViewAlyzerRecorder FreeRTOS

Use the FreeRTOS adapter when you want the recorder core plus automatic RTOS event capture from FreeRTOS trace macros.

This is the right path for timeline views that need task switches, notifications, queues, semaphores, mutexes, and optional stack usage.

## What the Adapter Adds

On top of the core recorder APIs, the FreeRTOS adapter maps native kernel activity into ViewAlyzer packets for:

- task creation
- task switch in and switch out
- task notifications
- queues
- semaphores and mutexes
- mutex contention
- event groups
- software timers (create/start/stop/expire, with arm periods)
- explicit sleep (`vTaskDelay`/`vTaskDelayUntil`, suspend/resume)
- kernel heap (`pvPortMalloc`/`vPortFree`)
- tickless-idle power management (requires `configUSE_TICKLESS_IDLE=1`)
- stack usage, when enabled

Version notes: the adapter supports FreeRTOS v9.0.0 and later (v9 through
v11 verified), single-core only (SMP builds are rejected at compile time).
`k_work`-style deferred-work tracing is a Zephyr-only category and is
reported off on FreeRTOS builds.

One operational requirement: call `VA_TickOverflowCheck()` periodically
from THREAD context (a housekeeping loop is fine). Every FreeRTOS kernel
trace hook runs in ISR or critical-section context, where the recorder
must defer its periodic setup bundle - without a thread-context recorder
call, late-attaching hosts never receive names.

## Files

- `VA_Adapter_FreeRTOS.c` - FreeRTOS-specific adapter implementation
- `VA_Adapter_FreeRTOS.h` - adapter declaration layer included through the core
- `ViewAlyzerFreeRTOSHook_Common.h` - every trace hook except task notifications
- `ViewAlyzerFreeRTOSHook.h` - include this from `FreeRTOSConfig.h` on FreeRTOS before 10.4
- `ViewAlyzerFreeRTOSHook_V10_4_Plus.h` - include this from `FreeRTOSConfig.h` on 10.4+

The two version-specific headers differ only in the task-notification macro
signatures (10.4 added the notification index); everything else lives in the
common header, so the two can never drift.

## Both halves must share one configuration

Your **kernel** compiles the trace hooks (`FreeRTOSConfig.h` includes the hook
header), while `ViewAlyzer.c` compiles into the recorder. Both read
`ViewAlyzerConfig.h`, so the switches cannot disagree at the source level -
but only if the configuration actually reaches both targets. Use `-D`,
`-include`, or `VA_CONFIG_HEADER`. A define written directly inside
`FreeRTOSConfig.h` is invisible to `ViewAlyzer.c`.

Getting this wrong has two outcomes, and only one of them is loud:

| Situation | Result |
|---|---|
| core gated off, kernel still calls | undefined reference at link - loud, fix it |
| kernel gated off, core gated on | **silent**: no hooks installed, zero events, no error |

## Selective tracing on FreeRTOS

FreeRTOS defines no `traceGIVE_MUTEX` / `traceTAKE_MUTEX` - the kernel never
invokes them. **All** non-recursive mutex and semaphore traffic flows through
the shared `traceQUEUE_SEND` / `traceQUEUE_RECEIVE` hooks, so mutexes,
semaphores, and queues share one pair of hooks and are separated at run time
by object type.

Consequences worth knowing:

- With every one of those categories off, the hooks are not installed at all
  and the cost is genuinely zero (the kernel keeps its own no-op defaults).
- With queues on and mutexes off, a mutex operation still enters the hook far
  enough to classify the object and return. That residual is imposed by
  FreeRTOS's shared trace points, not by the recorder: it is a null check on
  `pcHead` plus one `ucQueueType` load, ahead of the pending-bundle service
  and the interrupt mask that are the real cost of the hook.
- `configUSE_TRACE_FACILITY=1` is required for reliable filtering. Without it
  `Queue_t` has no `ucQueueType` field and mutexes cannot be told apart from
  semaphores; the adapter emits a `#warning` saying so.
- `VA_TRACE_MUTEX_CONTENTION=1` works with `VA_TRACE_MUTEXES=0`: the mutex and
  both tasks stay registered so the contention event can name them, and no
  give/take events reach the wire.

See the [top-level README](../README.md) for the full category list.

## Build Defines

Set the recorder mode to FreeRTOS and name your part's CMSIS device header
(a bare token: no quotes, no angle brackets):

```c
VA_ENABLED=1
VA_RTOS_SELECT=VA_RTOS_FREERTOS
VA_DEVICE_HEADER=stm32g474xx.h
```

Then select a transport the same way you would for bare-metal:

```c
VA_TRANSPORT=ARM_ITM
```

or:

```c
VA_TRANSPORT=JLINK_RTT
```

## Source Files to Compile

Compile these files into your firmware:

- `core/ViewAlyzer.c`
- `freertos/VA_Adapter_FreeRTOS.c`
- `core/viewalyzer_cobs.c` - ONLY for `CUSTOM_TRANSPORT` builds (COBS
  framing); ITM, RTT, and RAM-buffer builds do not use it

Add both `core/` and `freertos/` to the include path. If you use the
recommended `-DVA_CONFIG_HEADER=va_config.h` flow, the directory holding
your `va_config.h` must be on the KERNEL's include path too (the kernel
compiles the trace hooks and with them the configuration).

## FreeRTOSConfig.h Integration

Include the correct hook header from `FreeRTOSConfig.h` so the trace macros call into the recorder.

For FreeRTOS 10.4 and newer:

```c
#include "ViewAlyzerFreeRTOSHook_V10_4_Plus.h"
```

For older macro signatures:

```c
#include "ViewAlyzerFreeRTOSHook.h"
```

The hook headers define the `traceTASK_*`, queue, mutex, semaphore, and notification macros used by the adapter.

## FreeRTOS Options That Matter

These options affect how much detail the adapter can provide:

- `configUSE_TRACE_FACILITY=1` gives the adapter reliable queue object typing
- `INCLUDE_uxTaskGetStackHighWaterMark` enables stack usage reporting. The
  hook header defaults it to 1 when stack tracing is on; setting it to 0
  above the hook include disables stack packets entirely (no data, rather
  than wrong data)
- `INCLUDE_xSemaphoreGetMutexHolder=1` or `INCLUDE_xQueueGetMutexHolder=1` enables mutex contention ownership detection

If those options are off, the recorder still works, but the missing data stays unavailable.

## Object Names, Recursive Mutexes, and Late Init

- Names given with `vQueueAddToRegistry()` are picked up and replace the
  auto-generated names ("Queue", "BinSem", ...) in the viewer.
- Recursive mutexes appear as one take at first acquisition and one give at
  final release. Nested takes and gives change no ownership and emit
  nothing; a timed-out take emits no take event, only a failed-op event.
- Tasks created before `VA_Init()` are registered lazily on their first
  switch-in. They keep their real names, but their priority and stack size
  are unknown (reported as 0) and they get no stack-usage samples - call
  `VA_Init()` before creating tasks to get full data.

## Startup Sequence

Initialize the recorder after clocks and the selected transport backend are ready:

```c
#include "ViewAlyzer.h"

void app_init(void)
{
    VA_Init(SystemCoreClock);
    VA_RegisterUserTrace(1, "CpuLoad", VA_USER_TYPE_GRAPH);
}
```

User traces still work exactly the same as in bare-metal mode. The difference is that task and sync-object events are now emitted automatically through FreeRTOS.

## What You Usually Do Not Need to Call Manually

These functions are public because the hook layer needs them, but application code normally should not call them directly:

- `va_taskswitchedin()`
- `va_taskswitchedout()`
- `va_taskcreated()`
- `va_logtasknotifygive()`
- `va_logQueueObjectGive()`
- `va_logQueueObjectTake()`

Prefer the FreeRTOS trace macro path for scheduler and kernel-object events.

## Manual User Traces Still Apply

You can mix automatic RTOS events with explicit instrumentation:

```c
VA_RegisterUserEvent(1, "filter_block");

VA_EVENT_START(1);
filter_block();
VA_EVENT_END(1);
```

That is usually the best balance: let the adapter capture scheduler behavior automatically, then add user traces only around application-specific work.

## Related Docs

- [../README.md](../README.md)
- [../core/README.md](../core/README.md)
- [../zephyr/README.md](../zephyr/README.md)