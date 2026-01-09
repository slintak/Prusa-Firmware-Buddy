import asyncio
from amqtt.broker import Broker

BROKER_CONFIG = {
    "listeners": {
        "default": {
            "type": "tcp",
            "bind": "0.0.0.0:1883",  
        },
    },
    "sys_interval": 10,
    "auth": {
        "allow-anonymous": True, 
    },
    "topic-check": {
        "enabled": False,
    },
}


async def main():
    broker = Broker(BROKER_CONFIG)
    await broker.start()
    print("MQTT broker runs at 0.0.0.0:1883 (Ctrl+C to stop)")

    await asyncio.Event().wait()


if __name__ == "__main__":
    asyncio.run(main())
