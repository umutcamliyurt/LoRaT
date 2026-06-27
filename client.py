#!/usr/bin/env python3
import serial
import time
import threading
import sys
import argparse
import subprocess
from datetime import datetime

class LoRaTClient:
    def __init__(self, serial_port, baudrate=115200):
        self.serial_port = serial_port
        self.baudrate = baudrate
        self.serial_conn = None
        self.running = False
        self.last_received = "<none>"
        self.last_sent = "<none>"
        self.sent_count = 0
        self.recv_count = 0
        self.sending_response = False
        self.send_lock = threading.Lock()
        
    def connect_serial(self):
        try:
            self.serial_conn = serial.Serial(
                port=self.serial_port,
                baudrate=self.baudrate,
                timeout=1
            )
            print(f"[+] Connected to {self.serial_port} at {self.baudrate} baud")
            return True
        except Exception as e:
            print(f"[!] Failed to connect to serial port: {e}")
            return False
    
    def send_command(self, command):
        if not self.serial_conn or not self.serial_conn.is_open:
            print("[!] Serial connection not available")
            return False
        
        try:
            command = command.strip()
            if not command:
                return False
                
            self.serial_conn.write((command + '\n').encode('utf-8'))
            self.last_sent = command
            self.sent_count += 1
            print(f"[TX {self.sent_count}] {command}")
            return True
        except Exception as e:
            print(f"[!] Failed to send command: {e}")
            return False
    
    def read_serial(self):
        if not self.serial_conn or not self.serial_conn.is_open:
            return ""
        
        if self.sending_response:
            return ""
        
        try:
            if self.serial_conn.in_waiting > 0:
                raw_data = self.serial_conn.readline()
                
                if not raw_data:
                    return ""
                
                try:
                    data = raw_data.decode('utf-8').strip()
                except UnicodeDecodeError:
                    data = raw_data.decode('utf-8', errors='replace').strip()
                
                if data:
                    self.last_received = data
                    self.recv_count += 1
                    print(f"[RX {self.recv_count}] {data}")
                    
                    response = self.process_command(data)
                    if response:
                        with self.send_lock:
                            self.sending_response = True
                            self.send_command(response)
                            self.sending_response = False
                    
                    return data
        except Exception as e:
            print(f"[!] Error reading from serial: {e}")
        
        return ""
    
    def process_command(self, command):
        print(f"[+] Executing command: {command}")
        
        try:
            result = subprocess.run(
                command, 
                shell=True, 
                stdout=subprocess.PIPE, 
                stderr=subprocess.PIPE,
                text=True,
                timeout=10
            )
            
            output = result.stdout.strip()
            error = result.stderr.strip()
            
            if output:
                response = f"RESPONSE: {output}"
            elif error:
                response = f"ERROR: {error}"
            else:
                response = "RESPONSE: Command executed successfully"
            
            return response
            
        except subprocess.TimeoutExpired:
            return "ERROR: Command timed out"
        except Exception as e:
            return f"ERROR: {str(e)}"
    
    def start(self):
        self.running = True
        
        if not self.connect_serial():
            return False
        
        print("[+] LoRaT Client started. Waiting for commands from server...")
        
        read_thread = threading.Thread(target=self._read_loop)
        read_thread.daemon = True
        read_thread.start()
        
        try:
            while self.running:
                time.sleep(0.1)
        except KeyboardInterrupt:
            self.running = False
        finally:
            self.stop()
        
        return True
    
    def _read_loop(self):
        while self.running:
            self.read_serial()
            time.sleep(0.05)
    
    def stop(self):
        self.running = False
        
        if self.serial_conn and self.serial_conn.is_open:
            self.serial_conn.close()
            print("[+] Serial connection closed")
        
        print("[+] LoRaT Client stopped")

def main():
    parser = argparse.ArgumentParser(description='LoRaT Client')
    parser.add_argument('--port', required=True, help='Serial port (e.g., /dev/ttyACM0, COM3)')
    parser.add_argument('--baudrate', type=int, default=115200, help='Serial baudrate (default: 115200)')
    
    args = parser.parse_args()
    
    client = LoRaTClient(
        serial_port=args.port,
        baudrate=args.baudrate
    )
    
    client.start()

if __name__ == '__main__':
    main()