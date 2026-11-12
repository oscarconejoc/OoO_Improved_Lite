# =====================================================================
# Makefile — RISC-V OoO Lite
#
# make check-soc  -> compila y ejecuta el testbench de Lite
# make check      -> alias de check-soc
# make clean
# =====================================================================

CXX     := g++
SOCHLS  := RISCV-SoC
AP_INC  := -I third_party/ap_types/include
HLS_STD := -std=c++14 -O2

.PHONY: all check check-soc clean

all: check

check: check-soc

$(SOCHLS)/soc_tb_lite: $(SOCHLS)/soc_tb_lite.cpp $(SOCHLS)/soc_top.cpp
	$(CXX) $(HLS_STD) -I $(SOCHLS) $(AP_INC) -o $@ $^

check-soc: $(SOCHLS)/soc_tb_lite
	@echo "=== [LITE] verificacion funcional ==="
	@$(SOCHLS)/soc_tb_lite

clean:
	rm -f $(SOCHLS)/soc_tb_lite