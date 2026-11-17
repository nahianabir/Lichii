# XeFG-VK Research and MVP

## Status

XeFG-VK is an experimental, presentation-only frame-generation research project. This branch is based on the repository's existing `feature/ocarina-render-scale-1080p` XeSS branch at `855c09d877e1720a58566b2c54bc597676ec0be3`. The requested ref `feature/ocarina-render-scale-1080` does not exist in this checkout; the actual branch name includes the trailing `p`.

| Stage | Status |
| --- | --- |
| MVP 1: compositor color capture | Implemented as queued GPU copies; not compiled or executed in Lichii yet |
| Vulkan device capability query | A750 driver queried with `vulkaninfo`; cooperative-matrix extension and feature exposed |
| Matrix shape query | Not verified on this machine; `vulkaninfo` did not print the shape list |
| XMX instruction execution | Not verified |
| MVP 2/3: motion estimation, interpolation, generated-frame presentation | XeFG mode reuses Lichii's Vulkan optical-flow/interpolation and bounded presentation pipeline; not compiled or run yet |
| A750 frame-generation performance | Not measured |

The host reports an Intel Arc A750, Intel Vulkan driver `101.9175`, `VK_KHR_cooperative_matrix` revision 2, `cooperativeMatrix=true`, compute-stage support, `VK_KHR_external_memory_win32`, and `VK_KHR_timeline_semaphore`. These properties establish that the driver exposes a candidate Vulkan matrix path; they do not prove a specific shape is supported or that a shader dispatch executes on XMX.

## Architecture

```mermaid
flowchart TD
    A[Emulated game render] --> B[Ocarina scene resources when available]
    B --> C[Camera and actor motion reconstruction]
    B --> D[XeSS-SR, optional and Ocarina-specific]
    C --> D
    D --> E[Layer composition and display post-processing]
    A --> E
    E --> F[Final Vulkan presentation image]
    F --> G[MVP 1: two-image color history capture]
    G --> H[PresentManager queue and swapchain]
    F --> I[XeFG 2x optical-flow MVP]
    I --> J[Half-resolution flow and compute interpolation]
    J --> K[Generated intermediate frame]
    K --> H
    J -. future .-> L[Replaceable XMX inference backend]
```

MVP 1 is inserted in `RendererVulkan::Composite`, immediately after `BlitScreen::DrawToFrame`. It copies the final `Frame::image` into a two-slot Vulkan image ring using the existing `Scheduler` command stream. The copy transitions the source from `GENERAL` to `TRANSFER_SRC_OPTIMAL`, copies into a history image, then restores both images to `GENERAL`. The copy is ordered before the existing `render_ready` semaphore is signaled and before `PresentManager` reads the frame. It does not read pixels back to the CPU, submit work on a second queue, or wait for generation.

Set `EDEN_XEFG_CAPTURE=1` before launching Lichii to enable this diagnostic capture. The default is off. Logs say “queued” deliberately: the current implementation does not wait for or verify GPU completion. On a render-size or format change, the scheduler is finished before history images are replaced; this is a rare, potentially blocking reconfiguration path.

The UI now offers `XeFG (Optical Flow, 2x)`. This mode uses the existing `Fsr3FrameGen` GPU optical-flow and interpolation implementation and the existing bounded presentation queue, capped at one generated frame per real frame. It is an MVP algorithm, not a neural model and not XMX inference. Inference is explicitly reported unavailable. When the existing flow-dump debug option is enabled, XeFG writes a one-shot, colorized half-resolution motion-vector image to the user data `lossless/debug` directory. That debug readback waits for the GPU and should not be used for performance measurements.

## Renderer and XeSS Data Flow

The generic Vulkan path is:

1. `RendererVulkan::Composite` obtains a presentation `Frame` from `PresentManager`.
2. `BlitScreen::DrawToFrame` composites guest framebuffer layers and display processing into `Frame::image`.
3. `Scheduler::Flush(*frame->render_ready)` submits the recorded rendering work and signals the frame-ready semaphore.
4. `PresentManager` copies or blits the frame to the swapchain and presents it. Its frame pool uses `render_ready`, `present_done`, and the scheduler's GPU timeline for resource and queue lifetime.

The presentation image already has transfer-source usage and ends the render pass in `GENERAL`. The final composition includes all layers that were supplied to the compositor; this capture point cannot distinguish game imagery from HUD, menus, or other overlays. Interpolating it would therefore interpolate UI too.

The personal XeSS-SR integration remains intact and is game-specific to Ocarina of Time:

- `OcarinaUpscaler::Impl` creates `XessUpscaler` for the selected XeSS backend. `XessUpscaler` dynamically loads the user-supplied Intel XeSS runtime, creates a Vulkan XeSS context, and schedules initialization and evaluation through `Scheduler`.
- The scene color is prepared in per-frame `OcarinaFrameSlot` images. The XeSS path uses an XeSS-specific color conversion image, the scene depth image, and a motion image in `VK_FORMAT_R16G16_SFLOAT`.
- `CameraMotionPass` reconstructs camera motion from current and previous transforms plus depth. When actor data is available, `ActorOverlay` adds actor motion. This is host-reconstructed motion, not a general source of motion vectors from arbitrary emulated games.
- XeSS writes an `R16G16B16A16_SFLOAT` output image. When the selected input/output geometry needs an intermediate, Lichii blits that result to the configured output extent. The output pass places the selected upscaler result in the Ocarina composite target; later compositor work sends the visible frame toward the swapchain.

The exact XeSS runtime and its model are not used by XeFG-VK. No XeSS-SR source files are changed by the MVP.

## Motion and Frame Inputs

| Input | Current availability |
| --- | --- |
| Current color | Available at the generic final compositor; captured by MVP 1 |
| Previous color | Available in the MVP 1 two-slot image history after two capture records |
| Depth | Available in the Ocarina-specific scene/upscaler path; not exposed by the generic compositor |
| Motion vectors | XeFG estimates image-space motion from consecutive final-compositor colors; Ocarina also has reconstructed camera/actor motion, not currently wired into generic XeFG |
| Camera/object transforms | Available only through game-specific Ocarina hooks used by its motion pass |
| Frame timing | Host presentation and scheduler timing exist; no XeFG timing metrics are implemented |
| Render/output resolution | Present in Ocarina upscaler inputs and in the presentation `Frame` dimensions |

The eventual motion-provider interface should allow the Ocarina reconstructed vectors, other game-specific providers, and image-space optical flow to be selected independently. The current XeFG mode uses image-space optical flow only and resets its history after host frame gaps over 250 ms. Scene-cut, menu, loading, and camera-discontinuity detection is not implemented yet.

For Ocarina, the lower-resolution scene inputs preserve depth and reconstructed motion and reduce the pixel count for optical flow or inference. Generating before XeSS would require a compatible upscale of the generated image and careful management of XeSS's temporal history. Generating after XeSS reduces dependence on the lower-resolution input path and has a sharp output image, but costs more pixels and receives already-upscaled content. The current final-compositor capture is later still: it includes UI and has no matching depth/motion. The correct production insertion point is therefore not selected; a game-scene insertion before UI composition is the preferred direction if its resources and synchronization can be exposed safely.

## XMX Backend Research

### Vulkan cooperative matrices

The real Vulkan extension is [`VK_KHR_cooperative_matrix`](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_cooperative_matrix.html). It defines cooperative SPIR-V matrix operations and a query for implementation-supported shapes and component types. The application must query both the extension/features and `vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR`; extension presence alone is insufficient. The Vulkan specification does not guarantee that an implementation maps those operations to a particular physical matrix engine.

Lichii already has capability querying and a correctness self-test for guest HMMA cooperative-matrix operations in `vulkan_device.cpp` and `vk_cooperative_hmma_test.cpp`. Its accepted 16x8x16 or 16x16x16 FP16 shapes are chosen for emulating a guest instruction; they are not an XeFG inference backend and the correctness self-test does not identify XMX instruction selection. When XeFG is selected, it now queries and logs the device's reported matrix shapes, types, and scope. The A750's shape list has not yet been observed in a Lichii run, and generated machine instructions remain unverified.

### Level Zero and SYCL

Intel's [`ZE_intel_device_module_dp_exp_properties_t`](https://github.com/intel/compute-runtime/blob/master/level_zero/doc/experimental_extensions/MODULE_DP_SUPPORT.md) adds a `DPAS` capability query through `zeDeviceGetModuleProperties`. Intel's open-source compute runtime is MIT-licensed and contains Xe HPG support. This is a credible way to confirm a driver-reported DPAS path, but it is a separate API submission path. No Vulkan-image sharing, cross-API semaphore protocol, runtime installation, or end-to-end Lichii integration has been validated here; copying full-resolution images between APIs would likely defeat the latency and bandwidth goal.

Intel's [`sycl_ext_intel_matrix`](https://github.com/intel/llvm/blob/sycl/sycl/doc/extensions/experimental/sycl_ext_matrix/sycl_ext_intel_matrix.asciidoc) describes Intel AMX/XMX matrix operations. It is explicitly experimental and the documentation cautions against relying on it in shipping products. Intel DPC++ is Apache-2.0 with LLVM exceptions. Its current end-to-end tests include DG2 (`gpu-intel-dg2`) expected failures, which matters because Arc A750 is DG2. SYCL is therefore not selected as the first backend.

The preferred investigation order is:

1. Query the A750's exact Vulkan cooperative-matrix shapes from Lichii's Vulkan device and create a small, isolated matrix workload.
2. Prove instruction selection on the Intel driver using available compiler/disassembly or vendor performance evidence; a successful Vulkan dispatch alone is not XMX proof.
3. Compare the same workload with Level Zero DPAS. Only consider that backend if device matching, resource sharing, semaphore interop, and measured overhead are acceptable.
4. Keep model loading/inference separate from motion processing. The XeSS-FG network is not extracted or redistributed; a future model must have independently verified provenance and licensing.

## Support and Requirements

- **MVP 1 through 3:** uses Vulkan image transfer/compute operations already used by the renderer and existing FSR3 optical-flow shaders. No Intel SDK, runtime DLL, or model is required.
- **Future Vulkan matrix backend:** requires `VK_KHR_cooperative_matrix`, the enabled cooperative-matrix feature, and a queried shape/type combination suitable for its model. The exact A750 combination is still unknown.
- **Future Level Zero backend:** would require an Intel Level Zero runtime/compiler path reporting DPAS and validated sharing of the Lichii Vulkan device's resources and synchronization. This is not a dependency today.
- **Future SYCL backend:** would require an Intel DPC++ installation and its Intel matrix extensions. The relevant Intel extension is experimental and not selected.
- **Target hardware:** Intel Arc A750, 8 GB, Windows. The installed driver observed here is `101.9175`; no minimum Intel driver version is claimed or certified by this MVP.
- **Other GPUs:** MVP color capture is not Intel-specific. No GPU is yet certified for frame generation or an XMX path.

`VK_KHR_external_memory_win32` and timeline semaphores were exposed by the queried A750 driver, but no Level Zero import/export path has been implemented. A future cross-API backend must establish device identity, allocation compatibility, semaphore interop, and measured overhead before it can be considered viable.

### Relevant source and licensing

| Project/source | License | Relevant technique | Windows/Lichii relevance |
| --- | --- | --- | --- |
| Lichii `vk_cooperative_hmma_test.cpp` and Vulkan device capability checks | GPL-3.0-or-later | Queries supported KHR cooperative-matrix properties and validates guest HMMA results | Already in the Windows Vulkan renderer; not proof of XMX selection |
| Intel `compute-runtime` | MIT | Level Zero DPAS capability reporting and Xe HPG runtime support | Potential separate Intel backend; resource and synchronization interop still unproven |
| Intel LLVM/DPC++ matrix extensions | Apache-2.0 WITH LLVM-exception | SYCL `joint_matrix` and Intel XMX APIs | Windows is represented in tests, but DG2 failures and experimental status make it unsuitable as the first production dependency |
| Khronos Vulkan cooperative matrix | Vulkan specification | Vendor-neutral matrix operations and shape discovery | Available on this A750 driver; physical execution path still must be proved |

No external source code or model was copied into the MVP. The existing user-provided XeSS runtime remains a separate XeSS-SR dependency.

## Synchronization and Pacing

MVP 1 uses the renderer's graphics queue and scheduler. It appends image barriers and a copy after frame composition; the regular frame submission signals `Frame::render_ready`, which the present path waits on. The XeFG 2x path uses `Fsr3FrameGen` on the same scheduler, then Lichii's bounded frame pool and present thread pace the generated frame. No emulated CPU/GPU timing, input polling, game logic, or audio timing is changed.

Lichii already has an asynchronous present thread and bounded presentation-frame pool, and its optional FSR3 frame-generation path already spaces generated frames. Those mechanisms are references for future pacing, not evidence that XeFG scheduling is implemented. A future XeFG path must avoid unbounded queued frames and must not wait on the CPU for inference in the normal frame loop.

## Build and Runtime

MVP 1 adds no SDK or third-party dependency. Build using the repository's existing Windows Vulkan/CMake environment, for example:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The `XeFG (Optical Flow, 2x)` frame-generation type is selected in Lichii's existing frame-generation settings; enable frame generation and choose this type. `EDEN_XEFG_CAPTURE=1` independently enables the MVP 1 diagnostic capture and is off by default. The existing flow-dump option writes a motion visualization for XeFG. No XMX/Level Zero runtime DLL dependency has been added.

## Validation and Measurements

- Repository: feature work is on `feature/xmx-xefg-vk`, based at the XeSS branch commit shown above. The XeSS branch itself is unchanged.
- Source diagnostics: VS Code reported no errors in the new capture source or the touched renderer files.
- Patch hygiene: `git diff --check` passed.
- Build: not run. No configured build tree, CMake, or Ninja was available in this environment.
- Vulkan runtime: `vulkaninfo` identified the Intel Arc A750 and reported the cooperative-matrix extension/feature, Win32 external memory, and timeline semaphore. It did not expose cooperative-matrix shape details. XeFG now logs the shape list when selected, but that code has not been run.
- Capture and interpolation execution: not run. The installed Lichii binary predates these source edits; queued GPU copies and generated frames have not been verified in a rendered session.
- XMX execution, image quality, frame pacing, latency, VRAM, CPU overhead, and GPU timing: not measured.

## Test Plan

Before enabling generated frames, validate the capture and capability paths separately:

- **Hardware:** Intel Arc A750 on Windows; a non-Intel Vulkan GPU; and an Intel GPU/driver without a usable matrix path to verify graceful feature disablement.
- **Vulkan:** enumerate cooperative-matrix extension and feature support, list every reported matrix shape/type/scope, check the exact compute queue and image-format usages, query timeline semaphores, and check external-memory/semaphore capabilities only for an interop experiment.
- **Capture:** static scene, moving camera, moving objects, resize, format/HDR changes, dropped/hidden frames, and image reuse under Vulkan validation.
- **Temporal transitions:** particles/transparency, HUD, menus, cutscenes, loading transitions, camera cuts/teleports, and variable frame intervals. Verify history reset and that the game simulation/input/audio timing remains unchanged.
- **Performance:** record native and displayed FPS, frame time, motion time, inference time, GPU time, queue/present latency, VRAM, CPU overhead, and dropped/late generated frames. Compare the same scene and driver with capture/frame generation off and on.

Known image artifacts cannot yet be assessed because there is no generated frame. Expected risks for a future interpolation algorithm include disocclusion holes, transparency/particle ghosting, camera-cut blends, and UI trails.

## Remaining Work

1. Build the branch and run capture plus XeFG with Vulkan validation; verify image reuse, resize, reset, and present ordering.
2. Move XeFG input before UI composition and add a resource-provider contract for color, depth, motion, and reset metadata.
3. Run XeFG on the A750 to capture exact cooperative-matrix shapes, then implement a standalone matrix inference dispatch and prove XMX/DPAS execution from compiler or vendor evidence.
4. Evaluate quality across camera/object motion, particles, UI, menus, cutscenes, and loading transitions; add history-reset rules.
5. Add a replaceable, independently licensed neural model and measure latency, GPU time, frame pacing, memory, and image quality on the A750.