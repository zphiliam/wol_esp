# send_config.py
import serial, time

port = "/dev/cu.usbserial-1120"  # 改成你的端口
cmds = open("config.txt").readlines()

with serial.Serial(port, 115200, timeout=1) as s:
    time.sleep(1)
    s.write(b"config\n")
    time.sleep(0.5)  # 等待 ESP8266 进入配置模式
    for line in cmds:
        if line.strip() == "" or line.startswith("#"):
            continue  # 跳过空行和注释
        print(f"发送: {line.strip()}")
        s.write(line.encode())
        s.flush()
        time.sleep(0.2)  # 每行等 200ms，给 ESP8266 时间处理
        print(s.read_all().decode(errors='ignore'), end='')
    
    # 后面持续监听 ESP8266 输出
    while True:
        output = s.read_all().decode(errors='ignore')
        if output:
            print(output, end='')
        time.sleep(0.5)  # 每 500ms 检查一次输出
