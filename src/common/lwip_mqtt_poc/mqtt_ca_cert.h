#pragma once

// Replace this with your CA certificate (PEM) that signed the broker cert.
// Keep the trailing newline.
static const char MQTT_CA_CERT_PEM[] =
"-----BEGIN CERTIFICATE-----\n"
"...PASTE_YOUR_CA_CERT_BASE64_HERE...\n"
"-----END CERTIFICATE-----\n";