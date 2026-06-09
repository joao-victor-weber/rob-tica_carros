/*
  Carrinho ESP32 — versão corrigida e comentada

  O que esta versão faz:
  1. Cria um Wi-Fi próprio no ESP32.
  2. Abre uma página de controle em http://192.168.4.1
  3. Controla dois motores DC usando um driver L298N ou semelhante.
  4. Tem controle manual por joystick na página web.
  5. Tem modo "seguir Wi-Fi" usando RSSI do celular conectado.
  6. Usa somente três níveis de velocidade nos motores: 0, 127 e 255.

  Importante sobre o modo seguir:
  O ESP32 com uma antena só NÃO sabe a direção real do celular.
  Ele mede força de sinal, testa esquerda/direita e escolhe o lado onde o RSSI melhora.

  Ligações sugeridas com L298N:

  Motor esquerdo:
    ENA -> GPIO25
    IN1 -> GPIO18
    IN2 -> GPIO19

  Motor direito:
    ENB -> GPIO26
    IN3 -> GPIO21
    IN4 -> GPIO22

  Alimentação:
    GND do ESP32 ligado ao GND do driver/fonte dos motores.
    Motores alimentados por fonte/bateria separada.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <esp_wifi.h>

// =====================================================
// WI-FI DO CARRINHO
// =====================================================
// O ESP32 vai criar essa rede Wi-Fi.
// Conecte o celular nessa rede e abra: http://192.168.4.1

const char* ssid = "ESP32_Car";
const char* password = "12345678";  // mínimo 8 caracteres

WebServer server(80);

// =====================================================
// PINOS DOS MOTORES
// =====================================================
// Troquei os pinos 12 e 15 porque eles podem atrapalhar o boot do ESP32.
// Estes pinos abaixo são mais seguros para saída digital/PWM em placas ESP32 comuns.

#define MOTOR_E_PWM     25
#define MOTOR_E_FRENTE  18
#define MOTOR_E_RE      19

#define MOTOR_D_PWM     26
#define MOTOR_D_FRENTE  21
#define MOTOR_D_RE      22

// =====================================================
// PWM E VELOCIDADES
// =====================================================
// A API ledcAttach usa o próprio pino como referência no Arduino-ESP32 3.x.
// Com resolução de 8 bits, o duty vai de 0 até 255.

#define PWM_FREQ        5000
#define PWM_RESOLUTION  8

#define VEL_PARADO      0
#define VEL_MEDIA       127
#define VEL_MAXIMA      255

// velocidadeManual limita o controle manual.
// Ela só pode assumir 0, 127 ou 255.
int velocidadeManual = VEL_MEDIA;

// =====================================================
// PARÂMETROS DO SEGUIDOR WI-FI
// =====================================================
// RSSI é negativo.
// Quanto MENOS negativo, melhor o sinal.
// Exemplo: -40 é melhor/mais perto que -75.

#define RSSI_SEM_CLIENTE -100

// Valores padrão. A página web consegue ajustar estes limites em tempo real.
#define RSSI_ALVO_PADRAO        -45
#define RSSI_LONGE_PADRAO       -62
#define RSSI_MUITO_PERTO_PADRAO -30

int rssiAlvoDin = RSSI_ALVO_PADRAO;
int rssiLongeDin = RSSI_LONGE_PADRAO;
int rssiMuitoPertoDin = RSSI_MUITO_PERTO_PADRAO;

// Zona morta evita que o carrinho fique tremendo por diferença pequena de RSSI.
#define ZONA_MORTA_RSSI 2

// Suavização do RSSI.
// Maior = mais estável e lento.
// Menor = mais rápido e mais nervoso.
#define ALPHA_RSSI 0.55f

// Busca/correção de direção.
#define INTERVALO_CURVA_MS      700
#define DURACAO_TESTE_MS        180
#define DURACAO_CORRECAO_MS     350
#define TIMEOUT_BUSCA_MS        1800
#define MARGEM_MELHORA_RSSI     2

// =====================================================
// ESTADO GLOBAL DO CONTROLE
// =====================================================

bool modoSeguir = false;

volatile int joyX = 0;
volatile int joyY = 0;

int rssiSuavizado = RSSI_SEM_CLIENTE;
int rssiAnterior = RSSI_SEM_CLIENTE;
bool rssiInicializado = false;

int direcaoBusca = 1;
int melhorRSSIBusca = RSSI_SEM_CLIENTE;
unsigned long inicioBusca = 0;

int faseCorrecao = 0;
int rssiTesteEsq = RSSI_SEM_CLIENTE;
int rssiTesteDir = RSSI_SEM_CLIENTE;
bool emCorrecao = false;
unsigned long tempoUltimaCorrecao = 0;
unsigned long tempoInicioFase = 0;

String ultimaAcao = "Inativo";

// =====================================================
// HTML DA PÁGINA WEB
// =====================================================
// Mantive a interface em uma String para simplificar.
// Ela envia comandos para as rotas:
//   /analog?x=...&y=...
//   /velocidade?v=0|127|255
//   /seguir?ativo=1|0
//   /rssi?alvo=...&longe=...&perto=...
//   /status

const char pagina[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no">
<title>Carrinho ESP32</title>
<style>
  * { box-sizing: border-box; }
  body {
    margin: 0;
    padding: 14px;
    font-family: Arial, sans-serif;
    text-align: center;
    background: #0b1020;
    color: #e8eefc;
    touch-action: none;
  }
  h2 { margin: 8px 0 14px; color: #64d2ff; }
  .card {
    max-width: 420px;
    margin: 10px auto;
    padding: 14px;
    background: #121a2e;
    border: 1px solid #263552;
    border-radius: 14px;
  }
  canvas {
    background: #0f1729;
    border: 2px solid #334766;
    border-radius: 50%;
    touch-action: none;
  }
  button {
    padding: 12px 16px;
    margin: 5px;
    border: 0;
    border-radius: 10px;
    font-weight: bold;
    color: white;
    background: #2563eb;
  }
  button.stop { background: #dc2626; }
  button.on { background: #16a34a; }
  button.off { background: #475569; }
  .row {
    display: flex;
    gap: 8px;
    justify-content: center;
    flex-wrap: wrap;
    align-items: center;
  }
  .info {
    text-align: left;
    line-height: 1.7;
    font-size: 15px;
  }
  label { display: block; margin-top: 10px; font-size: 13px; color: #a9bbd8; }
  input[type=range] { width: 100%; }
  .mini { color: #a9bbd8; font-size: 12px; }
</style>
</head>
<body>

<h2>Carrinho ESP32</h2>

<div class="card">
  <h3>Joystick</h3>
  <canvas id="joy" width="240" height="240"></canvas>
  <p class="mini">Arraste para controlar. Solte para parar.</p>
  <div class="row">
    <button class="stop" onclick="pararTudo()">PARAR</button>
  </div>
</div>

<div class="card">
  <h3>Velocidade manual</h3>
  <div class="row">
    <button onclick="setVel(0)">0</button>
    <button onclick="setVel(127)">127</button>
    <button onclick="setVel(255)">255</button>
  </div>
  <p>Velocidade atual: <b id="velAtual">127</b></p>
</div>

<div class="card">
  <h3>Modo seguir Wi-Fi</h3>
  <div class="row">
    <button id="btnSeguir" class="off" onclick="toggleSeguir()">Seguir: OFF</button>
  </div>
  <p class="mini">Conecte apenas um celular na rede do carrinho para evitar confusão no RSSI.</p>
</div>

<div class="card">
  <h3>Ajustes de RSSI</h3>

  <label>RSSI alvo: <b id="vAlvo">-45</b></label>
  <input id="rAlvo" type="range" min="-80" max="-20" value="-45" oninput="enviaRSSI()">

  <label>RSSI longe: <b id="vLonge">-62</b></label>
  <input id="rLonge" type="range" min="-95" max="-45" value="-62" oninput="enviaRSSI()">

  <label>RSSI muito perto: <b id="vPerto">-30</b></label>
  <input id="rPerto" type="range" min="-55" max="-15" value="-30" oninput="enviaRSSI()">
</div>

<div class="card info">
  <h3>Status</h3>
  <div>Clientes conectados: <b id="clientes">0</b></div>
  <div>RSSI bruto: <b id="rssiRaw">--</b> dBm</div>
  <div>RSSI suavizado: <b id="rssiSuave">--</b> dBm</div>
  <div>Ação: <b id="acao">--</b></div>
</div>

<script>
const canvas = document.getElementById('joy');
const ctx = canvas.getContext('2d');
const cx = canvas.width / 2;
const cy = canvas.height / 2;
const raio = 90;
let kx = cx;
let ky = cy;
let arrastando = false;
let modoSeguir = false;
let ultimoX = 0;
let ultimoY = 0;

function desenhar() {
  ctx.clearRect(0, 0, canvas.width, canvas.height);

  ctx.beginPath();
  ctx.arc(cx, cy, raio, 0, Math.PI * 2);
  ctx.strokeStyle = '#536b92';
  ctx.lineWidth = 3;
  ctx.stroke();

  ctx.beginPath();
  ctx.moveTo(cx - raio, cy);
  ctx.lineTo(cx + raio, cy);
  ctx.moveTo(cx, cy - raio);
  ctx.lineTo(cx, cy + raio);
  ctx.strokeStyle = '#263552';
  ctx.lineWidth = 2;
  ctx.stroke();

  ctx.beginPath();
  ctx.arc(kx, ky, 25, 0, Math.PI * 2);
  ctx.fillStyle = '#38bdf8';
  ctx.fill();
  ctx.strokeStyle = '#e8eefc';
  ctx.stroke();
}

function posicao(e) {
  const rect = canvas.getBoundingClientRect();
  const p = e.touches ? e.touches[0] : e;
  return { x: p.clientX - rect.left, y: p.clientY - rect.top };
}

function mover(p) {
  let dx = p.x - cx;
  let dy = p.y - cy;
  const dist = Math.sqrt(dx * dx + dy * dy);

  if (dist > raio) {
    dx = dx * raio / dist;
    dy = dy * raio / dist;
  }

  kx = cx + dx;
  ky = cy + dy;
  desenhar();

  const x = Math.round(dx / raio * 100);
  const y = Math.round(-dy / raio * 100);

  ultimoX = x;
  ultimoY = y;

  if (modoSeguir) desligarSeguir();
  enviarAnalogico(x, y);
}

function enviarAnalogico(x, y) {
  fetch('/analog?x=' + x + '&y=' + y).catch(() => {});
}

function pararJoystick() {
  kx = cx;
  ky = cy;
  ultimoX = 0;
  ultimoY = 0;
  desenhar();
  enviarAnalogico(0, 0);
}

function pararTudo() {
  modoSeguir = false;
  atualizarBotaoSeguir();
  fetch('/parar').catch(() => {});
  pararJoystick();
}

function setVel(v) {
  document.getElementById('velAtual').textContent = v;
  fetch('/velocidade?v=' + v).catch(() => {});
}

function toggleSeguir() {
  modoSeguir = !modoSeguir;
  atualizarBotaoSeguir();
  fetch('/seguir?ativo=' + (modoSeguir ? 1 : 0)).catch(() => {});
}

function desligarSeguir() {
  modoSeguir = false;
  atualizarBotaoSeguir();
  fetch('/seguir?ativo=0').catch(() => {});
}

function atualizarBotaoSeguir() {
  const b = document.getElementById('btnSeguir');
  b.textContent = 'Seguir: ' + (modoSeguir ? 'ON' : 'OFF');
  b.className = modoSeguir ? 'on' : 'off';
}

function enviaRSSI() {
  const alvo = document.getElementById('rAlvo').value;
  const longe = document.getElementById('rLonge').value;
  const perto = document.getElementById('rPerto').value;

  document.getElementById('vAlvo').textContent = alvo;
  document.getElementById('vLonge').textContent = longe;
  document.getElementById('vPerto').textContent = perto;

  fetch('/rssi?alvo=' + alvo + '&longe=' + longe + '&perto=' + perto).catch(() => {});
}

function atualizarStatus() {
  fetch('/status')
    .then(r => r.json())
    .then(d => {
      document.getElementById('clientes').textContent = d.clientes;
      document.getElementById('rssiRaw').textContent = d.rssiRaw;
      document.getElementById('rssiSuave').textContent = d.rssiSuavizado;
      document.getElementById('acao').textContent = d.acao;
      document.getElementById('velAtual').textContent = d.velocidade;
      modoSeguir = d.seguir;
      atualizarBotaoSeguir();
    })
    .catch(() => {});
}

canvas.addEventListener('mousedown', e => {
  arrastando = true;
  mover(posicao(e));
});

canvas.addEventListener('mousemove', e => {
  if (!arrastando) return;
  mover(posicao(e));
});

canvas.addEventListener('mouseup', () => {
  arrastando = false;
  pararJoystick();
});

canvas.addEventListener('mouseleave', () => {
  if (!arrastando) return;
  arrastando = false;
  pararJoystick();
});

canvas.addEventListener('touchstart', e => {
  e.preventDefault();
  arrastando = true;
  mover(posicao(e));
}, { passive: false });

canvas.addEventListener('touchmove', e => {
  e.preventDefault();
  if (!arrastando) return;
  mover(posicao(e));
}, { passive: false });

canvas.addEventListener('touchend', e => {
  e.preventDefault();
  arrastando = false;
  pararJoystick();
}, { passive: false });

setInterval(atualizarStatus, 500);
desenhar();
atualizarStatus();
</script>
</body>
</html>
)rawliteral";

// =====================================================
// FUNÇÕES DE VELOCIDADE E MOTORES
// =====================================================

int normalizarVelocidade(int velocidade) {
  // Esta função força qualquer velocidade a virar somente 0, 127 ou 255.
  // Isso deixa o comportamento previsível e segue sua regra das três velocidades.
  velocidade = constrain(velocidade, 0, VEL_MAXIMA);

  if (velocidade == 0) return VEL_PARADO;
  if (velocidade <= VEL_MEDIA) return VEL_MEDIA;
  return VEL_MAXIMA;
}

int aplicarLimiteManual(int velocidade) {
  // Se velocidadeManual for 127, o carrinho nunca chega a 255 no controle manual.
  // Se for 255, ele pode usar 127 ou 255.
  velocidade = normalizarVelocidade(velocidade);

  if (velocidadeManual == VEL_PARADO) return VEL_PARADO;
  if (velocidadeManual == VEL_MEDIA && velocidade > VEL_MEDIA) return VEL_MEDIA;
  return velocidade;
}

void motorRaw(int pinoFrente, int pinoRe, int pinoPWM, int velocidadeAssinada) {
  // velocidadeAssinada:
  //   positiva = frente
  //   negativa = ré
  //   zero     = parado
  int velocidade = normalizarVelocidade(abs(velocidadeAssinada));

  if (velocidade == VEL_PARADO) {
    digitalWrite(pinoFrente, LOW);
    digitalWrite(pinoRe, LOW);
    ledcWrite(pinoPWM, VEL_PARADO);
    return;
  }

  if (velocidadeAssinada > 0) {
    digitalWrite(pinoFrente, HIGH);
    digitalWrite(pinoRe, LOW);
  } else {
    digitalWrite(pinoFrente, LOW);
    digitalWrite(pinoRe, HIGH);
  }

  ledcWrite(pinoPWM, velocidade);
}

void motorEsquerdo(int velocidadeAssinada) {
  motorRaw(MOTOR_E_FRENTE, MOTOR_E_RE, MOTOR_E_PWM, velocidadeAssinada);
}

void motorDireito(int velocidadeAssinada) {
  motorRaw(MOTOR_D_FRENTE, MOTOR_D_RE, MOTOR_D_PWM, velocidadeAssinada);
}

void moverMotores(int velEsq, int velDir) {
  motorEsquerdo(velEsq);
  motorDireito(velDir);
}

void parar() {
  moverMotores(VEL_PARADO, VEL_PARADO);
}

void frente(int velocidade) {
  velocidade = normalizarVelocidade(velocidade);
  moverMotores(velocidade, velocidade);
}

void re(int velocidade) {
  velocidade = normalizarVelocidade(velocidade);
  moverMotores(-velocidade, -velocidade);
}

void virarEsquerda(int velocidade) {
  velocidade = normalizarVelocidade(velocidade);
  moverMotores(-velocidade, velocidade);
}

void virarDireita(int velocidade) {
  velocidade = normalizarVelocidade(velocidade);
  moverMotores(velocidade, -velocidade);
}

void curvaSuave(int direcao) {
  // direcao < 0 = curva para esquerda
  // direcao > 0 = curva para direita
  // Como só usamos 0/127/255, a curva suave vira uma diferença simples entre as rodas.
  direcao = constrain(direcao, -1, 1);

  if (direcao < 0) {
    moverMotores(VEL_MEDIA, VEL_MAXIMA);
  } else if (direcao > 0) {
    moverMotores(VEL_MAXIMA, VEL_MEDIA);
  } else {
    frente(VEL_MEDIA);
  }
}

int comandoParaVelocidadeManual(int comando) {
  // comando vem do joystick entre -100 e 100.
  // Convertimos para -255, -127, 0, 127 ou 255.
  int modulo = abs(comando);

  if (modulo < 8) return 0;

  int velocidade;

  if (velocidadeManual == VEL_PARADO) {
    velocidade = VEL_PARADO;
  } else if (velocidadeManual == VEL_MEDIA) {
    velocidade = VEL_MEDIA;
  } else {
    velocidade = (modulo <= 60) ? VEL_MEDIA : VEL_MAXIMA;
  }

  if (comando < 0) velocidade = -velocidade;
  return velocidade;
}

void aplicarJoystick() {
  // Mistura diferencial:
  // y controla frente/ré.
  // x controla esquerda/direita.
  // Exemplo:
  //   y=100, x=0   -> duas rodas para frente
  //   y=100, x=50  -> curva enquanto anda
  //   y=0, x=100   -> gira no próprio eixo

  int y = constrain(joyY, -100, 100);
  int x = constrain(joyX, -100, 100);

  int comandoE = constrain(y + x, -100, 100);
  int comandoD = constrain(y - x, -100, 100);

  int velE = comandoParaVelocidadeManual(comandoE);
  int velD = comandoParaVelocidadeManual(comandoD);

  if (velE == 0 && velD == 0) {
    parar();
    ultimaAcao = "Manual parado";
    return;
  }

  moverMotores(velE, velD);
  ultimaAcao = "Controle manual";
}

// =====================================================
// RSSI DO CELULAR CONECTADO
// =====================================================

int obterRSSI() {
  // Se não tiver celular conectado no Wi-Fi do ESP32, não há RSSI para seguir.
  if (WiFi.softAPgetStationNum() == 0) {
    return RSSI_SEM_CLIENTE;
  }

  wifi_sta_list_t lista;
  memset(&lista, 0, sizeof(lista));

  if (esp_wifi_ap_get_sta_list(&lista) != ESP_OK || lista.num == 0) {
    return RSSI_SEM_CLIENTE;
  }

  // Se mais de um dispositivo conectar, pegamos o sinal mais forte.
  // Para testes, o ideal é conectar só um celular.
  int melhorRSSI = RSSI_SEM_CLIENTE;

  for (int i = 0; i < lista.num; i++) {
    if (lista.sta[i].rssi > melhorRSSI) {
      melhorRSSI = lista.sta[i].rssi;
    }
  }

  return melhorRSSI;
}

bool atualizarRSSI() {
  int rssiRaw = obterRSSI();

  if (rssiRaw == RSSI_SEM_CLIENTE) {
    return false;
  }

  if (!rssiInicializado) {
    rssiSuavizado = rssiRaw;
    rssiAnterior = rssiRaw;
    rssiInicializado = true;
    return true;
  }

  rssiAnterior = rssiSuavizado;
  rssiSuavizado = (int)(ALPHA_RSSI * rssiSuavizado + (1.0f - ALPHA_RSSI) * rssiRaw);

  Serial.printf("[RSSI] raw=%d | suave=%d | alvo=%d | longe=%d | perto=%d\n",
                rssiRaw, rssiSuavizado, rssiAlvoDin, rssiLongeDin, rssiMuitoPertoDin);

  return true;
}

void resetarSeguidor() {
  // Limpa a memória do seguidor.
  // Isso evita usar RSSI antigo quando o celular reconecta.
  rssiSuavizado = RSSI_SEM_CLIENTE;
  rssiAnterior = RSSI_SEM_CLIENTE;
  rssiInicializado = false;

  inicioBusca = 0;
  melhorRSSIBusca = RSSI_SEM_CLIENTE;
  direcaoBusca = 1;

  faseCorrecao = 0;
  emCorrecao = false;
  rssiTesteEsq = RSSI_SEM_CLIENTE;
  rssiTesteDir = RSSI_SEM_CLIENTE;
}

// =====================================================
// MODO SEGUIR WI-FI
// =====================================================

int velocidadePorErroRSSI(int erroAbsoluto) {
  // Converte o tamanho do erro em 0, 127 ou 255.
  // Erro pequeno: para.
  // Erro médio: velocidade média.
  // Erro grande: velocidade máxima.
  if (erroAbsoluto <= ZONA_MORTA_RSSI) return VEL_PARADO;
  if (erroAbsoluto <= 10) return VEL_MEDIA;
  return VEL_MAXIMA;
}

void buscarDirecao() {
  // Quando o sinal está muito fraco, o carrinho gira procurando RSSI melhor.
  // Se o RSSI melhorar, ele continua no mesmo sentido.
  // Se passar muito tempo sem melhorar, inverte o giro.

  if (inicioBusca == 0) {
    inicioBusca = millis();
    melhorRSSIBusca = rssiSuavizado;
    direcaoBusca = -direcaoBusca;
    Serial.println("[Busca] Iniciando busca de sinal");
  }

  int velocidadeBusca = (rssiSuavizado < rssiLongeDin - 10) ? VEL_MAXIMA : VEL_MEDIA;

  if (direcaoBusca > 0) {
    virarDireita(velocidadeBusca);
    ultimaAcao = "Buscando sinal: direita";
  } else {
    virarEsquerda(velocidadeBusca);
    ultimaAcao = "Buscando sinal: esquerda";
  }

  if (rssiSuavizado > melhorRSSIBusca) {
    melhorRSSIBusca = rssiSuavizado;
    inicioBusca = millis();
  }

  if (millis() - inicioBusca > TIMEOUT_BUSCA_MS) {
    direcaoBusca = -direcaoBusca;
    inicioBusca = millis();
    Serial.println("[Busca] Sem melhora. Invertendo sentido.");
  }

  if (rssiSuavizado > rssiLongeDin) {
    inicioBusca = 0;
    parar();
    ultimaAcao = "Sinal encontrado";
    Serial.println("[Busca] Sinal encontrado");
  }
}

void corrigirDirecao() {
  // Teste simples de direção:
  // 1. Vira um pouco para a esquerda e mede RSSI.
  // 2. Vira um pouco para a direita e mede RSSI.
  // 3. Escolhe o lado com RSSI melhor.

  unsigned long agora = millis();

  if (faseCorrecao == 0) {
    if (agora - tempoUltimaCorrecao >= INTERVALO_CURVA_MS) {
      faseCorrecao = 1;
      tempoInicioFase = agora;
      virarEsquerda(VEL_MEDIA);
      ultimaAcao = "Testando esquerda";
    }
    return;
  }

  if (faseCorrecao == 1) {
    if (agora - tempoInicioFase < DURACAO_TESTE_MS) {
      virarEsquerda(VEL_MEDIA);
      return;
    }

    rssiTesteEsq = obterRSSI();
    faseCorrecao = 2;
    tempoInicioFase = agora;
    virarDireita(VEL_MEDIA);
    ultimaAcao = "Testando direita";
    return;
  }

  if (faseCorrecao == 2) {
    if (agora - tempoInicioFase < DURACAO_TESTE_MS) {
      virarDireita(VEL_MEDIA);
      return;
    }

    rssiTesteDir = obterRSSI();
    faseCorrecao = 0;
    tempoUltimaCorrecao = agora;

    Serial.printf("[Curva] esquerda=%d | direita=%d | base=%d\n",
                  rssiTesteEsq, rssiTesteDir, rssiSuavizado);

    if (rssiTesteEsq > rssiTesteDir + MARGEM_MELHORA_RSSI) {
      curvaSuave(-1);
      emCorrecao = true;
      tempoInicioFase = agora;
      ultimaAcao = "Corrigindo esquerda";
      return;
    }

    if (rssiTesteDir > rssiTesteEsq + MARGEM_MELHORA_RSSI) {
      curvaSuave(1);
      emCorrecao = true;
      tempoInicioFase = agora;
      ultimaAcao = "Corrigindo direita";
      return;
    }

    ultimaAcao = "Direção estável";
  }
}

void controlarDistancia() {
  // erro > 0: RSSI atual está abaixo do alvo, então o celular está longe.
  // erro < 0: RSSI atual está acima do alvo, então o celular está perto demais.

  int erro = rssiAlvoDin - rssiSuavizado;
  int velocidade = velocidadePorErroRSSI(abs(erro));

  if (velocidade == VEL_PARADO) {
    parar();
    ultimaAcao = "Na distância alvo";
    return;
  }

  if (erro > 0) {
    frente(velocidade);
    ultimaAcao = (velocidade == VEL_MAXIMA) ? "Avançando rápido" : "Avançando médio";
  } else {
    re(velocidade);
    ultimaAcao = (velocidade == VEL_MAXIMA) ? "Recuando rápido" : "Recuando médio";
  }
}

void seguirDispositivo() {
  // Segurança: se o celular desconectar, para e limpa a memória do seguidor.
  if (WiFi.softAPgetStationNum() == 0) {
    parar();
    resetarSeguidor();
    ultimaAcao = "Aguardando celular";
    return;
  }

  if (!atualizarRSSI()) {
    parar();
    resetarSeguidor();
    ultimaAcao = "RSSI inválido";
    return;
  }

  // Muito perto: recua imediatamente.
  if (rssiSuavizado > rssiMuitoPertoDin) {
    re(VEL_MEDIA);
    inicioBusca = 0;
    faseCorrecao = 0;
    emCorrecao = false;
    ultimaAcao = "Muito perto: recuando";
    return;
  }

  // Muito longe: gira procurando sinal melhor.
  if (rssiSuavizado < rssiLongeDin) {
    buscarDirecao();
    faseCorrecao = 0;
    emCorrecao = false;
    return;
  }

  // Se estava buscando e achou sinal aceitável, sai da busca.
  inicioBusca = 0;

  // Se está no meio de uma correção, espera a correção acabar.
  if (emCorrecao) {
    if (millis() - tempoInicioFase < DURACAO_CORRECAO_MS) {
      return;
    }

    emCorrecao = false;
    parar();
  }

  controlarDistancia();

  // Só tenta corrigir direção quando está longe do alvo.
  // Se estiver perto demais e dando ré, não faz sentido testar esquerda/direita.
  if (!emCorrecao && rssiSuavizado < rssiAlvoDin) {
    corrigirDirecao();
  }
}

// =====================================================
// ROTAS DO SERVIDOR WEB
// =====================================================

void handleRoot() {
  server.send(200, "text/html", pagina);
}

void handleParar() {
  modoSeguir = false;
  joyX = 0;
  joyY = 0;
  parar();
  resetarSeguidor();
  ultimaAcao = "Parado";
  server.send(200, "text/plain", "ok");
}

void handleVelocidade() {
  if (server.hasArg("v")) {
    int v = server.arg("v").toInt();

    // Só aceita as três velocidades oficiais do projeto.
    if (v <= 0) velocidadeManual = VEL_PARADO;
    else if (v <= VEL_MEDIA) velocidadeManual = VEL_MEDIA;
    else velocidadeManual = VEL_MAXIMA;
  }

  server.send(200, "text/plain", String(velocidadeManual));
}

void handleAnalog() {
  // Qualquer comando manual desliga o modo seguir.
  modoSeguir = false;

  if (server.hasArg("x")) joyX = constrain(server.arg("x").toInt(), -100, 100);
  if (server.hasArg("y")) joyY = constrain(server.arg("y").toInt(), -100, 100);

  aplicarJoystick();
  server.send(200, "text/plain", "ok");
}

void handleSeguir() {
  if (server.hasArg("ativo")) {
    bool novoModo = server.arg("ativo") == "1";

    if (novoModo && !modoSeguir) {
      resetarSeguidor();
      ultimaAcao = "Seguidor iniciado";
    }

    if (!novoModo) {
      parar();
      resetarSeguidor();
      ultimaAcao = "Seguidor desligado";
    }

    modoSeguir = novoModo;
  }

  server.send(200, "text/plain", modoSeguir ? "on" : "off");
}

void handleRssi() {
  if (server.hasArg("alvo")) {
    rssiAlvoDin = constrain(server.arg("alvo").toInt(), -80, -20);
  }

  if (server.hasArg("longe")) {
    rssiLongeDin = constrain(server.arg("longe").toInt(), -95, -45);
  }

  if (server.hasArg("perto")) {
    rssiMuitoPertoDin = constrain(server.arg("perto").toInt(), -55, -15);
  }

  server.send(200, "text/plain", "ok");
}

void handleStatus() {
  int clientes = WiFi.softAPgetStationNum();
  int rssiRaw = obterRSSI();

  String json = "{";
  json += "\"clientes\":" + String(clientes) + ",";
  json += "\"rssiRaw\":" + String(rssiRaw) + ",";
  json += "\"rssiSuavizado\":" + String(rssiSuavizado) + ",";
  json += "\"velocidade\":" + String(velocidadeManual) + ",";
  json += "\"seguir\":";
  json += (modoSeguir ? "true" : "false");
  json += ",\"acao\":\"" + ultimaAcao + "\"";
  json += "}";

  server.send(200, "application/json", json);
}

// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(500);

  // Primeiro configura os pinos de direção.
  pinMode(MOTOR_E_FRENTE, OUTPUT);
  pinMode(MOTOR_E_RE, OUTPUT);
  pinMode(MOTOR_D_FRENTE, OUTPUT);
  pinMode(MOTOR_D_RE, OUTPUT);

  // Depois configura o PWM.
  // Só chamamos parar() depois disso, porque parar() usa ledcWrite().
  bool pwmE = ledcAttach(MOTOR_E_PWM, PWM_FREQ, PWM_RESOLUTION);
  bool pwmD = ledcAttach(MOTOR_D_PWM, PWM_FREQ, PWM_RESOLUTION);

  if (!pwmE) Serial.println("Erro ao configurar PWM do motor esquerdo");
  if (!pwmD) Serial.println("Erro ao configurar PWM do motor direito");

  parar();

  // Modo AP: o ESP32 cria a própria rede Wi-Fi.
  WiFi.mode(WIFI_AP);
  bool apOk = WiFi.softAP(ssid, password);

  // Desliga economia de energia do Wi-Fi para melhorar estabilidade do RSSI.
  esp_wifi_set_ps(WIFI_PS_NONE);

  if (apOk) {
    Serial.println("Access Point iniciado.");
    Serial.print("SSID: ");
    Serial.println(ssid);
    Serial.print("IP: ");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("Erro ao iniciar Access Point.");
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/parar", HTTP_GET, handleParar);
  server.on("/velocidade", HTTP_GET, handleVelocidade);
  server.on("/analog", HTTP_GET, handleAnalog);
  server.on("/seguir", HTTP_GET, handleSeguir);
  server.on("/rssi", HTTP_GET, handleRssi);
  server.on("/status", HTTP_GET, handleStatus);

  server.begin();

  Serial.println("Carrinho ESP32 corrigido iniciado.");
  Serial.println("Conecte no Wi-Fi do ESP32 e abra http://192.168.4.1");
}

// =====================================================
// LOOP
// =====================================================

void loop() {
  server.handleClient();

  if (modoSeguir) {
    static unsigned long ultimoCicloSeguidor = 0;

    // Atualiza o seguidor em torno de 14 vezes por segundo.
    // Isso evita processar RSSI rápido demais e deixar o robô nervoso.
    if (millis() - ultimoCicloSeguidor >= 70) {
      ultimoCicloSeguidor = millis();
      seguirDispositivo();
    }
  }
}
