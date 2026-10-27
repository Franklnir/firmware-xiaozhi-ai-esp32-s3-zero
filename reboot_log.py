import serial
import time

try:
    s = serial.Serial('COM18', 115200, timeout=0.2)
    s.dtr = False
    s.rts = True
    time.sleep(0.1)
    s.rts = False
    time.sleep(0.1)
    t0 = time.time()
    while time.time() - t0 < 3.5:
        line = s.readline()
        if line:
            print(line.decode('utf-8', errors='ignore').strip())
    s.close()
except Exception as e:
    print("Error:", e)
