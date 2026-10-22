# SPDX-License-Identifier: GPL-3.0-only
"""Cross-platform receiver for the PaperMono C153 BLE voice remote.

The package is split so that the wire protocol can be exercised without any
Bluetooth or audio dependency:

* :mod:`remote_mic.protocol` -- ATVV v1.0 framing, control events and IMA/DVI
  ADPCM, standard library only.
* :mod:`remote_mic.audio` -- ring buffer, DC blocker, gain and WAV capture,
  standard library only.
* :mod:`remote_mic.receiver` -- bleak + sounddevice glue; both third-party
  imports happen lazily inside the functions that need hardware.
* :mod:`remote_mic.cli` -- argument parsing and the ``papermono_mic`` entry
  point.
"""

from __future__ import annotations

__all__ = ["__version__"]

__version__ = "0.1.0"
