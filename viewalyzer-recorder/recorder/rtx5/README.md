# Arm RTX5 adapter

This adapter targets the source build of **CMSIS-RTX 5.9.1**
(`b566de42a5bf77d8b080f521cefc38ec7c45a3cc`), GNU Armv7-M on Cortex-M4/M7,
with the CMSIS SysTick tick driver. It does not support precompiled RTX libraries,
TrustZone, RTX safety extensions, other exception ports, or replacement tick
drivers. RTX is used through its CMSIS-RTOS2 API.
All instrumented application threads and recorder calls must be privileged;
`osThreadUnprivileged` is outside this adapter's support scope. Interrupt
masking must remain enabled (`VA_ALLOWED_TO_DISABLE_INTERRUPTS=1`), including
for the native API-entry hooks that run before entering SVC.

## Build integration

1. Compile `core/ViewAlyzer.c`, `core/viewalyzer_cobs.c`, and
   `rtx5/VA_Adapter_RTX5.c`.
2. Use `rtx5/ports/irq_armv7m.S` **instead of** the upstream file of that name.
   Its Arm license and upstream revision are retained in the file. It records
   the final context-switch decision at the physical exception-return boundary.
3. Omit `Source/rtx_evr.c`. Force-include `ViewAlyzerRTX5Hook.h` in every C
   translation unit that builds or calls RTX, including the recorder. Add the
   recorder's `core` and `rtx5` directories to the include paths.
4. Use identical recorder configuration in the kernel, port, application and
   adapter: `VA_RTOS_SELECT=VA_RTOS_RTX5`, `VA_DEVICE_HEADER`,
   `VA_TRANSPORT`, and any category overrides. The assembly also needs the
   configuration header. Configuration-specific link symbols reject missing or
   mismatched kernel/port instrumentation.
5. Enable `OS_STACK_WATERMARK=1` for stack-use tracing. Do not enable Arm
   Event Recorder or `OS_EVR_INIT` alongside this adapter. Initialize the
   recorder before `osKernelInitialize()` so initial object creation is captured.
6. Reserve enough MSP for the recorder calls inside SVC, PendSV, SysTick and
   application interrupts. The examples reserve 8 KiB; size the stack for your
   own interrupt nesting and build. Stack watermark sampling runs on context
   switches and before a thread's stack is freed.

The adapter owns the strong `osRtxThreadBeforeFree` override. An application
with its own override must integrate its cleanup with this implementation;
two strong definitions deliberately fail to link.

## Events

Native hooks cover thread creation/destruction, running/ready/blocked/sleep
states, suspend/resume, priority inheritance/restoration, stack watermarks,
mutex ownership/contention, semaphore operations, message queues, event flags,
memory pools, the RTX variable-size allocator, and timer creation/start/stop.
Blocking operations report success or failure, including timeout and
cancellation; the wire format does not distinguish every failure cause. Explicit
task/exception identities preserve the recipient of a direct handoff.

RTX5 always includes detailed queue/flag packets when those categories are
enabled, because their explicit actors are necessary for correct attribution.
`VA_TRACE_QUEUE_DETAILS=0` and `VA_TRACE_EVENT_FLAG_DETAILS=0` do not suppress
them for RTX5. RTX's completed queue hooks do not
provide message priority, so the adapter does not claim FIFO ordering. Pool
traces use the memory-slab category. Variable-size heap records include allocator
overhead; they do not represent the internal fixed object pools.
RTX's internal timer message queue is also traced and occupies one object slot;
its unnamed queue shows timer-thread deliveries from SysTick.

The native hooks do not bracket timer callback execution. Include
`ViewAlyzerRTX5Hook.h` and call `VA_RTX5_TimerEnter(timer)` and
`VA_RTX5_TimerExit(timer)` around the callback body when callback timing is
needed. Keep that timer alive until after Exit. Both helpers compile out when
callback tracing is disabled.

Application IRQs use `VA_LogISRStart(id)` / `VA_LogISREnd(id)`; use the
exception number (`IRQn + 16`). The port covers SysTick (reserved id 1),
PendSV (reserved id 2) and SVCall (id 11). Thread flags use
`VA_TRACE_TASK_NOTIFICATIONS`: set/clear masks, snapshots, wait options and
success/failure retain the destination thread and explicit task/IRQ actor.
They occupy no synchronization-object registry slots. Task-state tracing also
records notification waits and join waits (including the joined task ID), even
when notification tracing is disabled. API argument-validation failures and
memory-pool free failures are not detailed events in this adapter.

The matching ViewAlyzer host decoder is required for the explicit-actor packet
types, including thread-flag packet 0x28. ITM/SWO and the ViewAlyzer RAM buffer use the existing recorder transport
configuration; RTT additionally requires the user's SEGGER RTT sources.
