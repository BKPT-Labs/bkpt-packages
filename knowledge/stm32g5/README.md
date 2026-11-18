# STM32G564RE coverage

This package targets the STM32G564RET3 in LQFP64 on the NUCLEO-G564RE, MB2067 B02. The corpus includes the G564 SVD, package pinout, board descriptors, PLAY examples, and searchable pages of AN6401 Rev 2, DS14920 Rev 1, ES0656 Rev 1, RM0514 Rev 1 and UM3659 Rev 1. The DFP and SDK catalogs can be queried before project creation.

## Executable profiles

| Profile | What the application receives |
| --- | --- |
| `play_logic_gate` | A two-pad AND, OR, XOR, NAND, NOR or XNOR on a PLAY1 LUT with an output pad |
| `play_software_gate` | A gate with one input pad, one software-controlled input, and an output pad |
| `play_signal_route` | A buffered or inverted route from an input pad to an output pad |
| `crc32_checksum` | The C5 CRC-32 component bound to the G5 CRC instance |
| `independent_watchdog` | The C5 watchdog component bound to G5 IWDG with explicit application start |
| `gpio_input`, `gpio_output` | Allocation of an exposed header pad for application-owned GPIO initialization |

The PLAY profiles use a 1:1 LSI kernel clock and expose input pulls and a minimum pulse width in kernel-clock cycles. Zero bypasses the filter. The allocator assigns the muxes, LUT, pads and software trigger needed by each requirement. Multiple requirements may share PLAY1. Complete initialization before relying on the outputs. Check board population and attached circuitry before driving a pin. PC13 shares the B1 button through SB5 on the nominal board.

For example, this requirement implements an XOR of the button input and a software trigger on the same pads as ST's board example. A solve request also supplies schema 1, target `NUCLEO-G564RE`, a context object and a behavior string.

```json
{
  "id": "gate",
  "capability": "play_software_gate",
  "parameters": {"function": "xor", "min_pulse_width": "0", "pull_a": "none"},
  "bindings": {"pad_a": "STM32G564RET3.PC13", "pad_out": "STM32G564RET3.PD11"}
}
```

The PLAY registers declared in the G564 SVD and ST's counter, alarm and low-power examples are available for source inspection. The generated profiles cover combinational logic. Registered state machines, timing guarantees, and physical board behavior remain unverified. Other C5 allocation and component families, including timers, serial buses, DMA, and ADC, still need G5-specific integration.

## Rebuild with bkpt-fw

Prepare a separate source directory containing the following external inputs. Preserve the original file bytes when cloning the repositories (`core.autocrlf=false`).

| Source directory or file | Required input |
| --- | --- |
| `STM32CubeG5/` | STMicroelectronics/STM32CubeG5 at `55256c5f5d7050ea2474fb006877c12bbdd09f4c`, including its pinned `arch/cmsis` and `utilities/syscalls` submodules |
| `stm32g5xx-dfp/` | STMicroelectronics/stm32g5xx-dfp at `2288c48393e70c4a787cd0c1b5bcaf163d67340f` |
| `stm32g5xx-drivers/` | STMicroelectronics/stm32g5xx-drivers at `6df73a63fe7e2b480df2a17fbee2c7657337442e` |
| `nucleo-g564re_hw-board/` | Contents of ST's installed `STMicroelectronics.nucleo-g564re_hw-board.2.0.0` pack, including its PDSC and Descriptors directory |
| `AN6401.pdf`, `DS14920.pdf`, `ES0656.pdf`, `RM0514.pdf`, `UM3659.pdf` | The revisions listed above, with hashes in `catalog/stm32g5.json` |

From this package directory, replace the capitalized paths below with absolute paths. Use an empty staging directory outside the repository. Page extraction uses PyMuPDF 1.28.2. Building firmware requires CMake 3.30 or later, Ninja, and the ARM GCC toolchain.

```text
python -B prepare_inputs.py SOURCE_ROOT
python -B tests/regenerate_graph.py --source-root SOURCE_ROOT --check --stage-root STAGE
bkpt-fw corpus build --manifest catalog/stm32g5.json --source-root SOURCE_ROOT --out INSTALL/knowledge.sqlite
bkpt-fw devices build --manifest imports/devices.json --source-root SOURCE_ROOT/stm32g5xx-dfp --out INSTALL/devices.sqlite
bkpt-fw --asset-root g5_hal_pack=SOURCE_ROOT/stm32g5xx-drivers components build --manifest imports/components.json --source-root SOURCE_ROOT/stm32g5xx-drivers --out INSTALL/components.sqlite
bkpt-fw architecture build --manifest graph.json --review graph.review.json --source-root STAGE --out INSTALL/capabilities.sqlite
```

For generation, register `foundation-manifest.json` as `g5-hal2-minimal` in the operator's foundation catalog with its exact SHA-256. Set the `g5_reference` asset root to `SOURCE_ROOT`. The board routes to the `stm32g5` corpus, and the SDK key for the component catalog is `stm32g5-hal2-2.0.0`. The foundation comes from ST's NUCLEO-G564RE no-OS template. Its idle application calls `mx_system_init()`; selected cards add their own initialization.
