#!/usr/bin/env python3
"""
WebSocket Bridge Server for HC-06 SPP Communication
Connects web application to Arduino via HC-06 Bluetooth module (Classic Bluetooth/SPP)

Requirements:
    pip install websockets pyserial

Usage:
    python bridge_server.py

The server listens on ws://localhost:8765
"""

import asyncio
import json
import serial
import serial.tools.list_ports
import websockets
from typing import Optional
from datetime import datetime

def make_settime_command() -> str:
    now = datetime.now()
    return f"SETTIME:{now.strftime('%Y-%m-%d-%H-%M-%S')}"

class BluetoothBridge:
    def __init__(self):
        self.serial_port: Optional[serial.Serial] = None
        self.port_name: Optional[str] = None
    
    def find_hc06_port(self) -> Optional[str]:
        """
        Find HC-06 port by sending PING and expecting COOK response
        """
        import time
        ports = serial.tools.list_ports.comports()
        
        print("Scanning ports for HC-06...")
        print("Available ports:")
        for port in ports:
            print(f"  {port.device}: {port.description}")
        
        for port in ports:
            try:
                print(f"\nTrying {port.device}...")
                test_serial = serial.Serial(
                    port=port.device,
                    baudrate=9600,
                    timeout=2,
                    write_timeout=2
                )
                print(f"port opened: {port.device}")
                time.sleep(0.5)
                print("waited")
                test_serial.reset_input_buffer()
                print("input buffer reset")
                test_serial.reset_output_buffer()
                print("output buffer reset")
                
                # Send PING command
                test_serial.write(b"PING\n")
                print("written PING")
                test_serial.flush()
                print(f"  Sent: PING")
                
                # Wait for response
                time.sleep(0.5)
                response = test_serial.readline().decode('utf-8').strip()
                print(f"  Received: '{response}' (len={len(response)}, repr={repr(response)})")
                
                test_serial.close()
                
                # Check if response is COOK
                if "COOK" in response:
                    print(f"\n✓ Found HC-06 on {port.device}!")
                    return port.device
                    
            except Exception as e:
                print(f"  Error: {e}")
                continue
        
        print("\n✗ HC-06 not found on any port")
        return None
    
    def connect(self, port: Optional[str] = None, baudrate: int = 9600) -> bool:
        """
        Connect to HC-06 via serial port
        Auto-detect using PING/COOK if port not specified.
        """
        try:
            if not port:
                port = self.find_hc06_port()
            
            if not port:
                print("No HC-06 port found")
                return False
            
            print(f"Connecting to {port}...")
            
            self.serial_port = serial.Serial(
                port=port,
                baudrate=baudrate,
                timeout=2,
                write_timeout=5
            )
            import time
            time.sleep(0.5)
            self.serial_port.reset_input_buffer()
            self.serial_port.reset_output_buffer()
            self.port_name = port
            print(f"Connected to HC-06 on {port}")
            return True
            
        except serial.SerialException as e:
            print(f"Failed to connect: {e}")
            return False
        
    def disconnect(self):
        if self.serial_port and self.serial_port.is_open:
            self.serial_port.close()
            print("Disconnected from HC-06")
        self.serial_port = None
        self.port_name = None
    
    def send_command(self, command: str) -> Optional[str]:
        if not self.serial_port or not self.serial_port.is_open:
            print("Not connected to HC-06")
            return None
        
        try:
            import time
            self.serial_port.reset_input_buffer()
            self.serial_port.reset_output_buffer()
            time.sleep(0.1)
            
            command_bytes = f"{command}\n".encode('utf-8')
            self.serial_port.write(command_bytes)
            self.serial_port.flush()
            print(f"Sent: {command}")
            
            time.sleep(0.3)
            responses = []
            while self.serial_port.in_waiting > 0 or len(responses) == 0:
                line = self.serial_port.readline().decode('utf-8').strip()
                if line:
                    print(f"Received: {line}")
                    responses.append(line)
                if "REQTIME" in line:
                    settime=make_settime_command()
                    print(f"Auto Reply: {settime}")
                    self.serial_port.write(f"{settime}\n".encode('utf-8'))
                    self.serial_port.flush()
                if self.serial_port.in_waiting == 0:
                    time.sleep(0.1)
                    if self.serial_port.in_waiting == 0:
                        break
            return "\n".join(responses) if responses else "OK"
            
        except serial.SerialTimeoutException as e:
            print(f"Write timeout: {e}")
            return None
        except Exception as e:
            print(f"Error sending command: {e}")
            return None
    
    def is_connected(self) -> bool:
        return self.serial_port is not None and self.serial_port.is_open


bridge = BluetoothBridge()


async def handle_client(websocket):
    print(f"Client connected: {websocket.remote_address}")
    
    try:
        async for message in websocket:
            try:
                data = json.loads(message)
                msg_type = data.get('type')
                
                if msg_type == 'connect':
                    port = data.get('port')
                    success = bridge.connect(port)
                    
                    if success:
                        await websocket.send(json.dumps({
                            'status': 'connected',
                            'port': bridge.port_name
                        }))
                    else:
                        await websocket.send(json.dumps({
                            'status': 'failed',
                            'message': 'HC-06 포트를 찾을 수 없습니다.'
                        }))
                
                elif msg_type == 'disconnect':
                    bridge.disconnect()
                    await websocket.send(json.dumps({
                        'status': 'disconnected'
                    }))
                
                elif msg_type == 'command':
                    command = data.get('data', '')
                    
                    if not bridge.is_connected():
                        await websocket.send(json.dumps({
                            'status': 'error',
                            'message': 'HC-06이 연결되어 있지 않습니다.'
                        }))
                        continue
                    
                    response = bridge.send_command(command)
                    
                    if response:
                        await websocket.send(json.dumps({
                            'status': 'ok',
                            'response': response
                        }))
                    else:
                        await websocket.send(json.dumps({
                            'status': 'error',
                            'message': '명령 전송 실패'
                        }))
                
                elif msg_type == 'status':
                    await websocket.send(json.dumps({
                        'status': 'connected' if bridge.is_connected() else 'disconnected',
                        'port': bridge.port_name
                    }))
                
                else:
                    await websocket.send(json.dumps({
                        'status': 'error',
                        'message': f'Unknown message type: {msg_type}'
                    }))
                    
            except json.JSONDecodeError:
                await websocket.send(json.dumps({
                    'status': 'error',
                    'message': 'Invalid JSON'
                }))
                
    except websockets.exceptions.ConnectionClosed:
        print(f"Client disconnected: {websocket.remote_address}")


async def main():
    print("=" * 50)
    print("HC-06 WebSocket Bridge Server")
    print("=" * 50)
    print("Listening on ws://localhost:8765")
    print("Press Ctrl+C to stop")
    print("=" * 50)
    
    async with websockets.serve(handle_client, "localhost", 8765):
        await asyncio.Future()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nServer stopped")
        bridge.disconnect()
