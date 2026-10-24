.. SPDX-License-Identifier: GPL-2.0-only

VIM4 offline regression fixtures
===============================

Run from a clean kernel checkout on a Linux ARM64 host with Python 3, a host
C compiler and Pillow (the JPEG fixture decodes its generated JPEG stream)::

  export VIM4_VENDOR=/path/to/common_drivers
  export VIM4_AUDIO_BASELINE=/path/to/pre-audio-series-audio-test.dtb
  python3 scripts/vim4/offline-tests.py --output /tmp/vim4-host-results

The vendor checkout can also be passed with ``--vendor``. Display fixtures
execute local vendor C fragments for comparison. The report records that
checkout's revision; it is never fetched or modified by this runner.

The audio baseline must be the compiled audio-test DTB from before the audio
route series, not a DTB compiled from the code under test. It can also be
passed with ``--audio-baseline``. Both digital-route and processing fixtures
compare the RX contract against it; the report records its SHA256. Audio
fixtures additionally require cpp and dtc.

The output directory must be new and outside the source tree. All tracked
``test_*.py`` fixtures in the supported driver directories run independently.
They compile selected production C functions against fake registers, DMA and
power callbacks; they never load a module or access a board. The JSON report
records the revision, dirty state, per-fixture status and elapsed time. A
fixture timeout or failure makes the command fail after collecting the
remaining results. These fixtures do not prove firmware or DMA bus behavior.

KUnit is an optional additional stage. Supply an existing configuration for
the tests being investigated; arm64 execution requires a suitable QEMU::

  python3 scripts/vim4/offline-tests.py --output /tmp/vim4-kunit-results \
    --kunit-config /path/to/.kunitconfig --kunit-arch arm64

An omitted KUnit stage is recorded as null, never as a successful KUnit run.
KUnit build products and its console log remain inside the result directory.

Patch export, strict checkpatch, DT schema checks and full Image/DTB/module
builds are orchestrated by ``packaging/kernel-edge/maintain.py`` in the
vim4-kvm integration repository. Those commands use an external build
directory and do not install, boot or publish artifacts.
