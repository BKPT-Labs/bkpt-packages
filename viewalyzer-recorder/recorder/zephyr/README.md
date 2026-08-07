# ViewAlyzerRecorder Zephyr

Use the Zephyr integration when you want the recorder core packaged as a Zephyr module with Kconfig-controlled transport and tracing options.

This path hooks Zephyr's tracing callbacks and emits native ViewAlyzer task, ISR, mutex, semaphore, queue, and sleep events without modifying Zephyr kernel sources.

## What the Zephyr Path Adds

On top of the core recorder APIs, the Zephyr adapter can trace:

- thread creation
- thread switch in and switch out
- ISR enter and exit
- mutex lock and unlock
- semaphore give and take
- message queue activity
- sleep calls such as `k_sleep()`, `k_msleep()`, and `k_usleep()`
- stack usage, when enabled

## Files

- `VA_Adapter_Zephyr.c` - Zephyr-specific adapter implementation
- `VA_Adapter_Zephyr.h` - public Zephyr helper declarations
- `tracing_user.h` - bridge that wires Zephyr tracing macros to the adapter
- `Kconfig` - user-facing configuration options
- `module.yml` - Zephyr module manifest
- `CMakeLists.txt` - module build integration

## How It Integrates

When `CONFIG_VIEWALYZER=y`, the module CMake file:

- compiles `core/ViewAlyzer.c`
- compiles `zephyr/VA_Adapter_Zephyr.c`
- sets `VA_RTOS_SELECT=VA_RTOS_ZEPHYR`
- maps the selected Zephyr transport option to `VA_TRANSPORT`
- force-includes `zephyr/tracing_user.h`

That means application code does not need to define `VA_RTOS_SELECT` manually when using the module.

## prj.conf Example

Every category defaults to `y`, so the minimum config below already traces
everything. Set the ones you do not need to `n` and they compile out
completely - events, packet builders, registry storage, and the Zephyr trace
points that feed them:

```conf
CONFIG_VIEWALYZER=y
CONFIG_TRACING_USER=y

# Choose one transport
CONFIG_VIEWALYZER_TRANSPORT_ITM=y
# CONFIG_VIEWALYZER_TRANSPORT_RTT=y
# CONFIG_VIEWALYZER_TRANSPORT_RAMBUF=y
```

The full category list, all `y` by default:

```conf
# Threads and ISRs
CONFIG_VIEWALYZER_TRACE_THREADS=y
CONFIG_VIEWALYZER_TRACE_ISRS=y
CONFIG_VIEWALYZER_STACK_USAGE=y

# Synchronization and message-passing objects
CONFIG_VIEWALYZER_TRACE_MUTEXES=y
CONFIG_VIEWALYZER_TRACE_MUTEX_CONTENTION=y
CONFIG_VIEWALYZER_TRACE_SEMAPHORES=y
CONFIG_VIEWALYZER_TRACE_MESSAGE_QUEUES=y
CONFIG_VIEWALYZER_TRACE_TIMERS=y
CONFIG_VIEWALYZER_TRACE_HEAPS=y
CONFIG_VIEWALYZER_TRACE_SLEEP=y
CONFIG_VIEWALYZER_TRACE_PM=y          # needs CONFIG_PM

# Your own instrumentation
CONFIG_VIEWALYZER_TRACE_USER_VALUES=y
CONFIG_VIEWALYZER_TRACE_USER_EVENTS=y
CONFIG_VIEWALYZER_TRACE_STRINGS=y
CONFIG_VIEWALYZER_TRACE_GPIO=y
CONFIG_VIEWALYZER_TRACE_COUNTERS=y
CONFIG_VIEWALYZER_TRACE_HEAP_METRICS=y
```

Two things worth knowing:

- Thread ids and names survive `CONFIG_VIEWALYZER_TRACE_THREADS=n`. Stack
  usage, sleep, and mutex contention all name threads, so registration (and
  the `THREAD_MONITOR` / `THREAD_NAME` support it needs) follows what is
  actually enabled, not the thread-switch category.
- `CONFIG_VIEWALYZER_TRACE_MUTEX_CONTENTION=y` works with
  `CONFIG_VIEWALYZER_TRACE_MUTEXES=n`: the mutex and both threads stay
  registered so the contention event can name them, and no lock/unlock events
  reach the wire.

The recorder reports the categories it was built with in every setup bundle,
so the ViewAlyzer app can say "mutex tracing is disabled in this firmware
build" rather than showing an empty view. There is a worked example fragment
in `Example-Projects/zephyr/STM32-Zephyr-VA-Module-Full-Demo/selective-tracing.conf`.

The module maps each Kconfig choice onto the generic `VA_TRACE_*` switch the
recorder core uses, and does it through `zephyr_compile_definitions`, which is
global - so the kernel translation units that expand the trace points get the
identical set and the two halves cannot disagree.

If you use RTT instead of ITM/SWO, add the Zephyr `segger` module to your west manifest and update it before building.

## Transport Notes

### ITM/SWO

Use `CONFIG_VIEWALYZER_TRANSPORT_ITM=y`.

If you also want Zephyr log output over SWO, enable the relevant log backend separately. That is optional and independent from the recorder itself.

### SEGGER RTT

Use:

```conf
CONFIG_VIEWALYZER_TRANSPORT_RTT=y
CONFIG_VIEWALYZER_RTT_CHANNEL=0
CONFIG_VIEWALYZER_CONFIGURE_RTT=y
CONFIG_VIEWALYZER_RTT_BUFFER_SIZE=4096
```

Disable `CONFIG_VIEWALYZER_CONFIGURE_RTT` if another part of your system owns RTT initialization.

## Application Startup

Your application still initializes the recorder explicitly:

```c
#include "ViewAlyzer.h"
#include "VA_Adapter_Zephyr.h"

void main(void)
{
	VA_Init(CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC);
	VA_Zephyr_RegisterExistingThreads();

	VA_RegisterUserTrace(1, "LoopTime", VA_USER_TYPE_GRAPH);
}
```

`VA_Zephyr_RegisterExistingThreads()` is important if your system creates threads before `VA_Init()` runs. It emits setup packets for already-existing threads so the host can map thread IDs to names.

## Board-Specific Configuration

If different boards need different transports, keep the choice in board config fragments rather than in a single shared `prj.conf`.

Example:

```conf
# boards/nucleo_g474re.conf
CONFIG_VIEWALYZER_TRANSPORT_ITM=y
```

```conf
# boards/nucleo_f446re.conf
CONFIG_VIEWALYZER_TRANSPORT_RTT=y
```

## Manual User Traces Still Apply

The Zephyr adapter handles scheduler and kernel-object events, but user instrumentation is still useful for application-specific detail:

```c
VA_RegisterUserEvent(1, "control_loop");

VA_EVENT_START(1);
control_loop();
VA_EVENT_END(1);
```

## Related Docs

- [../README.md](../README.md)
- [../core/README.md](../core/README.md)
- [../freertos/README.md](../freertos/README.md)