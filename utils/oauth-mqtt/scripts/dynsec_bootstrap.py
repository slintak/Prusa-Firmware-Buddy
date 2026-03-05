#!/usr/bin/env python3
import os
import time

from oauth_server.config import load_config
from oauth_server.mqtt_provisioning import DynSecProvisioner, MosquittoCtrlProvisioner


def main() -> None:
    config = load_config()
    mqtt_cfg = dict(config.get("mqtt", {}))

    if not mqtt_cfg.get("enabled"):
        return

    admin_pass = os.getenv("DYNSEC_ADMIN_PASSWORD")
    if not admin_pass:
        raise SystemExit("DYNSEC_ADMIN_PASSWORD must be set")

    control_user = mqtt_cfg.get("username")
    control_pass = mqtt_cfg.get("password")
    if not control_user or not control_pass or control_pass == "change-me":
        raise SystemExit("mqtt.username/mqtt.password must be set in config.yaml")

    mqtt_cfg["username"] = "admin"
    mqtt_cfg["password"] = admin_pass
    mqtt_cfg["client_id"] = os.getenv("DYNSEC_ADMIN_CLIENT_ID", "dynsec-admin")

    provisioner_type = str(mqtt_cfg.get("provisioner", "mosquitto_ctrl")).lower()
    if provisioner_type == "control_topic":
        provisioner = DynSecProvisioner(mqtt_cfg)
    else:
        provisioner = MosquittoCtrlProvisioner(mqtt_cfg)

    retries = int(os.getenv("DYNSEC_BOOTSTRAP_RETRIES", "30"))
    delay = float(os.getenv("DYNSEC_BOOTSTRAP_DELAY", "1.0"))

    for attempt in range(retries):
        try:
            provisioner.connect()
            break
        except Exception:
            if attempt == retries - 1:
                raise
            time.sleep(delay)

    provisioner.ensure_control_role()
    provisioner.ensure_role(mqtt_cfg.get("device_role"), mqtt_cfg.get("device_acls", []))

    provisioner.create_client(control_user, control_pass, mqtt_cfg.get("control_role"))
    provisioner.set_client_password(control_user, control_pass)

    provisioner.close()


if __name__ == "__main__":
    main()
