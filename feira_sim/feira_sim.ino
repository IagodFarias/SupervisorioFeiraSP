#ifdef ESP8266
  #include <ESP8266WiFi.h>
  #include <ESP8266WebServer.h>
  ESP8266WebServer server(80);
#else
  #include <WiFi.h>
  #include <WebServer.h>
  #include <esp_system.h>   // esp_reset_reason()
  WebServer server(80);
#endif

// ==============================
// Configuração da rede Wi-Fi
// ==============================
const char* ssid = "A15";
const char* password = "123456789";

// ==============================
// Mapeamento de pinos da ESP32
// ==============================
// A chave geral (IA) tem relé próprio + 4 relés de válvula. A numeração das
// coils é a mesma das válvulas do site: válvula n <-> coil Vn.
//
//   GPIO18 -> IA = chave desabilitadora / linha comum        (liga em HIGH)
//   GPIO21 -> V1 = medidor volumétrico    -> válvula 1 do bench.html (NF: abre em LOW)
//   GPIO19 -> V2 = válvula de corte       -> válvula 2       (NA: abre em HIGH)
//   GPIO23 -> V3 = FCX DN25 x 260         -> válvula 3       (NA: abre em HIGH)
//   GPIO22 -> V4 = unijato / multijato    -> válvula 4       (NF: abre em LOW)
//
// Todas as 4 válvulas eram NA (normalmente aberta). V1 e V4 foram trocadas
// por válvulas NF (normalmente fechada); V2 e V3 continuam NA. Isso inverte
// o nível de pino que abre/fecha V1 e V4 — ver RELAY_OPENS_ON_LOW abaixo.


const int IA_PIN = 18;   // chave desabilitadora / linha comum
const int V1_PIN = 21;   // volumétrico
const int V2_PIN = 19;   // válvula de corte
const int V3_PIN = 23;   // FCX DN25 x 260
const int V4_PIN = 22;   // unijato, multijato

const bool RELAY_ACTIVE_LOW = false;

void relayWrite(int pin, bool on) {
  digitalWrite(pin, RELAY_ACTIVE_LOW ? !on : on);
}

// Relés entram um de cada vez: 4 bobinas puxando corrente juntas derrubam a
// alimentação e a ESP32 reinicia (brownout). Desligar é sempre imediato.
const unsigned long RELAY_STAGGER_MS = 200;
const int RELAY_COUNT = 5;
const int RELAY_PINS[RELAY_COUNT] = { IA_PIN, V1_PIN, V2_PIN, V3_PIN, V4_PIN };
bool relayOut[RELAY_COUNT] = { false, false, false, false, false };
unsigned long lastRelayOnMs = 0;

// V1 e V4 são NF (abrem com o pino em LOW, fecham em HIGH); IA, V2 e V3
// continuam NA (ligam/abrem em HIGH). Mesma ordem de RELAY_PINS
// (IA, V1, V2, V3, V4). O front-end (scada_painel_fenasan.html / bench.html)
// não sabe disso — ele só manda "válvula aberta: sim/não" por /coil, e é
// aqui que isso vira o nível de pino correto para cada válvula.
const bool RELAY_OPENS_ON_LOW[RELAY_COUNT] = { false, true, false, false, true };

const char* resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "energizou (power-on)";
    case ESP_RST_EXT:      return "pino EN";
    case ESP_RST_SW:       return "reset por software";
    case ESP_RST_PANIC:    return "PANIC (crash do firmware)";
    case ESP_RST_INT_WDT:  return "watchdog de interrupção";
    case ESP_RST_TASK_WDT: return "watchdog de tarefa";
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "BROWNOUT (queda de tensão na alimentação)";
    default:               return "outro";
  }
}

// ==============================
// Estado das "coils"
// ==============================
bool coilIA = false;
bool coilV1 = false;
bool coilV2 = false;
bool coilV3 = false;
bool coilV4 = false;

// Watchdog do supervisório: o site consulta /state a cada 1s. Se ninguém
// falar com a placa por esse tempo (aba fechada, PC travado, "Desligar" no
// site), a chave geral cai e todas as válvulas fecham.
const unsigned long SUPERVISOR_TIMEOUT_MS = 5000;
unsigned long lastSupervisorMs = 0;

bool* coilByName(const String& name) {
  if (name == "IA") return &coilIA;
  if (name == "V1") return &coilV1;
  if (name == "V2") return &coilV2;
  if (name == "V3") return &coilV3;
  if (name == "V4") return &coilV4;
  return nullptr;
}

struct ValveMap { const char* name; bool* coil; };
ValveMap VALVE_TO_COIL[] = {
  { "volumetrico",        &coilV1 },
  { "fcx_sensor_pressao", &coilV2 },
  { "fcx_valvula_corte",  &coilV3 },
  { "unijato_multijato",  &coilV4 },
};

const int VALVE_COUNT = sizeof(VALVE_TO_COIL) / sizeof(VALVE_TO_COIL[0]);

// ==============================
// Helpers para ler JSON simples (corpo sempre no formato
// {"chave":"...","value":true/false}, um único nível)
// ==============================
String jsonStringField(const String& body, const String& key) {
  String pat = "\"" + key + "\"";
  int k = body.indexOf(pat);
  if (k < 0) return "";
  int colon = body.indexOf(':', k + pat.length());
  if (colon < 0) return "";
  int q1 = body.indexOf('"', colon);
  if (q1 < 0) return "";
  int q2 = body.indexOf('"', q1 + 1);
  if (q2 < 0) return "";
  return body.substring(q1 + 1, q2);
}

bool jsonBoolField(const String& body, const String& key, bool fallback) {
  String pat = "\"" + key + "\"";
  int k = body.indexOf(pat);
  if (k < 0) return fallback;
  int colon = body.indexOf(':', k + pat.length());
  if (colon < 0) return fallback;
  int t = body.indexOf("true", colon);
  int f = body.indexOf("false", colon);
  if (t >= 0 && (f < 0 || t < f)) return true;
  if (f >= 0) return false;
  return fallback;
}

// ==============================
// CORS bench.html
// ==============================
void sendCors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void handleOptions() {
  sendCors();
  server.send(204);
}

// Com IA desligada o loop() zeraria a válvula na volta seguinte; em vez de
// responder ok e descartar em silêncio, recusa o comando.
bool rejectIfInterlocked(bool* coil) {
  if (coil == &coilIA || coilIA) return false;
  Serial.println("  recusado: IA desligada");
  server.send(409, "application/json", "{\"ok\":false,\"error\":\"IA desligada\"}");
  return true;
}

// GET /state -> { "connected": true, "coils": { "IA":bool, "V1":bool, ... } }
void handleState() {
  lastSupervisorMs = millis();
  sendCors();
  String json = "{\"connected\":true,\"coils\":{";
  json += "\"IA\":" + String(coilIA ? "true" : "false") + ",";
  json += "\"V1\":" + String(coilV1 ? "true" : "false") + ",";
  json += "\"V2\":" + String(coilV2 ? "true" : "false") + ",";
  json += "\"V3\":" + String(coilV3 ? "true" : "false") + ",";
  json += "\"V4\":" + String(coilV4 ? "true" : "false");
  json += "}}";
  server.send(200, "application/json", json);
}

// POST /coil  body: {"coil":"IA","value":true}
void handleCoil() {
  lastSupervisorMs = millis();
  sendCors();
  String body = server.arg("plain");
  Serial.print("[/coil] recebido: ");
  Serial.println(body);
  String coilName = jsonStringField(body, "coil");
  bool* coil = coilByName(coilName);
  if (!coil) {
    Serial.println("[/coil] coil desconhecida: '" + coilName + "'");
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"unknown coil\"}");
    return;
  }
  if (rejectIfInterlocked(coil)) return;
  *coil = jsonBoolField(body, "value", *coil);
  Serial.println("[/coil] " + coilName + " agora = " + String(*coil ? "ON" : "OFF"));
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /valve  body: {"valve":"unijato_multijato","value":true}
void handleValve() {
  lastSupervisorMs = millis();
  sendCors();
  String body = server.arg("plain");
  Serial.print("[/valve] recebido: ");
  Serial.println(body);
  String valveName = jsonStringField(body, "valve");
  bool* coil = nullptr;
  for (int i = 0; i < VALVE_COUNT; i++) {
    if (valveName == VALVE_TO_COIL[i].name) {
      coil = VALVE_TO_COIL[i].coil;
      break;
    }
  }
  if (!coil) {
    Serial.println("[/valve] sem relé mapeado para '" + valveName + "'");
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"no relay mapped for valve\"}");
    return;
  }
  if (rejectIfInterlocked(coil)) return;
  *coil = jsonBoolField(body, "value", *coil);
  Serial.println("[/valve] " + valveName + " agora = " + String(*coil ? "ON" : "OFF"));
  server.send(200, "application/json", "{\"ok\":true}");
}

// ==============================
// Wi-Fi: conexão e diagnóstico
// ==============================
// Reconexão: sem isso, uma queda de rede (comum em hotspot de celular) deixa
// a ESP32 "muda" para sempre, só voltando com reset físico.
bool wifiWasConnected = false;
unsigned long lastReconnectAttempt = 0;
unsigned long lastIpPrint = 0;
const unsigned long RECONNECT_INTERVAL_MS = 5000;
const unsigned long CONNECT_TIMEOUT_MS = 20000;
const unsigned long IP_REPRINT_INTERVAL_MS = 15000;

const char* wifiStatusName(int st) {
  switch (st) {
    case WL_IDLE_STATUS:     return "ocioso";
    case WL_NO_SSID_AVAIL:   return "rede não encontrada (SSID errado, fora de alcance, ou hotspot em 5GHz)";
    case WL_CONNECT_FAILED:  return "falha ao autenticar (senha errada?)";
    case WL_CONNECTION_LOST: return "conexão perdida";
    case WL_DISCONNECTED:    return "desconectado";
    case WL_CONNECTED:       return "conectado";
    default:                 return "desconhecido";
  }
}

// É este bloco que você copia para o campo de endereço no site.
void printNetworkInfo() {
  Serial.println();
  Serial.println("==================================================");
  Serial.print  ("  IP da ESP32: ");
  Serial.println(WiFi.localIP());
  Serial.print  ("  Cole no site: http://");
  Serial.println(WiFi.localIP());
  Serial.print  ("  Rede: ");
  Serial.print  (WiFi.SSID());
  Serial.print  ("   sinal: ");
  Serial.print  (WiFi.RSSI());
  Serial.println(" dBm");
  Serial.println("==================================================");
  Serial.println();
}

// Só roda quando a conexão falha: mostra o que a placa realmente enxerga.
void listVisibleNetworks() {
  Serial.println("Redes 2.4GHz que a placa está enxergando:");
  int n = WiFi.scanNetworks();
  if (n <= 0) {
    Serial.println("  (nenhuma)");
  } else {
    for (int i = 0; i < n; i++) {
      Serial.print("  - ");
      Serial.print(WiFi.SSID(i));
      Serial.print("  (");
      Serial.print(WiFi.RSSI(i));
      Serial.println(" dBm)");
    }
  }
  WiFi.scanDelete();
  Serial.print("Se \"");
  Serial.print(ssid);
  Serial.println("\" não está na lista: a rede está fora de alcance ou em 5GHz — a ESP32 só fala 2.4GHz.");
}

bool connectWifi() {
  Serial.print("Conectando em \"");
  Serial.print(ssid);
  Serial.println("\"...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < CONNECT_TIMEOUT_MS) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.print("Não conectou — status: ");
    Serial.println(wifiStatusName(WiFi.status()));
    listVisibleNetworks();
    return false;
  }

  // Desliga o power-save do rádio: com ele ligado (padrão), a ESP32 "cochila"
  // entre requisições e perde/atrasa pacotes — como o site consulta /state a
  // cada 1s, isso aparece como "funcionou uma vez e depois passou a falhar".
  WiFi.setSleep(false);
  Serial.println("Wi-Fi conectado!");
  printNetworkInfo();
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(500);  // dá tempo do monitor serial aparecer antes do primeiro print
  Serial.println();
  Serial.println();
  Serial.println("=== feira_sim — bancada de válvulas ===");
  Serial.print("Motivo do último reset: ");
  Serial.println(resetReasonName(esp_reset_reason()));

  // Primeiro o nível de repouso, depois a direção: assim o pino sai do reset
  // já no estado "desligado" (= válvula fechada), em vez de dar um tranco no
  // relé. Para V1/V4 (NF, abrem em LOW) isso é nível HIGH, não LOW — por
  // isso usa RELAY_OPENS_ON_LOW em vez de "false" fixo; e relayOut precisa
  // começar sincronizado com o que já foi escrito no pino, senão a primeira
  // volta do loop() vê a inconsistência e dá um pulso indevido no relé.
  for (int i = 0; i < RELAY_COUNT; i++) {
    bool closedPinHigh = RELAY_OPENS_ON_LOW[i];
    relayWrite(RELAY_PINS[i], closedPinHigh);
    pinMode(RELAY_PINS[i], OUTPUT);
    relayWrite(RELAY_PINS[i], closedPinHigh);
    relayOut[i] = closedPinHigh;
  }

  // Não trava aqui: se o Wi-Fi não subir, o servidor sobe do mesmo jeito e o
  // loop() segue tentando reconectar, dizendo no serial o que está pegando —
  // em vez de ficar preso imprimindo pontinhos para sempre.
  connectWifi();

  // Rotas HTTP consumidas por bench.html / bench2.html
  server.on("/state", HTTP_GET, handleState);
  server.on("/state", HTTP_OPTIONS, handleOptions);
  server.on("/coil", HTTP_POST, handleCoil);
  server.on("/coil", HTTP_OPTIONS, handleOptions);
  server.on("/valve", HTTP_POST, handleValve);
  server.on("/valve", HTTP_OPTIONS, handleOptions);
  server.begin();
  Serial.println("Servidor HTTP iniciado na porta 80.");

  wifiWasConnected = (WiFi.status() == WL_CONNECTED);
  lastIpPrint = millis();
}

void loop() {
  bool wifiConnected = (WiFi.status() == WL_CONNECTED);

  if (wifiConnected) {
    if (!wifiWasConnected) {
      WiFi.setSleep(false);
      Serial.println("Wi-Fi reconectado!");
      printNetworkInfo();
      lastIpPrint = millis();
    }
    server.handleClient();
    wifiWasConnected = true;

    // Repete o IP de tempos em tempos: se você abrir o monitor serial depois
    // do boot, a linha original já rolou para fora da tela.
    unsigned long now = millis();
    if (now - lastIpPrint >= IP_REPRINT_INTERVAL_MS) {
      lastIpPrint = now;
      Serial.print("[status] IP da ESP32: ");
      Serial.print(WiFi.localIP());
      Serial.print("   sinal: ");
      Serial.print(WiFi.RSSI());
      Serial.println(" dBm");
    }
  } else {
    if (wifiWasConnected) {
      // acabou de cair: por segurança, desliga tudo, já que ninguém consegue
      // supervisionar as válvulas sem rede
      Serial.println("Wi-Fi caiu — desligando válvulas e tentando reconectar...");
      coilIA = false;
      coilV1 = false;
      coilV2 = false;
      coilV3 = false;
      coilV4 = false;
      wifiWasConnected = false;
    }
    unsigned long now = millis();
    if (now - lastReconnectAttempt >= RECONNECT_INTERVAL_MS) {
      lastReconnectAttempt = now;
      Serial.print("Sem Wi-Fi (");
      Serial.print(wifiStatusName(WiFi.status()));
      Serial.println(") — tentando reconectar...");
      WiFi.disconnect();
      WiFi.begin(ssid, password);
    }
  }

  // Supervisório sumiu: derruba a chave geral (o bloco abaixo fecha o resto).
  if (coilIA && millis() - lastSupervisorMs >= SUPERVISOR_TIMEOUT_MS) {
    Serial.println("Supervisório sem contato há 5s — desligando IA e válvulas.");
    coilIA = false;
  }

  // IA precisa estar ativa para permitir qualquer válvula. Se estiver OFF,
  // zera as coils para /state refletir o estado real dos relés — por isso,
  // ao ligar a IA, todas as válvulas partem fechadas e o operador abre uma a
  // uma pelo site.
  if (!coilIA) {
    coilV1 = false;
    coilV2 = false;
    coilV3 = false;
    coilV4 = false;
  }

  
  // Estado lógico desejado (chave geral ligada / válvula aberta), antes de
  // converter para nível de pino.
  const bool wantActive[RELAY_COUNT] = {
    coilIA, coilIA && coilV1, coilIA && coilV2, coilIA && coilV3, coilIA && coilV4
  };

  // V1 e V4 abrem com o pino em LOW, então fechar essas duas é que energiza a
  // bobina. O escalonamento abaixo precisa olhar para o pino indo a HIGH (é
  // isso que puxa corrente), não para "válvula aberta" — senão V1/V4
  // fechando (que agora energiza) ficaria fora do escalonamento.
  bool pinHighWanted[RELAY_COUNT];
  for (int i = 0; i < RELAY_COUNT; i++) {
    pinHighWanted[i] = RELAY_OPENS_ON_LOW[i] ? !wantActive[i] : wantActive[i];
  }

  // Cada pino é escrito uma única vez por volta, já intertravado pela chave
  // geral — escrever o mesmo pino duas vezes gerava pulso a cada loop.
  // Desligar vale na hora; ligar, no máximo um relé a cada RELAY_STAGGER_MS
  // (a IA vem primeiro na lista, então entra antes das válvulas).
  unsigned long now = millis();
  bool turnedOnThisLoop = false;
  for (int i = 0; i < RELAY_COUNT; i++) {
    if (!pinHighWanted[i]) {
      relayOut[i] = false;
    } else if (!relayOut[i] && !turnedOnThisLoop && now - lastRelayOnMs >= RELAY_STAGGER_MS) {
      relayOut[i] = true;
      lastRelayOnMs = now;
      turnedOnThisLoop = true;
    }
    relayWrite(RELAY_PINS[i], relayOut[i]);
  }
}
