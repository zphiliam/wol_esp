# send_config.py
import serial, time

port = "/dev/cu.usbserial-1120"  # 改成你的端口
cmds = open("config.txt").readlines()

with serial.Serial(port, 115200, timeout=1) as s:
    time.sleep(1)
    for line in cmds:
        s.write(line.encode())
        s.flush()
        time.sleep(0.2)  # 每行等 200ms，给 ESP8266 时间处理
        print(s.read_all().decode(errors='ignore'), end='')
