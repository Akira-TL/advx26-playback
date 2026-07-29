#ifndef DEMO_NETWORK_CONFIG_H
#define DEMO_NETWORK_CONFIG_H

#define DEMO_WIFI_SSID          "replace-wifi-ssid"
#define DEMO_WIFI_PASSWORD      "replace-wifi-password"
#define DEMO_VIDEO_URL          "https://advx26.babelbeast.com"
#define DEMO_HTTP_TLS_NO_VERIFY (1)

/* Beken stores classic Bluetooth addresses least-significant byte first. */
#define DEMO_SPEAKER_ADDRESS \
    {0xFFU, 0xEEU, 0xDDU, 0xCCU, 0xBBU, 0xAAU} /* AA:BB:CC:DD:EE:FF */

/* Keep the deployment token only in the ignored demo_network_config.h file. */
#define DEMO_PLAYBACK_AUTHORIZATION "Bearer replace-playback-token"
#define DEMO_TRIGGER_AUTHORIZATION  "Bearer replace-trigger-token"

#endif /* DEMO_NETWORK_CONFIG_H */
