# Calibration (Phase 2.3)

Measures the numbers `p05tool`'s cost model prices against and writes them as a
machine file for `p05tool -machine=<file>`:

    # host only (e.g. the M4):
    python3 eval/calibrate/calibrate.py --name m4 --out machine/m4.json \
      --cc /opt/homebrew/opt/llvm/bin/clang \
      --cflags "-O2 -fopenmp -isysroot $(xcrun --show-sdk-path) -L/opt/homebrew/opt/llvm/lib -Wl,-rpath,/opt/homebrew/opt/llvm/lib"
    # host + NVIDIA GPU: eval/colab/calibrate.ipynb (Run all, copy the JSON back)

`calibrate.c` holds the benchmarks, `calibrate.py` runs them and derives the
fields, `make_notebook.py` regenerates the Colab notebook from both (the repo
is private, so the notebook carries them).

## Machine file

    {
      "name": "m4",                       // shown in the report
      "cpu": {
        "throughput_gflops": 31.7,         // one thread, ops as p05tool counts them, per ns
        "cores": 4,                        // all threads together / one thread, rounded
        "dram_bandwidth_gbs": 96.2,        // triad, all threads
        "single_core_dram_share": 0.905,   // triad, one thread / all threads
        "thread_start_us": 23.7,           // an empty `parallel for`, runtime up
        "runtime_init_us": 48.3            // first `parallel` in a process, minus the above
      },
      "gpu": {                             // leave out: no GPU, offload never chosen
        "kernel_launch_us": ...,           // an empty kernel, device up
        "pcie_bandwidth_gbs": ...,         // map(to)/map(from) of 1 MiB..256 MiB, fitted
        "throughput_gflops": ...,          // the ops kernel on the device
        "bandwidth_gbs": ...,              // triad on the device, data resident
        "init_us": ...                     // first `target` in a process, minus a launch
      },
      "measured": ..., "host": ..., "device": ..., "compiler": ..., "notes": ..., "raw": {...}
    }

Every field of a section is required, and unknown keys are errors.
`measured`, `host`, `device`, `compiler`, `notes` and `raw` are for
people; the tool doesn't read them. A machine flag given on the command line
(`-thread-start-us=...`) overrides the file.

## How each number is taken

- **Bytes and ops are counted the way the cost model counts them.** The
  triad touches 24 B per element, with no write-allocate traffic. The ops
  kernel is 64 multiply-adds written out, 128 ops with no loop counter
  among them (`p05tool eval/calibrate/calibrate.c -- -fopenmp` reports 128
  ops/iteration).
- **Throughputs** are the best of several runs in one process.
- **Startup costs** are the median of 7 fresh processes, minus the cost of
  the same construct once the runtime is up.
- **`cores`** is how many single threads all the threads together are
  worth. On the M4, `schedule(static)` gives the efficiency cores equal
  chunks, so 10 threads are worth about 4.
- **Transfers** are an empty `target data` mapping N bytes: allocate, copy one
  way, free, which is what a per-loop `map()` pays. They're fitted as
  latency + bytes/bandwidth over the sizes of at least 1 MiB. The model has
  one bandwidth for both directions, the per-byte average of the two; the
  fitted latencies are kept in `raw` (the model has no per-map latency term).
- `raw.first_parallel_if0_us` is a first `parallel if(c)` with c false at
  run time. It says whether a false guard still pays the runtime's start:
  about 1 µs on the M4, about 70 µs of 400 µs on Linux libomp in Docker.
