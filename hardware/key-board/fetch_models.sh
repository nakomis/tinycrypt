#!/usr/bin/env bash
# Fetch the 3D models (STEP + WRL) for every LCSC part into footprints/lcsc.3dshapes/.
# They are large (the module's STEP alone is megabytes), so they are not committed: build.py --step
# runs this first. The footprints already carry each model's offset and rotation.
set -euo pipefail
cd "$(dirname "$0")"
MODELS=(
  C2913206  # ESP32-S3-MINI-1-N8
  C1518769  # ATECC608B (SOIC-8)
  C265419   # SM05B-SRSS-G-TB
  C7519     # USBLC6-2SC6 (SOT-23-6)
  C51118    # AP2112K (SOT-23-5)
  C318884   # TS-1187A
  C965555   # WS2812B-2020: C52917434 (the -V6) has no EasyEDA model; same 2020 package
  C25905    # R0402
  C1525     # C0402
  C19702    # C0603
)
mkdir -p footprints/lcsc.3dshapes
FAILED=()
for c in "${MODELS[@]}"; do
  # resumable: skip parts already fetched (recorded per LCSC number)
  [ -f "footprints/lcsc.3dshapes/.$c" ] && continue
  sleep 8   # be polite: easyeda.com rate-limits bursts with a 403
  # --3d alone writes only the model files (into <output>.3dshapes), not the footprint
  if .venv/bin/python -c '
import sys
from easyeda2kicad.easyeda.easyeda_api import EasyedaApi
_init = EasyedaApi.__init__
def init(self):
    _init(self)
    self.headers["User-Agent"] = "key-board-fetch"
EasyedaApi.__init__ = init
from easyeda2kicad.__main__ import main
sys.argv[0] = "easyeda2kicad"
sys.exit(main())
' --3d --lcsc_id="$c" --output footprints/lcsc; then
    touch "footprints/lcsc.3dshapes/.$c"
  else
    FAILED+=("$c")
  fi
done
if ((${#FAILED[@]})); then echo "FAILED: ${FAILED[*]}" >&2; exit 1; fi
