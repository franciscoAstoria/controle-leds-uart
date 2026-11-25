#!/usr/bin/env python3
"""
Envia pacotes de teste para o receptor UART.
Uso:  pip install pyserial
      python test_uart.py COM3          (Windows)
      python test_uart.py /dev/ttyACM0  (Linux)
Obs.: feche o monitor serial antes, ou use outra porta.
"""
import sys
import time
import serial

USE_CRC8 = False  # deve ser igual ao USE_CRC8 do main.c
DEVICE_ID = 0x01


def xor(data):
    x = 0
    for b in data:
        x ^= b
    return x


def crc8(data):
    crc = 0x00
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def make_packet(r, g, b, action, dev_id=DEVICE_ID):
    body = bytes([dev_id, r, g, b, action])
    chk = crc8(body) if USE_CRC8 else xor(body)
    return body + bytes([chk])


def send(ser, pkt, label):
    print(f"{label:28s} -> {pkt.hex(' ').upper()}")
    ser.write(pkt)
    time.sleep(0.1)
    resp = ser.read(ser.in_waiting or 1)
    if resp:
        print("   placa:", resp.decode(errors="replace").strip())
    time.sleep(1.5)


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
    with serial.Serial(port, 115200, bytesize=8, parity="N", stopbits=1, timeout=0.2) as ser:
        time.sleep(0.5)
        send(ser, make_packet(255, 0, 0, 0x01), "LIGAR vermelho")
        send(ser, make_packet(0, 40, 0, 0x01), "LIGAR verde")
        send(ser, make_packet(0, 0, 255, 0x03), "TOGGLE azul")
        send(ser, make_packet(0, 0, 255, 0x03), "TOGGLE azul de novo")
        send(ser, make_packet(255, 255, 0, 0x04), "PISCA 1s amarelo")
        time.sleep(3)
        send(ser, make_packet(0, 255, 255, 0x05), "PISCA 2s ciano")
        time.sleep(4)

        # Pacotes invalidos: o sistema deve continuar funcionando
        send(ser, make_packet(255, 0, 0, 0x01, dev_id=0x07), "ID errado")
        bad = bytearray(make_packet(255, 0, 0, 0x01))
        bad[5] ^= 0xFF
        send(ser, bytes(bad), "CHECK errado")
        send(ser, make_packet(255, 0, 0, 0x09), "ACAO desconhecida")
        send(ser, make_packet(255, 0, 0, 0x01)[:3], "Truncado (3 bytes)")

        # Prova de que nao travou
        send(ser, make_packet(255, 255, 255, 0x01), "LIGAR branco (apos erros)")
        send(ser, make_packet(0, 0, 0, 0x02), "DESLIGAR")


if __name__ == "__main__":
    main()
