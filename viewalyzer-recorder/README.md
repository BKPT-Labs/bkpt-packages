# ViewAlyzer Recorder

Firmware recorder sources for the ViewAlyzer desktop app. The exact recorder
version, source commit, wire version and content hash are in [manifest.json](manifest.json).

Choose the integration for your firmware:

| Integration | Guide |
| --- | --- |
| Bare metal / shared core | [core](recorder/core/README.md) |
| FreeRTOS | [freertos](recorder/freertos/README.md) |
| Zephyr | [zephyr](recorder/zephyr/README.md) |
| ThreadX | [threadx](recorder/threadx/README.md) |
| Arm CMSIS-RTX5 | [rtx5](recorder/rtx5/README.md) |

Use the shared `recorder/core` files together with the adapter described in its
guide. RTX5 also requires the supplied `recorder/rtx5/ports/irq_armv7m.S` port.
Supported kernel versions and configuration requirements are documented in each
adapter's guide.

The recorder is distributed under [Apache-2.0](LICENSE). See [NOTICE](NOTICE)
and the source files for copyright and third-party notices.

## Updating the package

From the `bkpt-packages` repository root, using a clean checkout of the selected
ViewAlyzer release:

```sh
python -B tools/sync_recorder.py --source ../ViewAlyzer
python -B tools/sync_recorder.py --source ../ViewAlyzer --check
python -B -m unittest tools.test_sync_recorder
```

Commit the generated recorder, manifest and root `index.json` together. The
registry revision increases when recorder content changes; re-running a sync
on identical source is a no-op. `--check` fails on stale or incomplete payloads
without changing the package. Keep LICENSE and NOTICE aligned with the source.
