# ncnn-android-yolov8 — Car Vision Data Source

A fork of [nihui/ncnn-android-yolov8](https://github.com/nihui/ncnn-android-yolov8),
itself built on [Tencent/ncnn](https://github.com/Tencent/ncnn), reworked to turn an
Android phone into a lightweight **perception data source** for a self-built car head
unit (HMI). The phone detects vehicles/pedestrians, estimates their distance and
direction, and outputs a compact binary stream — the HMI does the Tesla-style 3D
rendering on its own side.

## What changed vs. the original demo

- **No on-screen rendering.** The original demo draws bounding boxes on the camera
  preview; this version drops that pipeline and only produces data.
- **Monocular ranging + pose.** For each detection it computes:
  - `distance` — ground-plane method (`d = H·fx / (y_bottom - cy)`)
  - `bearing` — horizontal angle from the box center
  - `heading` — rough orientation from the box aspect ratio
- **Binary protocol output.** Results are packed into a little-endian binary frame
  (class / distance / bearing / heading) described in [protocol.md](protocol.md),
  intended to be sent over a CH341 USB-serial link to the HMI.
- **Built-in WebUI.** A tiny embedded HTTP server serves a live MJPEG preview plus
  Camera2 controls (AF mode / focus distance / AE mode / exposure / ISO / AWB /
  camera switching) for calibration and debugging. Open `http://<phone-ip>:8080`
  in a browser on the same network.

## Build

Same dependencies as the upstream project. Extract them into `app/src/main/jni` and
set the paths in `app/src/main/jni/CMakeLists.txt`:

1. [ncnn](https://github.com/Tencent/ncnn/releases) — `ncnn-YYYYMMDD-android-vulkan.zip`
2. [opencv-mobile](https://github.com/nihui/opencv-mobile) — `opencv-mobile-XYZ-android.zip`
3. (optional) [mesa-turnip driver](https://github.com/nihui/mesa-turnip-android-driver)
   for Vulkan on some Adreno devices

Then open the project with Android Studio and build.

## Notes

- Requires Camera2 (HAL3) support; very old devices may crash.
- `CAM_HEIGHT_M` and `CAM_FX_PX` in `app/src/main/jni/yolov8ncnn.cpp` must be
  calibrated for your mounting position before trusting the distance values.
- Camera parameter controls depend on device/lens support — some combinations may be
  silently ignored.
- The WebUI camera naming (`主摄`/`超广角`/`长焦`/`黑白`) is hardcoded for the
  OnePlus 9 Pro rear camera order.

## License

BSD 3-Clause, same as the original. Original work © THL A29 Limited (Tencent) and
[nihui/ncnn-android-yolov8](https://github.com/nihui/ncnn-android-yolov8).
