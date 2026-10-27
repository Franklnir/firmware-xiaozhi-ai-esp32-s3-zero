import re
import pathlib

sdk = pathlib.Path('sdkconfig')
if not sdk.exists():
    sdk = pathlib.Path('build/sdkconfig')
board_cfg = {
    'CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_ZERO': 'y',
    'CONFIG_USE_DEFAULT_MESSAGE_STYLE': 'y',
    'CONFIG_USE_WECHAT_MESSAGE_STYLE': 'n',
    'CONFIG_USE_MULTILINE_CHAT_MESSAGE': 'n',
    'CONFIG_USE_HOTSPOT_WIFI_PROVISIONING': 'y',
    'CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING': 'n',
    'CONFIG_SPIRAM_MODE_QUAD': 'y',
    'CONFIG_ESPTOOLPY_FLASHSIZE_4MB': 'y',
    'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME': '"partitions/v2/4m_s3.csv"',
    'CONFIG_BT_ENABLED': 'y',
    'CONFIG_BT_BLUEDROID_ENABLED': 'y',
    'CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST': 'y',
    'CONFIG_BT_BLE_DYNAMIC_ENV_MEMORY': 'y',
    'CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP': 'y',
    'CONFIG_BT_BLE_42_FEATURES_SUPPORTED': 'y',
    'CONFIG_BT_BLE_50_FEATURES_SUPPORTED': 'n',
    'CONFIG_BT_ACL_CONNECTIONS': '2',
    'CONFIG_BT_CTRL_BLE_MAX_ACT': '3',
    'CONFIG_LANGUAGE_EN_US': 'y',
}

txt = sdk.read_text(encoding='utf-8')
for key, val in board_cfg.items():
    line = f'{key}={val}'
    pat = re.compile(r'^#?\s*' + re.escape(key) + r'(=.*|\s+is not set)$', re.MULTILINE)
    if pat.search(txt):
        txt = pat.sub(line, txt)
    else:
        txt += '\n' + line
sdk.write_text(txt, encoding='utf-8')
print('Board config (Waveshare ESP32-S3-Zero) applied to sdkconfig')

