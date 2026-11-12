#!/usr/bin/env python3
"""Modbus RTU master (via USB->RS485-adapter) der sender data til MQTT.

Poller ESP32-C6'en (unit 1) og publicerer JSON på
    iiot/modbus/<unit>/telemetry
og lytter efter kommandoer på
    iiot/modbus/<unit>/cmd/coil      payload: "1" / "0"
    iiot/modbus/<unit>/cmd/holding/<n>  payload: heltal 0..65535

Kør uden MQTT (kun Modbus-test):  ./modbus_mqtt_bridge.py --no-mqtt
"""

import argparse
import json
import threading
import time

from pymodbus.client import ModbusSerialClient
from pymodbus.exceptions import ModbusException

try:
    import paho.mqtt.client as mqtt
except ImportError:
    mqtt = None


def parse_args():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", default="/dev/ttyUSB0")
    p.add_argument("--baud", type=int, default=9600)
    p.add_argument("--unit", type=int, default=1)
    p.add_argument("--interval", type=float, default=2.0, help="sekunder mellem polls")
    p.add_argument("--broker", default="localhost")
    p.add_argument("--broker-port", type=int, default=1883)
    p.add_argument("--no-mqtt", action="store_true")
    return p.parse_args()


def main():
    args = parse_args()
    topic_base = f"iiot/modbus/{args.unit}"

    client = ModbusSerialClient(port=args.port, baudrate=args.baud,
                                bytesize=8, parity="N", stopbits=1, timeout=1)
    if not client.connect():
        raise SystemExit(f"Kunne ikke åbne {args.port} (tilsluttet? i gruppen 'dialout'?)")
    # Bus-adgang fra både poll-løkke og MQTT-callbacks skal serialiseres
    bus_lock = threading.Lock()

    mq = None
    if not args.no_mqtt:
        if mqtt is None:
            raise SystemExit("paho-mqtt mangler: pip install -r requirements.txt")
        mq = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)

        def on_connect(c, _userdata, _flags, reason_code, _props):
            print(f"MQTT forbundet ({reason_code})")
            c.subscribe(f"{topic_base}/cmd/#")

        def on_message(_c, _userdata, msg):
            sub = msg.topic.removeprefix(f"{topic_base}/cmd/")
            payload = msg.payload.decode().strip()
            try:
                with bus_lock:
                    if sub == "coil":
                        rr = client.write_coil(0, payload in ("1", "on", "true"), slave=args.unit)
                    elif sub.startswith("holding/"):
                        rr = client.write_register(int(sub.split("/")[1]), int(payload),
                                                   slave=args.unit)
                    else:
                        print(f"Ukendt kommando: {msg.topic}")
                        return
                print(f"CMD {sub}={payload} -> {'FEJL ' + str(rr) if rr.isError() else 'OK'}")
            except ValueError:
                print(f"Ugyldig payload på {msg.topic}: {payload!r}")
            except ModbusException as e:
                print(f"CMD {sub}={payload} -> FEJL {e}")

        mq.on_connect = on_connect
        mq.on_message = on_message
        mq.connect(args.broker, args.broker_port)
        mq.loop_start()

    try:
        while True:
            try:
                with bus_lock:
                    ir = client.read_input_registers(0, count=3, slave=args.unit)
                    hr = client.read_holding_registers(0, count=8, slave=args.unit)
                    co = client.read_coils(0, count=1, slave=args.unit)
            except ModbusException as e:
                print(f"Intet svar fra unit {args.unit}: {e}")
                time.sleep(args.interval)
                continue

            if ir.isError() or hr.isError() or co.isError():
                print(f"Modbus-fejl/timeout: {ir if ir.isError() else hr if hr.isError() else co}")
            else:
                data = {
                    "ts": time.time(),
                    "uptime_s": ir.registers[0],
                    "temperature_c": ir.registers[1] / 10,
                    "request_count": ir.registers[2],
                    "holding": hr.registers,
                    "coil": co.bits[0],
                }
                print(json.dumps(data))
                if mq:
                    mq.publish(f"{topic_base}/telemetry", json.dumps(data))
            time.sleep(args.interval)
    except KeyboardInterrupt:
        pass
    finally:
        client.close()
        if mq:
            mq.loop_stop()
            mq.disconnect()


if __name__ == "__main__":
    main()
