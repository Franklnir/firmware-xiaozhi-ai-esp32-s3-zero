import serial
import time

try:
    s = serial.Serial('COM18', 115200, timeout=1)
    s.dtr = True
    s.rts = True
    time.sleep(0.2)
    t0 = time.time()
    while time.time() - t0 < 4:
        l = s.readline()
        if l:
            print(l.decode('utf-8', errors='ignore').strip())
    s.close()
except Exception as e:
    print("Error:", e)
