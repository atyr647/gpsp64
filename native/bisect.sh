#!/bin/bash
# Fast native bisect driver: build+run each AOT variant, report pass/fail.
set -u
cd /home/user/gpsp64
ROM="Pokemon - Emerald Version (USA, Europe).gba"
BIOS="bios/open_gba_bios.bin"
FRAMES=3000
TIMEOUT=25

build_and_run() {
  local label="$1"; shift
  local defs="$*"
  rm -f cpu_bisect.o native_bisect_main.o gen_bisect.o main_bisect.o
  g++ -DN64 -DPROFILE_AOT $defs -I. -Inative -O2 -w -fno-rtti -fno-exceptions -ffunction-sections -fdata-sections \
    -c cpu.cc -o cpu_bisect.o 2>/tmp/bisect_err_$label.log
  if [ $? -ne 0 ]; then echo "$label: CPU COMPILE FAIL"; return; fi
  gcc -DN64 -DPROFILE_AOT $defs -I. -Inative -O2 -w -ffunction-sections -fdata-sections \
    -c n64/aot_generated.c -o gen_bisect.o 2>>/tmp/bisect_err_$label.log
  if [ $? -ne 0 ]; then echo "$label: AOT_GEN COMPILE FAIL"; return; fi
  gcc -DN64 -DPROFILE_AOT $defs -I. -Inative -O2 -w -ffunction-sections -fdata-sections \
    -c native/native_main.c -o main_bisect.o 2>>/tmp/bisect_err_$label.log
  if [ $? -ne 0 ]; then echo "$label: MAIN COMPILE FAIL"; return; fi
  g++ -Wl,--gc-sections -o native_bisect main.o gba_memory.o sound.o cheats.o serial.o gbp.o rfu.o serial_proto.o gba_cc_lut.o memmap.o cpu_bisect.o video.o bios_data.o aot_hle.o gen_bisect.o main_bisect.o -lm 2>>/tmp/bisect_err_$label.log
  if [ $? -ne 0 ]; then echo "$label: LINK FAIL"; return; fi
  local t0=$(date +%s%N)
  timeout $TIMEOUT ./native_bisect "$ROM" $FRAMES "$BIOS" > /tmp/bisect_out_$label.log 2>&1
  local rc=$?
  local t1=$(date +%s%N)
  local ms=$(( (t1 - t0) / 1000000 ))
  if [ $rc -eq 0 ]; then
    echo "$label: PASS (${ms}ms)"
  elif [ $rc -eq 124 ]; then
    echo "$label: HANG (timed out at ${TIMEOUT}s)  last: $(tail -3 /tmp/bisect_out_$label.log | tr '\n' ' ')"
  else
    echo "$label: CRASH rc=$rc  last: $(tail -3 /tmp/bisect_out_$label.log | tr '\n' ' ')"
  fi
}

build_and_run "$@"
