# Clock constraint for standalone core benchmarking.
# Start at 40 MHz (25 ns). If it closes with slack, retry at 12.5 ns (80 MHz)
# to find the ceiling on this C8 part.
create_clock -name {clk} -period 25.000 -waveform { 0.000 12.500 } [get_ports {clk}]
derive_clock_uncertainty

# I/O timing is irrelevant here - the wrapper registers every port so the
# fitter reports core paths, not pad delays. Cut the I/O paths so they cannot
# mask the real critical path.
set_false_path -from [all_inputs] -to [all_registers]
set_false_path -from [all_registers] -to [all_outputs]
