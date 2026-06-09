## Extracted CCEH layout

This subtree isolates:

- `include/CCEH`: the CCEH implementation and its direct local header dependencies
- `include/CCEH/CCEH_baseline_dram.h`: extracted baseline DRAM CCEH variant
- `include/CCEH/allocator.h`: restored historical allocator used by baseline
- `include/CCEH/tests`: test-only workload generator helpers copied from `util/`
- `tests`: the two benchmark drivers renamed to `cceh_test_pmem*.cpp`

For testing inside this repo, the extracted `CMakeLists.txt` includes
`third_party/epoch_reclaimer` directly.
