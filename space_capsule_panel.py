"""SpaceCapsule desktop controller. Python 3 + pyserial; firmware owns all timing."""
import math
import time
import tkinter as tk
from tkinter import ttk, messagebox
import serial
from serial.tools import list_ports


class ControlPanel:
    def __init__(self, root):
        self.root = root
        self.port = None
        self.rx = bytearray()
        self.last_status = None
        self.awaiting_boundary = False
        root.title('太空艙最後三分鐘｜控制面板')
        root.geometry('820x650')
        root.minsize(720, 570)
        self.com = tk.StringVar()
        self.connection = tk.StringVar(value='未連線')
        self.power_input = tk.StringVar(value='0')
        self.values = {key: tk.StringVar(value='—') for key in
                       ('任務狀態', '剩餘時間', '備援電源', '核心輸出', '穩定時間', '確認資格')}
        frame = ttk.Frame(root, padding=16)
        frame.pack(fill='both', expand=True)
        ttk.Label(frame, text='太空艙最後三分鐘', font=('Microsoft JhengHei', 20, 'bold')).pack(anchor='w')
        ttk.Label(frame, text='計時與結果以 STM32 回報為準；最後請按板上 B1 確認。').pack(anchor='w', pady=(4, 12))
        top = ttk.Frame(frame)
        top.pack(fill='x')
        ttk.Label(top, text='序列埠').pack(side='left')
        self.combo = ttk.Combobox(top, textvariable=self.com, width=13, state='readonly')
        self.combo.pack(side='left', padx=8)
        ttk.Button(top, text='重新掃描', command=self.scan).pack(side='left')
        self.connect_button = ttk.Button(top, text='連線', command=self.toggle)
        self.connect_button.pack(side='left', padx=8)
        ttk.Label(top, textvariable=self.connection).pack(side='left')
        status = ttk.LabelFrame(frame, text='板子狀態', padding=12)
        status.pack(fill='x', pady=14)
        for i, (key, value) in enumerate(self.values.items()):
            row, col = divmod(i, 2)
            ttk.Label(status, text=key + '：').grid(row=row, column=col*2, sticky='w', pady=6)
            ttk.Label(status, textvariable=value, font=('Microsoft JhengHei', 12, 'bold')).grid(
                row=row, column=col*2+1, sticky='w', padx=(5, 24), pady=6)
        status.columnconfigure(1, weight=1)
        status.columnconfigure(3, weight=1)
        controls = ttk.LabelFrame(frame, text='操作', padding=12)
        controls.pack(fill='x')
        for label, command in [('啟動任務', 'START'), ('接通備援', 'BACKUP ON'),
                               ('查詢狀態', 'STATUS'), ('結果後重設', 'RESET')]:
            ttk.Button(controls, text=label, command=lambda c=command: self.send(c)).pack(side='left', padx=4)
        power = ttk.Frame(frame)
        power.pack(fill='x', pady=12)
        ttk.Label(power, text='核心設定 0～100%：').pack(side='left')
        ttk.Spinbox(power, from_=0, to=100, textvariable=self.power_input, width=7).pack(side='left', padx=6)
        ttk.Button(power, text='送出輸出設定', command=self.set_power).pack(side='left')
        ttk.Label(power, text='需保持 60% 至少 5 秒，再按實體 B1。').pack(side='left', padx=12)
        ttk.Label(frame, text='通訊紀錄（TX 傳送／RX 接收）').pack(anchor='w')
        logs = ttk.Frame(frame)
        logs.pack(fill='both', expand=True, pady=(5, 0))
        self.log = tk.Text(logs, height=12, wrap='word', state='disabled', font=('Consolas', 10))
        scrollbar = ttk.Scrollbar(logs, command=self.log.yview)
        self.log.configure(yscrollcommand=scrollbar.set)
        self.log.pack(side='left', fill='both', expand=True)
        scrollbar.pack(side='right', fill='y')
        self.scan()
        root.protocol('WM_DELETE_WINDOW', self.close)
        root.after(50, self.poll)

    def write_log(self, text):
        self.log.configure(state='normal')
        self.log.insert('end', time.strftime('%H:%M:%S ') + text + '\n')
        if int(self.log.index('end-1c').split('.')[0]) > 1200:
            self.log.delete('1.0', '201.0')
        self.log.see('end')
        self.log.configure(state='disabled')

    def scan(self):
        if self.port:
            return
        ports = list(list_ports.comports())
        self.combo['values'] = [p.device for p in ports]
        if self.com.get() not in self.combo['values']:
            preferred = next((p.device for p in ports if 'stlink' in (p.description or '').lower().replace('-', '')), '')
            self.com.set(preferred or (ports[0].device if ports else ''))
        for p in ports:
            self.write_log(f'PORT {p.device}: {p.description}')

    def toggle(self):
        if self.port:
            self.disconnect()
            return
        if not self.com.get():
            messagebox.showinfo('選擇序列埠', '請接上板子、重新掃描並選擇 ST-LINK COM 埠。')
            return
        try:
            self.port = serial.Serial(self.com.get(), baudrate=115200, bytesize=8,
                                      parity='N', stopbits=1, timeout=0, write_timeout=0.5)
            self.rx.clear()
            self.last_status = None
            self.connection.set('已開啟序列埠，等待板子回報')
            self.combo.configure(state='disabled')
            self.connect_button.configure(text='斷線')
            self.write_log('OPEN ' + self.com.get())
            self.port.write(b'\n')
            self.root.after(200, self.request_status)
        except (serial.SerialException, OSError) as exc:
            self.disconnect()
            messagebox.showerror('無法連線', str(exc))

    def request_status(self):
        if self.port:
            self.send('STATUS')

    def disconnect(self):
        if self.port:
            try:
                self.port.close()
            except (serial.SerialException, OSError):
                pass
        self.port = None
        self.rx.clear()
        self.last_status = None
        self.connection.set('未連線；畫面數值為上次回報')
        self.connect_button.configure(text='連線')
        self.combo.configure(state='readonly')

    def send(self, command):
        if not self.port:
            messagebox.showinfo('尚未連線', '請先連線到 ST-LINK 序列埠。')
            return
        try:
            self.port.write((command + '\n').encode('ascii'))
            self.write_log('TX ' + command)
        except (serial.SerialException, OSError) as exc:
            self.write_log('ERROR ' + str(exc))
            self.disconnect()

    def set_power(self):
        text = self.power_input.get().strip()
        if not text.isascii() or not text.isdecimal() or not 0 <= int(text) <= 100:
            messagebox.showinfo('輸出設定', '請輸入 0～100 的整數。')
            return
        self.send('POWER ' + str(int(text)))

    def receive_line(self, line):
        self.write_log('RX ' + line)
        if not line.startswith('STATUS '):
            return
        fields = dict(word.split('=', 1) for word in line.split()[1:] if '=' in word)
        try:
            state = fields['state']
            remaining = max(0, int(fields['remaining_ms']))
            stable = max(0, int(fields['stable_ms']))
            output = int(fields['power'])
            backup = fields['backup']
            ready = fields['ready']
        except (KeyError, ValueError):
            self.write_log('WARN 無法解析這筆 STATUS')
            return
        names = {'IDLE': '待命', 'RUNNING': '維修中', 'SUCCESS': '重啟成功', 'FAILED': '任務失敗'}
        seconds = math.ceil(remaining / 1000)
        self.values['任務狀態'].set(names.get(state, state))
        self.values['剩餘時間'].set(f'{seconds//60:02d}:{seconds%60:02d}')
        self.values['備援電源'].set('已接通' if backup == '1' else '未接通')
        self.values['核心輸出'].set(f'{output}%')
        self.values['穩定時間'].set(f'{stable/1000:.2f} / 5 秒')
        self.values['確認資格'].set('可按 B1' if ready == '1' else '尚未符合／已結束')
        self.last_status = time.monotonic()
        self.connection.set('通訊正常｜已收到板子狀態')

    def poll(self):
        if self.port:
            try:
                chunk = self.port.read(min(self.port.in_waiting, 4096))
                self.rx.extend(chunk)
                while b'\n' in self.rx:
                    line, _, rest = self.rx.partition(b'\n')
                    self.rx = bytearray(rest)
                    text = line.decode('ascii', errors='replace').strip()
                    if text:
                        self.receive_line(text)
                if len(self.rx) > 8192:
                    self.rx.clear()
                    self.write_log('WARN 接收資料過長，已清除未完成的一行')
                if self.last_status is not None and time.monotonic() - self.last_status > 2:
                    self.connection.set('已連線｜數值為上次回報，可按「查詢狀態」更新')
            except (serial.SerialException, OSError) as exc:
                self.write_log('ERROR ' + str(exc))
                self.disconnect()
        self.root.after(50, self.poll)

    def close(self):
        self.disconnect()
        self.root.destroy()


if __name__ == '__main__':
    window = tk.Tk()
    ControlPanel(window)
    window.mainloop()
