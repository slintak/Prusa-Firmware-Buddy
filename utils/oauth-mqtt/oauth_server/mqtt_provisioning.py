import json
import logging
import threading
import time
import uuid
from typing import Any, Dict, List, Optional

import paho.mqtt.client as mqtt
import subprocess

logger = logging.getLogger("oauth_server.mqtt")


class DynSecProvisioner:
    def __init__(self, config: Dict[str, Any]) -> None:
        self.enabled = bool(config.get("enabled"))
        self.host = config.get("host", "localhost")
        self.port = int(config.get("port", 8883))
        self.username = config.get("username", "")
        self.password = config.get("password", "")
        self.client_id = config.get("client_id", "oauth-control")
        self.keepalive = int(config.get("keepalive", 30))
        self.control_topic = config.get("control_topic", "$CONTROL/dynamic-security/v1")
        self.response_topic = config.get("response_topic", "$CONTROL/dynamic-security/v1/response")
        self.device_role = config.get("device_role", "device-role")
        self.device_acls = config.get("device_acls", [])
        self.control_role = config.get("control_role", "oauth-control")
        self.control_acls = config.get("control_acls", [])
        self.tls_config = config.get("tls", {})

        self._client: Optional[mqtt.Client] = None
        self._responses: Dict[str, Dict[str, Any]] = {}
        self._cv = threading.Condition()
        self._ready = threading.Event()

    def connect(self) -> None:
        if not self.enabled:
            return
        if self._client:
            return

        client = mqtt.Client(client_id=self.client_id, protocol=mqtt.MQTTv311)
        if self.username:
            client.username_pw_set(self.username, self.password)

        ca_file = self.tls_config.get("ca_file") or None
        cert_file = self.tls_config.get("cert_file") or None
        key_file = self.tls_config.get("key_file") or None
        insecure = bool(self.tls_config.get("insecure"))
        if ca_file:
            client.tls_set(ca_certs=ca_file, certfile=cert_file, keyfile=key_file)
            client.tls_insecure_set(insecure)

        client.on_connect = self._on_connect
        client.on_subscribe = self._on_subscribe
        client.on_message = self._on_message
        client.connect(self.host, self.port, keepalive=self.keepalive)
        client.loop_start()

        if not self._ready.wait(timeout=5.0):
            client.loop_stop()
            raise TimeoutError("dynsec client subscription timeout")

        self._client = client
        logger.info("connected to mosquitto dynsec control topic at %s:%s", self.host, self.port)

    def close(self) -> None:
        if not self._client:
            return
        self._client.loop_stop()
        self._client.disconnect()
        self._client = None

    def _on_connect(self, client: mqtt.Client, _userdata: Any, _flags: Dict[str, Any], rc: int) -> None:
        if rc != 0:
            logger.error("dynsec connect failed with rc=%s", rc)
            return
        client.subscribe(self.response_topic, qos=1)

    def _on_subscribe(
        self,
        _client: mqtt.Client,
        _userdata: Any,
        _mid: int,
        _granted_qos: List[int],
    ) -> None:
        self._ready.set()

    def _on_message(self, _client: mqtt.Client, _userdata: Any, msg: mqtt.MQTTMessage) -> None:
        try:
            payload = json.loads(msg.payload.decode("utf-8"))
        except json.JSONDecodeError:
            logger.warning("invalid dynsec response payload")
            return

        responses = payload.get("responses", [])
        with self._cv:
            for response in responses:
                correlation = response.get("correlationData")
                if correlation:
                    self._responses[correlation] = response
            self._cv.notify_all()

    def _send_commands(self, commands: List[Dict[str, Any]], timeout: float = 5.0) -> List[Dict[str, Any]]:
        if not self._client:
            raise RuntimeError("dynsec client not connected")

        correlations = []
        for command in commands:
            if "correlationData" not in command:
                command["correlationData"] = uuid.uuid4().hex
            correlations.append(command["correlationData"])

        payload = json.dumps({"commands": commands})
        self._client.publish(self.control_topic, payload, qos=1)

        deadline = time.time() + timeout
        responses = []
        with self._cv:
            while True:
                missing = [c for c in correlations if c not in self._responses]
                if not missing:
                    break
                remaining = deadline - time.time()
                if remaining <= 0:
                    raise TimeoutError("dynsec response timeout")
                self._cv.wait(timeout=remaining)

            for correlation in correlations:
                responses.append(self._responses.pop(correlation))

        return responses

    def _ignore_errors(self, responses: List[Dict[str, Any]], allowed: List[str]) -> None:
        for response in responses:
            error = response.get("error")
            if error and error not in allowed:
                raise RuntimeError(f"dynsec error: {error}")

    def ensure_role(self, role_name: str, acls: List[Dict[str, Any]]) -> None:
        if not acls:
            return
        responses = self._send_commands(
            [
                {
                    "command": "createRole",
                    "rolename": role_name,
                    "acls": acls,
                }
            ]
        )
        self._ignore_errors(responses, ["Role already exists"])

    def ensure_control_role(self) -> None:
        if not self.control_acls:
            return
        responses = self._send_commands(
            [
                {
                    "command": "createRole",
                    "rolename": self.control_role,
                    "acls": self.control_acls,
                }
            ]
        )
        self._ignore_errors(responses, ["Role already exists"])

    def create_client(self, username: str, password: str, role_name: str) -> None:
        responses = self._send_commands(
            [
                {
                    "command": "createClient",
                    "username": username,
                    "password": password,
                    "roles": [{"rolename": role_name, "priority": 1}],
                }
            ]
        )
        self._ignore_errors(responses, ["Client already exists"])

    def set_client_password(self, username: str, password: str) -> None:
        responses = self._send_commands(
            [
                {
                    "command": "setClientPassword",
                    "username": username,
                    "password": password,
                }
            ]
        )
        self._ignore_errors(responses, [])

    def delete_client(self, username: str) -> None:
        responses = self._send_commands(
            [
                {
                    "command": "deleteClient",
                    "username": username,
                }
            ]
        )
        self._ignore_errors(responses, ["Client not found"])

    def provision_device(self, device_id: str, access_token: str) -> None:
        if not self.enabled:
            return
        self.connect()
        self.ensure_role(self.device_role, self.device_acls)
        self.create_client(device_id, access_token, self.device_role)
        self.set_client_password(device_id, access_token)
        logger.info("mqtt credentials provisioned for device_id=%s", device_id)

    def revoke_device(self, device_id: str) -> None:
        if not self.enabled:
            return
        self.connect()
        self.delete_client(device_id)
        logger.info("mqtt credentials revoked for device_id=%s", device_id)


_provisioner: Optional[object] = None


def init_provisioner(config: Dict[str, Any]) -> object:
    global _provisioner
    if _provisioner is None:
        provisioner_type = str(config.get("provisioner", "mosquitto_ctrl")).lower()
        if provisioner_type == "control_topic":
            _provisioner = DynSecProvisioner(config)
        else:
            _provisioner = MosquittoCtrlProvisioner(config)
    return _provisioner


def get_provisioner() -> Optional[object]:
    return _provisioner


class MosquittoCtrlProvisioner:
    def __init__(self, config: Dict[str, Any]) -> None:
        self.enabled = bool(config.get("enabled"))
        self.host = config.get("host", "localhost")
        self.port = int(config.get("port", 8883))
        self.username = config.get("username", "")
        self.password = config.get("password", "")
        self.device_role = config.get("device_role", "device-role")
        self.device_role_per_client = bool(config.get("device_role_per_client", True))
        self.device_acls = config.get("device_acls", [])
        self.control_role = config.get("control_role", "oauth-control")
        self.control_acls = config.get("control_acls", [])
        self.tls_config = config.get("tls", {})

    def connect(self) -> None:
        if not self.enabled:
            return
        if not self.username or not self.password:
            raise RuntimeError("mqtt.username/mqtt.password must be set for mosquitto_ctrl")

    def close(self) -> None:
        return

    def _base_args(self) -> List[str]:
        args = [
            "mosquitto_ctrl",
            "-h",
            self.host,
            "-p",
            str(self.port),
            "-u",
            self.username,
            "-P",
            self.password,
            "--tls-version",
            "tlsv1.2",
            "--ciphers",
            "ECDHE-ECDSA-AES128-GCM-SHA256",
        ]
        ca_file = self.tls_config.get("ca_file")
        if ca_file:
            args += ["--cafile", ca_file]
        if self.tls_config.get("insecure"):
            args.append("--insecure")
        return args

    def _run(self, args: List[str], ignore_errors: Optional[List[str]] = None) -> None:
        ignore_errors = ignore_errors or []
        proc = subprocess.run(args, capture_output=True, text=True)
        if proc.returncode == 0:
            return
        stderr = proc.stderr.strip()
        if any(msg in stderr for msg in ignore_errors):
            return
        raise RuntimeError(stderr or "mosquitto_ctrl failed")

    def ensure_role(self, role_name: str, acls: List[Dict[str, Any]]) -> None:
        if not acls:
            return
        self._run(
            self._base_args() + ["dynsec", "createRole", role_name],
            ignore_errors=["already exists"],
        )
        for acl in acls:
            if not acl.get("allow", True):
                continue
            acltype = acl["acltype"]
            topic = acl["topic"]
            self._run(
                self._base_args()
                + ["dynsec", "addRoleACL", role_name, acltype, topic, "allow", "1"],
                ignore_errors=["already exists", "already present"],
            )

    def ensure_control_role(self) -> None:
        if not self.control_acls:
            return
        self._run(
            self._base_args() + ["dynsec", "createRole", self.control_role],
            ignore_errors=["already exists"],
        )
        for acl in self.control_acls:
            if not acl.get("allow", True):
                continue
            acltype = acl["acltype"]
            topic = acl["topic"]
            self._run(
                self._base_args()
                + ["dynsec", "addRoleACL", self.control_role, acltype, topic, "allow", "1"],
                ignore_errors=["already exists", "already present"],
            )

    def create_client(self, username: str, password: str, role_name: str) -> None:
        self._run(
            self._base_args() + ["dynsec", "createClient", username, "-p", password],
            ignore_errors=["already exists"],
        )
        self._run(
            self._base_args() + ["dynsec", "addClientRole", username, role_name, "1"],
            ignore_errors=["already exists", "already present"],
        )

    def set_client_password(self, username: str, password: str) -> None:
        self._run(self._base_args() + ["dynsec", "setClientPassword", username, password])

    def delete_client(self, username: str) -> None:
        self._run(
            self._base_args() + ["dynsec", "deleteClient", username],
            ignore_errors=["not found"],
        )

    def provision_device(self, device_id: str, access_token: str) -> None:
        if not self.enabled:
            return
        self.connect()
        role_name = self.device_role
        acls = self._expand_acls(self.device_acls, device_id)
        if self.device_role_per_client:
            role_name = f"{self.device_role}-{device_id}"
        self.ensure_role(role_name, acls)
        self.create_client(device_id, access_token, role_name)
        self.set_client_password(device_id, access_token)
        logger.info("mqtt credentials provisioned for device_id=%s", device_id)

    def revoke_device(self, device_id: str) -> None:
        if not self.enabled:
            return
        self.connect()
        self.delete_client(device_id)
        logger.info("mqtt credentials revoked for device_id=%s", device_id)

    @staticmethod
    def _expand_acls(acls: List[Dict[str, Any]], device_id: str) -> List[Dict[str, Any]]:
        expanded = []
        for acl in acls:
            topic = acl["topic"].replace("%u", device_id)
            expanded.append({**acl, "topic": topic})
        return expanded
