<p align="center">
  <img src="dist/branding/lichii_icon_with_logo.png" alt="Lichii logo" width="560">
</p>

<h1 align="center">Lichii — LionheartTim Edition</h1>

<p align="center">
  A performance-focused, experimental Linux fork of Lichii, a Nintendo Switch and Switch 2 emulator.
</p>

> [!IMPORTANT]
> This is an **independent, unofficial fork**. It is not affiliated with Nintendo or the upstream Lichii, Eden, Eglantine, yuzu, or Sudachi teams. Game compatibility and performance vary by title, hardware, graphics driver, and settings.

## About

**Lichii — LionheartTim Edition** builds on Lichii and the projects that preceded it, with an emphasis on smoother gameplay, lower emulation overhead, and practical performance improvements on Linux.

The project focuses on improving the native rendering and emulation pipeline while preserving correctness. Experimental changes are tested against real gameplay and performance traces; an increase in displayed frame rate is not the same as an increase in native emulation speed.

## Areas of focus

- **CPU/GPU coordination:** reduce unnecessary synchronization and waiting in the GPU command pipeline.
- **Vulkan renderer:** improve compute-pipeline setup, buffer-view reuse, resource lookups, and state-handling overhead.
- **Shaders and pipelines:** support asynchronous compilation, local preloading/streaming, and more efficient reuse of known pipelines.
- **CPU-side emulation:** investigate scheduling, memory access, and JIT-related bottlenecks without sacrificing correctness.
- **AMD graphics:** preserve FidelityFX upscaling options and experimental AMD frame-generation integration where supported by the build.
- **Performance diagnostics:** add profiling points to help identify bottlenecks rather than relying on FPS numbers alone.

These are ongoing development areas, **not guarantees that every game or GPU benefits from every change**. Some features are experimental and may not be available in every build or configuration.

## Downloads

See this repository's **Releases** section for published builds and release notes.

- **Linux:** native builds are supported by the project; AppImage packaging is being prepared and tested.
- **Other operating systems:** build availability is not guaranteed by this fork.

No commercial games, firmware, console keys, or other proprietary system files are distributed with this project. Use only content you are legally entitled to use.

## Building on Linux

The following is an example for an existing Linux development environment with **CMake, Ninja, a C++20-capable compiler, Qt 6 development packages, Vulkan development headers**, and the dependencies required by the upstream project. See [the upstream build documentation](docs/Build.md) for additional setup details.

```bash
git clone <YOUR-REPOSITORY-URL> Lichii-LionheartTim
cd Lichii-LionheartTim

cmake -S . -B build/local-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS="-fPIC" \
  -DENABLE_QT=ON \
  -DYUZU_USE_BUNDLED_QT=OFF \
  -Dfmt_FORCE_BUNDLED=ON \
  -Dlz4_FORCE_BUNDLED=ON \
  -DSPIRV-Headers_FORCE_BUNDLED=ON

cmake --build build/local-release --target yuzu --parallel 4
./build/local-release/bin/lichii
```

Replace `<YOUR-REPOSITORY-URL>` with the URL of this GitHub repository. Dependencies and CMake options may need to be adjusted for your distribution. On Bazzite/Fedora, using one consistent set of **system Qt headers and libraries** is particularly important; mixing Linuxbrew Qt headers with system Qt libraries can cause compilation or linker failures.

## Performance testing

For meaningful comparisons, use the same game location, resolution, driver, shader-cache state, and graphics settings:

1. Disable **Frame Generation** when measuring **native FPS**.
2. Keep upscaling settings consistent between runs.
3. Compare frame times and stutters, not only average FPS.
4. Repeat demanding scenes several times; the first run may include shader or asset-loading work.

Optimizations are workload-dependent and may help one scene more than another. No fixed performance increase is promised.

## Stability and data

This fork is experimental. Back up your saves, settings, and shader caches before testing new builds. Existing data-directory names may still reflect the project's upstream heritage. Do not delete or migrate them without a separate backup.

## Reporting issues

Please use the repository's **Issues** section. Include the game and game version, a brief reproduction procedure, CPU and GPU models, graphics-driver version, relevant settings, and a log excerpt with private information removed. Do not upload proprietary game files, keys, or firmware.

## Credits

This fork stands on the work of the **Lichii, Eglantine, Eden, yuzu, and Sudachi** projects and their many contributors. Original copyright notices and third-party acknowledgements remain applicable. The existing Lichii artwork is credited to **YOB (yobslab)** in the upstream project.

## License

See [LICENSE.txt](LICENSE.txt) and individual source files for the applicable licensing and attribution requirements. Contributions and redistributed builds must comply with those licenses.
