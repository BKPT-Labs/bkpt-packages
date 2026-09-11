# Trace source clocks

Revision 14 routes the STM32C562 trace gate through system AP0, matched by device ID `0x44E` at
`0x44024000`. It enables DBGMCU trace clocks, the SWO pin and debug during
stop/standby through `0x44024004`, then uses the standard Cortex-M TPIU.
The C562 core is on AP1, which returns zero for these vendor registers while
the core sleeps. Both the match and initialization therefore declare AP0.
This corrects the revision 13 route, which worked with a halted core.
This route has been exercised with the rack's NUCLEO-C562RE at 144 MHz core
and 2 MHz SWO. Other C5 device IDs are not implicitly enrolled.

`src-clock-div` on a `$prescaler` operation specifies a fixed integer
core-to-trace divider. The target prescaler is
`max(1, floor((cpu_hz / divider) / swo_hz)) - 1`.

Revision 12 introduces `src-clock` for the H723 RCC clock tree:

```json
{
  "kind": "stm32h7-rcc",
  "cfgr": "0x58024410",
  "d1cfgr": "0x58024418",
  "pllcfgr": "0x5802442C",
  "pll1divr": "0x58024430"
}
```

This object replaces `src-clock-div` on the H723 SWO CODR operation.
The interpreter reads RCC_CFGR.SWS and RCC_D1CFGR.D1CPRE. For HSI, CSI
or HSE, traceclkin uses the system source before D1CPRE, so
`trace_hz = cpu_hz * core_prescaler`.
When SWS selects PLL1, the core branch uses PLL1 P and trace uses PLL1 R:
`trace_hz = cpu_hz * core_prescaler * (DIVP1 + 1) / (DIVR1 + 1)`.
Here DIVP1 and DIVR1 are the encoded register fields; the core prescaler
uses the RCC D1CPRE lookup table. PLL1 R must be enabled.

The interpreter does not change RCC. Unsupported models, unreadable
registers, reserved sources and a disabled PLL1 R output are errors,
not permission to substitute a fixed ratio. Resolve clocks before vendor
writes, and use the same source for target and receiver-rate calculations.

See [ST AN5419 Figure 16](https://www.st.com/resource/en/application_note/dm00663674.pdf)
and RM0468 RCC register definitions. Native C and Rust consumers contain
matching oscillator, PLL, core-prescaler and failure regression cases.

Ship this catalog with a consumer that implements the model. Older
interpreters may ignore unknown JSON properties; replacing only their
catalog does not upgrade their clock handling. The legacy C++ app's
snapshot remains separate pending support in its interpreter.
