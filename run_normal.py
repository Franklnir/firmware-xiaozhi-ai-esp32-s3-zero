import serial
import time

s = serial.Serial('COM18', 115200, timeout=0.2)
s.dtr = False
s.rts = True
time.sleep(0.05)
s.rts = False
time.sleep(0.2)

t0 = time.time()
while time.time() - t0 < 5:
    l = s.readline()
    if l:
        print(l.decode('utf-8', errors='ignore').strip())
s.close()
