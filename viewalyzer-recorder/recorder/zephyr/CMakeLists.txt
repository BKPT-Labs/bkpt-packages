# Copyright 2025-2026 BKPT, Inc.
# SPDX-License-Identifier: Apache-2.0

if(CONFIG_VIEWALYZER)
  get_filename_component(VIEWALYZER_MODULE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)

  if(CONFIG_VIEWALYZER_TRANSPORT_RTT)
    set(VIEWALYZER_TRANSPORT_VALUE 2)
  elseif(CONFIG_VIEWALYZER_TRANSPORT_RAMBUF)
    set(VIEWALYZER_TRANSPORT_VALUE 4)
  else()
    set(VIEWALYZER_TRANSPORT_VALUE 1)
  endif()

  zephyr_library()

  zephyr_library_sources(
    ${VIEWALYZER_MODULE_DIR}/core/ViewAlyzer.c
    ${VIEWALYZER_MODULE_DIR}/zephyr/VA_Adapter_Zephyr.c
  )

  # Kconfig is the user interface on Zephyr; everything below the recorder
  # sees only the generic VA_TRACE_* switches, so the core, the adapter, and
  # tracing_user.h all read one vocabulary. zephyr_compile_definitions is
  # global, so the kernel TUs that expand the trace points get the identical
  # set - the two sides cannot disagree.
  zephyr_compile_definitions(
    VA_ENABLED=1
    VA_RTOS_SELECT=2
    VA_DEVICE_HEADER=cmsis_core.h
    VA_TRANSPORT=${VIEWALYZER_TRANSPORT_VALUE}
    VA_MAX_TASKS=${CONFIG_VIEWALYZER_MAX_TASKS}
    VA_MAX_SYNC_OBJECTS=${CONFIG_VIEWALYZER_MAX_SYNC_OBJECTS}
    VA_MAX_USER_EVENTS=${CONFIG_VIEWALYZER_MAX_USER_EVENTS}
    VA_MAX_GPIOS=${CONFIG_VIEWALYZER_MAX_GPIOS}
    VA_MAX_HEAPS=${CONFIG_VIEWALYZER_MAX_HEAP_GAUGES}
    VA_MAX_TASK_NAME_LEN=${CONFIG_VIEWALYZER_MAX_TASK_NAME_LEN}
    VA_AUTO_SETUP_INTERVAL_MS=${CONFIG_VIEWALYZER_AUTO_SETUP_INTERVAL_MS}
    VA_STACK_USAGE_HEARTBEAT_MS=${CONFIG_VIEWALYZER_STACK_USAGE_HEARTBEAT_MS}
    VA_TRANSPORT_BUFFERED=$<IF:$<BOOL:${CONFIG_VIEWALYZER_BUFFERED}>,1,0>

    # ── Trace categories ───────────────────────────────────────────
    VA_TRACE_TASKS=$<IF:$<BOOL:${CONFIG_VIEWALYZER_TRACE_THREADS}>,1,0>
    VA_TRACE_STACK_USAGE=$<IF:$<BOOL:${CONFIG_VIEWALYZER_STACK_USAGE}>,1,0>
    VA_TRACE_ISRS=$<IF:$<BOOL:${CONFIG_VIEWALYZER_TRACE_ISRS}>,1,0>
    VA_TRACE_MUTEXES=$<IF:$<BOOL:${CONFIG_VIEWALYZER_TRACE_MUTEXES}>,1,0>
    VA_TRACE_MUTEX_CONTENTION=$<IF:$<BOOL:${CONFIG_VIEWALYZER_TRACE_MUTEX_CONTENTION}>,1,0>
    VA_TRACE_SEMAPHORES=$<IF:$<BOOL:${CONFIG_VIEWALYZER_TRACE_SEMAPHORES}>,1,0>
    VA_TRACE_QUEUES=$<IF:$<BOOL:${CONFIG_VIEWALYZER_TRACE_MESSAGE_QUEUES}>,1,0>
    VA_TRACE_EVENT_FLAGS=$<IF:$<BOOL:${CONFIG_VIEWALYZER_TRACE_EVENT_FLAGS}>,1,0>
    VA_TRACE_WORK=$<IF:$<BOOL:${CONFIG_VIEWALYZER_TRACE_WORK}>,1,0>
    VA_TRACE_SLEEP=$<IF:$<BOOL:${CONFIG_VIEWALYZER_TRACE_SLEEP}>,1,0>
    VA_TRACE_TIMERS=$<IF:$<BOOL:${CONFIG_VIEWALYZER_TRACE_TIMERS}>,1,0>
    VA_TRACE_RTOS_HEAPS=$<IF:$<BOOL:${CONFIG_VIEWALYZER_TRACE_HEAPS}>,1,0>
    VA_TRACE_PM=$<IF:$<BOOL:${CONFIG_VIEWALYZER_TRACE_PM}>,1,0>
    VA_TRACE_USER_VALUES=$<IF:$<BOOL:${CONFIG_VIEWALYZER_TRACE_USER_VALUES}>,1,0>
    VA_TRACE_USER_EVENTS=$<IF:$<BOOL:${CONFIG_VIEWALYZER_TRACE_USER_EVENTS}>,1,0>
    VA_TRACE_STRINGS=$<IF:$<BOOL:${CONFIG_VIEWALYZER_TRACE_STRINGS}>,1,0>
    VA_TRACE_GPIO=$<IF:$<BOOL:${CONFIG_VIEWALYZER_TRACE_GPIO}>,1,0>
    VA_TRACE_COUNTERS=$<IF:$<BOOL:${CONFIG_VIEWALYZER_TRACE_COUNTERS}>,1,0>
    VA_TRACE_HEAP_METRICS=$<IF:$<BOOL:${CONFIG_VIEWALYZER_TRACE_HEAP_METRICS}>,1,0>

    # Zephyr has no task-notification concept, so the category is reported
    # off rather than claiming a capability the build can never deliver.
    VA_TRACE_TASK_NOTIFICATIONS=0
  )

  if(CONFIG_VIEWALYZER_BUFFERED)
    zephyr_compile_definitions(VA_BUFFER_SIZE=${CONFIG_VIEWALYZER_BUFFER_SIZE})
  endif()

  if(CONFIG_VIEWALYZER_TRANSPORT_RTT)
    zephyr_compile_definitions(
      VA_RTT_CHANNEL=${CONFIG_VIEWALYZER_RTT_CHANNEL}
      VA_CONFIGURE_RTT=$<IF:$<BOOL:${CONFIG_VIEWALYZER_CONFIGURE_RTT}>,1,0>
      VA_RTT_BUFFER_SIZE=${CONFIG_VIEWALYZER_RTT_BUFFER_SIZE}u
      VA_RTT_MODE=${CONFIG_SEGGER_RTT_MODE}
    )
  endif()

  if(CONFIG_VIEWALYZER_TRANSPORT_RAMBUF)
    # Retention mode: WRAP (post-mortem snapshot) > BLOCK (lossless) > DROP.
    # Kconfig marks WRAP and BLOCK mutually exclusive.
    if(CONFIG_VIEWALYZER_RAMBUF_WRAP)
      set(VIEWALYZER_RAMBUF_MODE_VALUE 2)
    elseif(CONFIG_VIEWALYZER_RAMBUF_BLOCK)
      set(VIEWALYZER_RAMBUF_MODE_VALUE 1)
    else()
      set(VIEWALYZER_RAMBUF_MODE_VALUE 0)
    endif()
    zephyr_compile_definitions(
      VA_RAMBUF_SIZE=${CONFIG_VIEWALYZER_RAMBUF_SIZE}u
      VA_RAMBUF_MODE=${VIEWALYZER_RAMBUF_MODE_VALUE}
      VA_RAMBUF_BUSY_IDLE=$<IF:$<BOOL:${CONFIG_VIEWALYZER_RAMBUF_BUSY_IDLE}>,1,0>
    )
  endif()

  if(CONFIG_VIEWALYZER_SNAPSHOT)
    zephyr_compile_definitions(
      VA_SNAPSHOT=1
      VA_SNAPSHOT_SIZE=${CONFIG_VIEWALYZER_SNAPSHOT_SIZE}u
    )
  endif()

  # Names area beside the snapshot ring (both PM ring shapes).
  if(CONFIG_VIEWALYZER_RAMBUF_WRAP OR CONFIG_VIEWALYZER_SNAPSHOT)
    zephyr_compile_definitions(
      VA_SNAPSHOT_SETUP_SIZE=${CONFIG_VIEWALYZER_SNAPSHOT_SETUP_SIZE}u
    )
  endif()

  if(CONFIG_VIEWALYZER_ALLOW_DISABLE_INTERRUPTS)
    zephyr_compile_definitions(VA_ALLOWED_TO_DISABLE_INTERRUPTS=1)
  else()
    zephyr_compile_definitions(VA_ALLOWED_TO_DISABLE_INTERRUPTS=0)
  endif()

  # Our tracing_user.h must be found BEFORE Zephyr's stock subsys/tracing/user
  # copy: it has its own include guard and pulls the stock header in via
  # #include_next, then re-points selected trace macros. The mutex/heap trace
  # points expand inside Zephyr's OWN kernel TUs (kernel/mutex.c, the heap
  # code) via kernel.h -> tracing/tracing.h -> `#include "tracing_user.h"`, so
  # our dir must precede subsys/tracing/user on the include path of *those*
  # TUs too.
  #
  # `include_directories(BEFORE ...)` is directory-scoped to this module and
  # does NOT reach the `kernel` target, so it loses there. Prepend to the
  # global zephyr_interface include path instead. Note: zephyr_include_
  # directories(BEFORE ...) does not work - that helper hardcodes INTERFACE
  # before ${ARGV}, mis-placing BEFORE - so call target_include_directories
  # directly. No global -include is used, so nothing leaks <zephyr/kernel.h>
  # into Zephyr's bootstrap TUs (the lib/heap heap_constants.c hazard on >=4.3).
  target_include_directories(zephyr_interface BEFORE INTERFACE
    ${VIEWALYZER_MODULE_DIR}/zephyr
  )

  zephyr_include_directories(
    ${VIEWALYZER_MODULE_DIR}/core
  )

  zephyr_library_include_directories(
    ${VIEWALYZER_MODULE_DIR}/core
    ${VIEWALYZER_MODULE_DIR}/zephyr
  )
endif()
