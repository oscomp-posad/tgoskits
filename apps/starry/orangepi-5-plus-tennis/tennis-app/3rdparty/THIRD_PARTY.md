# Third-party components

The tennis app vendors the libraries below so it builds and runs on the target
without fetching external blobs. Each is used unmodified (build glue only) and
keeps its upstream license. This directory is checked in on purpose; do not
treat it as generated.

| Component | Purpose | Upstream | License |
|---|---|---|---|
| **librga** | 2D image scaling / colour conversion (RGA) user library | https://github.com/airockchip/librga | Apache-2.0 — see `librga/LICENSE` |
| **Rockchip MPP** (`librockchip_mpp`) | MJPEG hardware decode via `/dev/mpp_service` | https://github.com/rockchip-linux/mpp | Apache-2.0 — see `mpp/LICENSE` |
| **RKNPU2 runtime** (`librknnrt`) | NPU inference runtime + C headers | https://github.com/airockchip/rknn-toolkit2 (rknpu2) | Rockchip proprietary runtime, redistributed per Rockchip terms — see `rknpu2/NOTICE` |
| **libjpeg-turbo** | CPU JPEG fallback decode | https://github.com/libjpeg-turbo/libjpeg-turbo | BSD-style / IJG / zlib — see `jpeg_turbo/LICENSE.md` |
| **stb_image** | lightweight image load (host self-test) | https://github.com/nothings/stb | Public domain / MIT — see `stb_image/LICENSE.txt` |

The RKNPU2 runtime (`librknnrt.so`) is a closed-source Rockchip binary; it is
redistributed unmodified as a prebuilt shared object the app links against, not
built from source here.
