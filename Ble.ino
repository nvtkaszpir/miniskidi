// Bluetooth Low Energy (BLE) GATT server: the phone app (web/, Chrome on Android with Web
// Bluetooth) connects here.
//
// One service with four characteristics:
//   cmd      (write / write without response): text commands, see handleCarInput()
//   status   (read / notify every statusIntervalMs): {"rssi":-58,"light":1,"bucket":120,"aux":150}
//   info     (read): device info as JSON, for the Settings tab
//   settings (read): the Settings tab options as JSON (settingsJson() in Drive.ino)
// A characteristic value can be at most 512 bytes, which is why the settings have their own.
// All four need an encrypted, authenticated (passkey) link. On first use Android shows its
// pairing dialog, where the user types blePasskey. The phone stays paired (bonded) after that.
//
// One phone at a time: advertising stops while a phone is connected and restarts when it
// disconnects.
//
// The NimBLE callbacks run on the NimBLE host task. They only copy commands into a queue;
// loop() runs them, so all motor and servo work happens on the loop() task.

// Keep in sync with web/index.html
#define BLE_SERVICE_UUID "d0280000-59cd-41da-9d1e-e7992ddd2cb1"
#define BLE_CMD_UUID     "d0280001-59cd-41da-9d1e-e7992ddd2cb1"
#define BLE_STATUS_UUID  "d0280002-59cd-41da-9d1e-e7992ddd2cb1"
#define BLE_INFO_UUID    "d0280003-59cd-41da-9d1e-e7992ddd2cb1"
#define BLE_SETTINGS_UUID "d0280004-59cd-41da-9d1e-e7992ddd2cb1"

const unsigned long statusIntervalMs = 250;

// Connection parameters asked for on connect: interval 15-30 ms (units of 1.25 ms), no
// latency, supervision timeout 400 ms (units of 10 ms). The supervision timeout is how long the
// link survives without any packet; after it the connection counts as lost and the motors stop.
// The phone may pick other values; the watchdog in Drive.ino covers that case.
const uint16_t bleMinInterval = 12;
const uint16_t bleMaxInterval = 24;
const uint16_t bleSupervisionTimeout = 40;

struct BleCommand
{
  char text[64];
};

QueueHandle_t bleCommandQueue = NULL;
NimBLEServer *bleServer = NULL;
NimBLECharacteristic *bleStatusChar = NULL;
volatile uint16_t bleConnHandle = BLE_HS_CONN_HANDLE_NONE;
volatile bool bleLinkLost = false; // set by onDisconnect, handled in loop()

class MiniSkidiServerCallbacks : public NimBLEServerCallbacks
{
  void onConnect(NimBLEServer *server, NimBLEConnInfo &connInfo) override
  {
    bleConnHandle = connInfo.getConnHandle();
    LOGI("Phone connected: %s", connInfo.getAddress().toString().c_str());
    server->updateConnParams(connInfo.getConnHandle(), bleMinInterval, bleMaxInterval, 0, bleSupervisionTimeout);
  }

  void onDisconnect(NimBLEServer *server, NimBLEConnInfo &connInfo, int reason) override
  {
    bleConnHandle = BLE_HS_CONN_HANDLE_NONE;
    bleLinkLost = true;
    // 0x208 = supervision timeout (signal lost), 0x213 = closed by the phone
    LOGI("Phone disconnected (reason 0x%X), advertising again", reason);
  }

  void onMTUChange(uint16_t mtu, NimBLEConnInfo &connInfo) override
  {
    LOGD("MTU %u", mtu);
  }

  uint32_t onPassKeyDisplay() override
  {
    LOGI("Pairing: enter the passkey on the phone");
    return blePasskey;
  }

  void onAuthenticationComplete(NimBLEConnInfo &connInfo) override
  {
    if (!connInfo.isEncrypted() || !connInfo.isAuthenticated())
    {
      LOGW("Pairing failed (wrong passkey?), disconnecting %s", connInfo.getAddress().toString().c_str());
      NimBLEDevice::getServer()->disconnect(connInfo.getConnHandle());
      return;
    }
    LOGI("Secure connection with %s (bonded: %s)", connInfo.getAddress().toString().c_str(),
         connInfo.isBonded() ? "yes" : "no");
    LOGD("Connection: interval %.2f ms, supervision timeout %u ms", connInfo.getConnInterval() * 1.25,
         connInfo.getConnTimeout() * 10);
  }
} bleServerCallbacks;

class CommandCallbacks : public NimBLECharacteristicCallbacks
{
  void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &connInfo) override
  {
    NimBLEAttValue value = characteristic->getValue();
    BleCommand command;
    size_t len = std::min<size_t>(value.length(), sizeof(command.text) - 1);
    memcpy(command.text, value.data(), len);
    command.text[len] = '\0';
    if (xQueueSend(bleCommandQueue, &command, 0) != pdTRUE)
    {
      LOGW("Command queue full, dropping [%s]", command.text);
    }
  }
} bleCommandCallbacks;

class InfoCallbacks : public NimBLECharacteristicCallbacks
{
  // Runs before the phone reads the value, so it's always current
  void onRead(NimBLECharacteristic *characteristic, NimBLEConnInfo &connInfo) override
  {
    characteristic->setValue(infoJson(connInfo.getMTU(), connInfo.getConnInterval(), connInfo.getConnTimeout()).c_str());
  }
} bleInfoCallbacks;

class SettingsCallbacks : public NimBLECharacteristicCallbacks
{
  void onRead(NimBLECharacteristic *characteristic, NimBLEConnInfo &connInfo) override
  {
    characteristic->setValue(settingsJson().c_str());
  }
} bleSettingsCallbacks;

String infoJson(uint16_t mtu, uint16_t interval, uint16_t timeout)
{
  char json[512];
  snprintf(json, sizeof(json),
           "{\"name\":\"%s\",\"mac\":\"%s\",\"firmware\":\"%s %s\",\"uptime\":%lu,"
           "\"chip\":\"%s rev %d, %d cores, %u MHz\",\"heapSize\":%u,\"heap\":%u,\"minHeap\":%u,"
           "\"mtu\":%u,\"interval\":%.2f,\"timeout\":%u,\"bonds\":%d}",
           deviceName.c_str(), NimBLEDevice::getAddress().toString().c_str(), __DATE__, __TIME__,
           millis() / 1000, ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores(),
           ESP.getCpuFreqMHz(), ESP.getHeapSize(), ESP.getFreeHeap(), ESP.getMinFreeHeap(), mtu, interval * 1.25,
           timeout * 10, NimBLEDevice::getNumBonds());
  return String(json);
}

void bleSetup()
{
  bleCommandQueue = xQueueCreate(16, sizeof(BleCommand));

  NimBLEDevice::init(deviceName.c_str());
  NimBLEDevice::setPower(9); // dBm, the ESP32's maximum, for the best range
  NimBLEDevice::setMTU(185); // the info JSON then needs only 2-3 packets
  // Bonding, MITM protection (passkey), LE Secure Connections
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY); // "display" = the passkey in the README
  NimBLEDevice::setSecurityPasskey(blePasskey);

  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(&bleServerCallbacks);
  bleServer->advertiseOnDisconnect(true);

  NimBLEService *service = bleServer->createService(BLE_SERVICE_UUID);
  NimBLECharacteristic *cmd = service->createCharacteristic(
      BLE_CMD_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC |
                        NIMBLE_PROPERTY::WRITE_AUTHEN);
  cmd->setCallbacks(&bleCommandCallbacks);
  bleStatusChar = service->createCharacteristic(
      BLE_STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_ENC |
                           NIMBLE_PROPERTY::READ_AUTHEN);
  bleStatusChar->setValue(statusJson(0).c_str());
  NimBLECharacteristic *info = service->createCharacteristic(
      BLE_INFO_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN);
  info->setCallbacks(&bleInfoCallbacks);
  NimBLECharacteristic *settings = service->createCharacteristic(
      BLE_SETTINGS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN);
  settings->setCallbacks(&bleSettingsCallbacks);

  // The 128-bit service UUID fills most of the 31-byte advertisement, so the name goes into
  // the scan response. enableScanResponse() must come before setName().
  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->enableScanResponse(true);
  advertising->addServiceUUID(BLE_SERVICE_UUID);
  advertising->setName(deviceName.c_str());
  advertising->start();
  LOGI("Bluetooth started as \"%s\" (%s), waiting for the phone", deviceName.c_str(),
       NimBLEDevice::getAddress().toString().c_str());
}

String statusJson(int rssi)
{
  char json[96];
  snprintf(json, sizeof(json), "{\"rssi\":%d,\"light\":%d,\"bucket\":%d,\"aux\":%d}", rssi, light,
           (int)(getServoTarget(BUCKET_SERVO) + 0.5), (int)(getServoTarget(AUX_SERVO) + 0.5));
  return String(json);
}

void bleLoop()
{
  if (bleLinkLost)
  {
    bleLinkLost = false;
    stopEverything("phone disconnected");
  }

  BleCommand command;
  while (xQueueReceive(bleCommandQueue, &command, 0) == pdTRUE)
  {
    noteCommandReceived();
    handleCarInput(std::string(command.text));
  }

  static unsigned long lastStatus = 0;
  uint16_t conn = bleConnHandle;
  if (conn != BLE_HS_CONN_HANDLE_NONE && millis() - lastStatus >= statusIntervalMs)
  {
    lastStatus = millis();
    int8_t rssi = 0;
    if (ble_gap_conn_rssi(conn, &rssi) != 0)
    {
      rssi = 0; // 0 = unknown
    }
    bleStatusChar->setValue(statusJson(rssi).c_str());
    bleStatusChar->notify();
  }
}
