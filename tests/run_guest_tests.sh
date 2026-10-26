#!/bin/bash
# Run from the repository root. Uses no proprietary or generated game source.
set -euo pipefail
DEST="${1:?usage: tests/run_guest_tests.sh output-directory}"
mkdir -p "$DEST"
CC="${CC:-clang}"
FLAGS=(-O2 -std=gnu11 -Wall -Wextra -Werror -Iruntime -fno-strict-aliasing -pthread)
LIBS=(-lm)
"$CC" "${FLAGS[@]}" tests/win32_runtime_clock_test.c -o "$DEST/win32-runtime-clock-test"
"$DEST/win32-runtime-clock-test"
"$CC" "${FLAGS[@]}" tests/audio_core_test.c -o "$DEST/audio-core-test" "${LIBS[@]}"
"$DEST/audio-core-test"
"$CC" "${FLAGS[@]}" tests/metal_pipeline_cache_test.c -o "$DEST/metal-pipeline-cache-test"
"$DEST/metal-pipeline-cache-test"
# Exercise the production file import with synthetic guest memory. The legacy
# implementation retains its production warning flags; the fixture is strict.
"$CC" -O2 -std=gnu11 -Iruntime -fno-strict-aliasing -pthread \
  -c runtime/fileio.c -o "$DEST/full-fileio.o"
"$CC" "${FLAGS[@]}" "$DEST/full-fileio.o" tests/win32_full_path_test.c \
  -o "$DEST/win32-full-path-test"
"$DEST/win32-full-path-test"
"$CC" "${FLAGS[@]}" "$DEST/full-fileio.o" tests/fileio_case_cache_test.c \
  -o "$DEST/fileio-case-cache-test"
"$DEST/fileio-case-cache-test"
"$CC" "${FLAGS[@]}" tests/win32_exit_scope_test.c -o "$DEST/win32-exit-scope-test"
"$DEST/win32-exit-scope-test"
"$CC" "${FLAGS[@]}" tests/win32_guest_thread_resources_test.c -o "$DEST/win32-guest-thread-resources-test"
"$DEST/win32-guest-thread-resources-test"
"$CC" "${FLAGS[@]}" tests/bc_decoder_test.c -o "$DEST/bc-decoder-test"
"$DEST/bc-decoder-test"
"$CC" "${FLAGS[@]}" runtime/guest_dispatch.c tests/guest_dispatch_test.c -o "$DEST/guest-dispatch-test"
"$DEST/guest-dispatch-test"
"$CC" "${FLAGS[@]}" runtime/guest_dispatch.c runtime/guest_execution.c runtime/guest_bootstrap.c   runtime/guest_memory.c tests/guest_execution_test.c -o "$DEST/guest-execution-test"
"$DEST/guest-execution-test"
"$CC" "${FLAGS[@]}" runtime/guest_dispatch.c runtime/guest_execution.c runtime/guest_memory.c tests/guest_execution_tail_test.c -o "$DEST/guest-execution-tail-test"
"$DEST/guest-execution-tail-test"
"$CC" "${FLAGS[@]}" runtime/guest_memory.c runtime/guest_registry.c tests/guest_registry_test.c -o "$DEST/guest-registry-test"
"$DEST/guest-registry-test"
"$CC" "${FLAGS[@]}" runtime/guest_cpuid.c tests/guest_cpuid_test.c -o "$DEST/guest-cpuid-test"
"$DEST/guest-cpuid-test"
"$CC" "${FLAGS[@]}" runtime/guest_memory.c runtime/guest_resources.c tests/guest_resources_test.c -o "$DEST/guest-resources-test"
"$DEST/guest-resources-test"
"$CC" "${FLAGS[@]}" runtime/guest_memory.c runtime/guest_user_objects.c tests/guest_user_objects_test.c -o "$DEST/guest-user-objects-test"
"$DEST/guest-user-objects-test"
"$CC" "${FLAGS[@]}" runtime/guest_toolhelp.c tests/test_guest_toolhelp.c -o "$DEST/guest-toolhelp-test"
"$DEST/guest-toolhelp-test"
"$CC" "${FLAGS[@]}" runtime/guest_memory.c runtime/guest_heap.c tests/guest_heap_test.c -o "$DEST/guest-heap-test"
"$DEST/guest-heap-test"
"$CC" "${FLAGS[@]}" runtime/guest_memory.c runtime/guest_critical.c tests/guest_critical_test.c -o "$DEST/guest-critical-test"
"$CC" "${FLAGS[@]}" runtime/guest_memory.c runtime/guest_tls.c tests/guest_tls_test.c -o "$DEST/guest-tls-test"
"$DEST/guest-tls-test"
"$DEST/guest-critical-test"
"$CC" "${FLAGS[@]}" runtime/guest_memory.c runtime/guest_startup.c tests/guest_startup_test.c -o "$DEST/guest-startup-test"
"$DEST/guest-startup-test"
"$CC" "${FLAGS[@]}" runtime/guest_memory.c runtime/guest_error.c tests/guest_error_test.c -o "$DEST/guest-error-test"
"$DEST/guest-error-test"
"$CC" "${FLAGS[@]}" runtime/guest_memory.c runtime/guest_stdio.c tests/guest_stdio_test.c -o "$DEST/guest-stdio-test"
"$DEST/guest-stdio-test"
"$CC" "${FLAGS[@]}" runtime/guest_memory.c runtime/guest_process.c tests/guest_process_test.c -o "$DEST/guest-process-test"
"$DEST/guest-process-test"
"$CC" "${FLAGS[@]}" runtime/guest_memory.c runtime/guest_locale.c tests/guest_locale_test.c -o "$DEST/guest-locale-test"
"$DEST/guest-locale-test"
"$CC" "${FLAGS[@]}" runtime/guest_dispatch.c runtime/guest_execution.c runtime/guest_windows.c runtime/guest_clock.c runtime/guest_resources.c runtime/guest_user_objects.c runtime/guest_toolhelp.c runtime/guest_cursor.c runtime/guest_registry.c runtime/guest_locale.c runtime/guest_heap.c runtime/guest_critical.c runtime/guest_tls.c runtime/guest_process.c runtime/guest_startup.c runtime/guest_error.c runtime/guest_stdio.c runtime/guest_memory.c tests/guest_windows_test.c -o "$DEST/guest-windows-test" "${LIBS[@]}"
"$DEST/guest-windows-test"
for API_TEST in processor_feature window_query cursor metrics; do
  "$CC" "${FLAGS[@]}" runtime/guest_dispatch.c runtime/guest_execution.c runtime/guest_windows.c runtime/guest_clock.c runtime/guest_resources.c runtime/guest_user_objects.c runtime/guest_toolhelp.c runtime/guest_cursor.c runtime/guest_registry.c runtime/guest_locale.c runtime/guest_heap.c runtime/guest_critical.c runtime/guest_tls.c runtime/guest_process.c runtime/guest_startup.c runtime/guest_error.c runtime/guest_stdio.c runtime/guest_memory.c "tests/guest_${API_TEST}_test.c" -o "$DEST/guest-${API_TEST}-test" "${LIBS[@]}"
  "$DEST/guest-${API_TEST}-test"
done
"$CC" "${FLAGS[@]}" runtime/guest_dispatch.c runtime/guest_execution.c runtime/guest_windows.c runtime/guest_clock.c runtime/guest_resources.c runtime/guest_user_objects.c runtime/guest_toolhelp.c runtime/guest_cursor.c runtime/guest_registry.c runtime/guest_locale.c runtime/guest_heap.c runtime/guest_critical.c runtime/guest_tls.c runtime/guest_process.c runtime/guest_startup.c runtime/guest_error.c runtime/guest_stdio.c runtime/guest_memory.c tests/guest_wchar_test.c -o "$DEST/guest-wchar-test" "${LIBS[@]}"
"$DEST/guest-wchar-test"
"$CC" "${FLAGS[@]}" runtime/guest_dispatch.c runtime/guest_execution.c runtime/guest_windows.c runtime/guest_clock.c runtime/guest_resources.c runtime/guest_user_objects.c runtime/guest_toolhelp.c runtime/guest_cursor.c runtime/guest_registry.c runtime/guest_locale.c runtime/guest_heap.c runtime/guest_critical.c runtime/guest_tls.c runtime/guest_process.c runtime/guest_startup.c runtime/guest_error.c runtime/guest_stdio.c runtime/guest_memory.c tests/guest_exception_test.c -o "$DEST/guest-exception-test" "${LIBS[@]}"
"$DEST/guest-exception-test"
"$CC" "${FLAGS[@]}" runtime/guest_dispatch.c runtime/guest_execution.c runtime/guest_windows.c runtime/guest_clock.c runtime/guest_resources.c runtime/guest_user_objects.c runtime/guest_toolhelp.c runtime/guest_cursor.c runtime/guest_registry.c runtime/guest_locale.c runtime/guest_heap.c runtime/guest_critical.c runtime/guest_tls.c runtime/guest_process.c runtime/guest_startup.c runtime/guest_error.c runtime/guest_stdio.c runtime/guest_memory.c tests/guest_time_test.c -o "$DEST/guest-time-test" "${LIBS[@]}"
"$DEST/guest-time-test"
"$CC" "${FLAGS[@]}" runtime/guest_dispatch.c runtime/guest_execution.c runtime/guest_windows.c runtime/guest_clock.c runtime/guest_resources.c runtime/guest_user_objects.c runtime/guest_toolhelp.c runtime/guest_cursor.c runtime/guest_registry.c runtime/guest_locale.c runtime/guest_heap.c runtime/guest_critical.c runtime/guest_tls.c runtime/guest_process.c runtime/guest_startup.c runtime/guest_error.c runtime/guest_stdio.c runtime/guest_memory.c tests/guest_heap_realloc_test.c -o "$DEST/guest-heap-realloc-test" "${LIBS[@]}"
"$DEST/guest-heap-realloc-test"
"$CC" "${FLAGS[@]}" runtime/guest_memory.c runtime/guest_file.c tests/guest_file_test.c -o "$DEST/guest-file-test"
"$DEST/guest-file-test"
"$CC" "${FLAGS[@]}" runtime/guest_cursor.c ios/NFSU2iOS/Native/NFSU2InputBridge.c tests/input_bridge_test.c -o "$DEST/input-bridge-test" "${LIBS[@]}"
"$DEST/input-bridge-test"
"$CC" "${FLAGS[@]}" runtime/guest_cursor.c ios/NFSU2iOS/Native/NFSU2InputBridge.c tests/game_rumble_test.c -o "$DEST/game-rumble-test" "${LIBS[@]}"
"$DEST/game-rumble-test"
"$CC" "${FLAGS[@]}" -c runtime/dispatch.c -o "$DEST/desktop-dispatch.o"
"$CC" "${FLAGS[@]}" runtime/guest_memory.c tests/guest_memory_test.c -o "$DEST/guest-memory-test"
"$CC" "${FLAGS[@]}" runtime/guest_memory.c runtime/guest_pe.c tests/guest_pe_test.c -o "$DEST/guest-pe-test"
"$DEST/guest-pe-test"
"$DEST/guest-memory-test"
"$CC" "${FLAGS[@]}" runtime/guest_memory.c runtime/guest_dispatch.c ios/tests/cpu_flags_test.c \
  ios/NFSU2iOS/Native/GuestSmoke.c ios/tests/smoke_cli.c -o "$DEST/nfsu2-smoke"
"$DEST/nfsu2-smoke"
if [[ "$(uname -s)" == Darwin ]]; then
  "$CC" "${FLAGS[@]}" tests/metal_framebuffer_pool_test.c -o "$DEST/metal-framebuffer-pool-test"
  "$DEST/metal-framebuffer-pool-test"
  python3 tests/ios_file_storage_test.py "$DEST"
  python3 tools/ios_private/typecheck_full_ui.py
  # Compile the production thread/timer call paths, including iOS-only branches.
  # This uses authored source only; no generated game functions or input EXE.
  "$CC" -O2 -std=gnu11 -Iruntime -fno-strict-aliasing -DNFS_FULL_RUNTIME=1 \
    -c runtime/win32_ext.c -o "$DEST/win32-ext-macos.o"
  "$CC" -O2 -std=gnu11 -Iruntime -fno-strict-aliasing -DNFS_FULL_RUNTIME=1 \
    -c runtime/win32.c -o "$DEST/win32-macos.o"
  xcrun --sdk iphoneos clang -O2 -std=gnu11 -Iruntime -fno-strict-aliasing \
    -arch arm64 -miphoneos-version-min=16.0 -DNFS_FULL_RUNTIME=1 \
    -c runtime/win32_ext.c -o "$DEST/win32-ext-ios-arm64.o"
  xcrun --sdk iphoneos clang -O2 -std=gnu11 -Iruntime -fno-strict-aliasing \
    -arch arm64 -miphoneos-version-min=16.0 -DNFS_FULL_RUNTIME=1 \
    -c runtime/win32.c -o "$DEST/win32-ios-arm64.o"
  xcrun --sdk iphoneos clang -O2 -std=gnu11 -Iruntime -fno-strict-aliasing \
    -arch arm64 -miphoneos-version-min=16.0 -DNFS_FULL_RUNTIME=1 \
    -c runtime/dispatch.c -o "$DEST/dispatch-ios-arm64.o"
  echo 'PASS | production Win32 thread/timer source compiles for macOS and iOS ARM64'
  "$CC" -O2 -std=gnu11 -fobjc-arc -Iruntime -fno-strict-aliasing \
    -DNFS_FULL_RUNTIME=1 -DNFS_ENHANCED=0 -DNFSU2_LEGACY_METAL_INPUT=1 \
    -c runtime/metal.m -o "$DEST/metal-macos.o"
  xcrun --sdk iphoneos clang -O2 -std=gnu11 -fobjc-arc -Iruntime \
    -fno-strict-aliasing -arch arm64 -miphoneos-version-min=16.0 \
    -DNFS_FULL_RUNTIME=1 -DNFS_ENHANCED=0 -DNFSU2_LEGACY_METAL_INPUT=1 \
    -c runtime/metal.m -o "$DEST/metal-ios-arm64.o"
  echo 'PASS | production Metal source compiles for macOS and iOS ARM64'
  "$CC" -O2 -std=gnu11 -fobjc-arc -Iruntime -fno-strict-aliasing \
    -DNFS_FULL_RUNTIME=1 -c runtime/audio.m -o "$DEST/audio-macos.o"
  xcrun --sdk iphoneos clang -O2 -std=gnu11 -fobjc-arc -Iruntime \
    -fno-strict-aliasing -arch arm64 -miphoneos-version-min=16.0 \
    -DNFS_FULL_RUNTIME=1 -c runtime/audio.m -o "$DEST/audio-ios-arm64.o"
  "$CC" -O2 -std=gnu11 -Iruntime -fno-strict-aliasing \
    -DNFS_FULL_RUNTIME=1 -DNFS_ENHANCED=0 -c runtime/com.c -o "$DEST/com-macos.o"
  xcrun --sdk iphoneos clang -O2 -std=gnu11 -Iruntime \
    -fno-strict-aliasing -arch arm64 -miphoneos-version-min=16.0 \
    -DNFS_FULL_RUNTIME=1 -DNFS_ENHANCED=0 -c runtime/com.c -o "$DEST/com-ios-arm64.o"
  echo 'PASS | production DirectSound COM and audio sources compile for macOS and iOS ARM64'
  "$CC" "${FLAGS[@]}" -DNFS_FULL_RUNTIME=1 runtime/mem.c runtime/guest_memory.c runtime/guest_pe.c \
    tests/full_pe_adapter_test.c -o "$DEST/full-pe-adapter-test"
  "$DEST/full-pe-adapter-test"
  "$CC" "${FLAGS[@]}" runtime/mem.c runtime/guest_memory.c \
    tests/guest_memory_desktop_test.c -o "$DEST/guest-memory-desktop-test"
  "$DEST/guest-memory-desktop-test"
fi
CC="$CC" python3 tests/ios_startup_probe_strict_test.py "$DEST"
python3 tests/guest_locale_windows_test.py

"$CC" "${FLAGS[@]}" runtime/guest_memory.c runtime/guest_clock.c tests/guest_clock_test.c -o "$DEST/guest-clock-test" "${LIBS[@]}"
"$DEST/guest-clock-test"
