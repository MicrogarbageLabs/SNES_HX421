# Integrated RPG core internal clock target: 96 MHz (base CLKIN 8 MHz x12 PLL).
create_clock -name {clk} -period 10.417 [get_ports {clk}]
derive_clock_uncertainty
