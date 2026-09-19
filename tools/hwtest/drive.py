#!/usr/bin/env python3
"""Send a command char to the test rig and capture its output until DONE."""
import serial, sys, time
cmd = sys.argv[1]
wait = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
s = serial.Serial('/dev/cu.usbserial-110', 115200, timeout=1)
time.sleep(0.3)
s.reset_input_buffer()
s.write(cmd.encode())
s.flush()
t0 = time.time()
while time.time() - t0 < wait:
    line = s.readline().decode('utf-8', 'replace').rstrip()
    if line:
        print(line, flush=True)
        if line.strip() == 'DONE':
            break
s.close()
