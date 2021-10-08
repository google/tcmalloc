# Empirical Driver

TCMalloc's empirical driver is a benchmark that drives realistic multithreaded
loads in order to test the behavior and performance of TCMalloc. The empirical
driver runs a simulation of malloc traffic across many threads that matches the
observed allocation rates and live counts of real workloads.

## Running the Driver

First, build the `tcmalloc/testing:empirical_driver` target:

```
tcmalloc$ bazel build -c opt --dynamic_mode=off //tcmalloc/testing:empirical_driver
Extracting Bazel installation...
Starting local Bazel server and connecting to it...
INFO: Analyzed target //tcmalloc/testing:empirical_driver (39 packages loaded, 703 targets configured).
INFO: Found 1 target...
Target //tcmalloc/testing:empirical_driver up-to-date:
  bazel-bin/tcmalloc/testing/empirical_driver
INFO: Elapsed time: 28.351s, Critical Path: 7.20s
INFO: 174 processes: 5 internal, 169 linux-sandbox.
INFO: Build completed successfully, 174 total actions
```

Now, run the compiled program:

```
tcmalloc$ bazel run //tcmalloc/testing:empirical_driver -- --profile=delta --test_iterations=1000
```

## Command-line Flags

*   `--bytes`: Total size of base heap. Default: 17179869184.
*   `--empirical_malloc_release_bytes_per_sec`: Number of bytes to try to
    release from the page heap per second. Default: 0.
*   `--print_stats_to_file`: Write mallocz stats to a file. Default: true.
*   `--profile`: Which source of profiled allocation to use for the base load.
    This can be beta, bravo, charlie, delta, echo, foxtrot, merced, sierra,
    sigma, or uniform. It can also be a uint64_t N, which means allocate N-byte
    objects exclusively. Default: "".
*   `--record_and_replay`: Precalculate a trace of allocs / deallocs and use
    this trace to drive allocation / deallocation. Removes (expensive) testbench
    overhead from actual performance measurement. Default: false.
*   `--record_and_replay_buffer_size`: The total number of allocs / deallocs to
    precalculate for later replay. Memory required to store the replay buffers
    scales with the number of threads. Default: 100000000.
*   `--simulated_bytes_per_sec`: If non-0, empirical driver will simulate tick
    of ReleaseMemoryToOS iteration each given number of bytes allocated.
    Default: 0.
*   `--spike_bytes`: Additional memory allocated periodically. Could model
    per-query memory or diurnal variation, etc. Default: 0.
*   `--spike_lifetime`: Processing time for spikes. Default: 30ms.
*   `--spike_locality`: Probability a spike freed by the thread which created
    it. Default: 0.9;
*   `--spike_profile`: If `--spike_profile` has one of the valid values for
    `--profile` it has the same meaning. Otherwise must be the empty string,
    which means copy `--profile`. Default: "".
*   `--spike_rate`: 1/QPS for spikes. Default: 10ms;
*   `--spikes_exact`: Should spike rates/lifetimes be exactly the average
    (instead of randomized)? Default: false.
*   `--spikes_shared`: Should spikes be split among threads equally (instead of
    on one chosen thread)? Default: false.
*   `--test_iterations`: Exit the benchmark after specified number of
    iterations. If unspecified benchmark runs forever. Default: 0.
*   `--threads`: Number of parallel allocators. Default: 1.
*   `--transient_bytes`: Additional size of data allocated at program start,
    then freed before running main benchmark. Default: 0.
*   `--transient_profile`: If `--transient_profile` has one of the valid values
    for `--profile` it has the same meaning. Otherwise must be the empty string,
    which means copy `--profile`. Default: "".
