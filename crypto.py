#!/usr/bin/env python3
import os
import base64
from Crypto.Cipher import AES
from Crypto.Util.Padding import pad, unpad
from Crypto.Random import get_random_bytes

class LoRaTCrypto:
    def __init__(self, key=None):
        if key:
            self.key = key
        else:
            self.key = get_random_bytes(32)
    
    @staticmethod
    def generate_key():
        return get_random_bytes(32)
    
    def encrypt(self, plaintext):
        try:
            if isinstance(plaintext, str):
                plaintext = plaintext.encode('utf-8')
            iv = get_random_bytes(16)
            cipher = AES.new(self.key, AES.MODE_CBC, iv)
            padded_data = pad(plaintext, AES.block_size)
            ciphertext = cipher.encrypt(padded_data)
            combined = iv + ciphertext
            return base64.b64encode(combined).decode('utf-8')
        except Exception as e:
            print(f"[!] Encryption error: {e}")
            return None
    
    def decrypt(self, b64_data):
        try:
            if isinstance(b64_data, str):
                combined = base64.b64decode(b64_data)
            else:
                combined = b64_data
            if len(combined) < 16:
                print(f"[!] Decryption error: Data too short ({len(combined)} bytes)")
                return None
            iv = combined[:16]
            ciphertext = combined[16:]
            if len(ciphertext) % 16 != 0:
                print(f"[!] Decryption error: Ciphertext length not a multiple of block size")
                return None
            cipher = AES.new(self.key, AES.MODE_CBC, iv)
            padded_plaintext = cipher.decrypt(ciphertext)
            plaintext = unpad(padded_plaintext, AES.block_size)
            return plaintext.decode('utf-8')
        except Exception as e:
            print(f"[!] Decryption error: {e}")
            return None
    
    def get_key_b64(self):
        return base64.b64encode(self.key).decode('utf-8')
    
    @staticmethod
    def set_key_from_b64(b64_key):
        return base64.b64decode(b64_key)