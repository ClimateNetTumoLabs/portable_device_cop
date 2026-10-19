#pragma once
// Fill in the six values below (see the README, "Set up AWS by hand").
// Never share this file or put it on GitHub: the private key lets anyone pretend to be your device.

const char WIFI_SSID[] = "";
const char WIFI_PASS[] = "";

// AWS IoT console -> Settings -> Device data endpoint
const char AWS_IOT_ENDPOINT[] = "";

// Must match the Thing name (it is also the MQTT client ID the device policy allows)
const char THING_NAME[] = "";

static const char AWS_ROOT_CA[] = R"PEM(

)PEM";

static const char DEVICE_CERT[] = R"PEM(

)PEM";

static const char DEVICE_KEY[] = R"PEM(

)PEM";

