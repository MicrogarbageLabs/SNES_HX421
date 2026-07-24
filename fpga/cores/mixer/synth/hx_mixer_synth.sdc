# The mixer runs on the base core's 96 MHz memory clock (CLKIN 8 MHz x12).
# 10.417 ns period. If it holds this, it holds the PSRAM read cadence.
create_clock -name {clk} -period 10.417 [get_ports {clk}]
derive_clock_uncertainty
