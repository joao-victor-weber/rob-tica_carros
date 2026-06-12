/*
  Carrinho ESP32 - versão com abas HTML, RSSI por preset/manual e aceleração progressiva

  O que esta versão faz:
  1. Cria um Wi-Fi próprio no ESP32.
  2. Abre uma página de controle em http://192.168.4.1
  3. Controla dois motores DC usando driver L298N ou semelhante.
  4. Tem controle manual por joystick ou setas, alternando na primeira aba.
  5. Tem modo seguir Wi-Fi usando RSSI do celular conectado, sem ré automática.
  6. Tem três velocidades de comando: 0, 210 e 255.
  7. Usa aceleração progressiva interna para evitar trancos nos motores.
  8. Ao trocar de aba, o carrinho entra em estado de espera.
  9. Movimentos usam comandos explícitos por motor: (frente, ré, velocidade).

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
// Estes pinos costumam ser seguros para saída digital/PWM em placas ESP32 comuns.

#define MOTOR_E_PWM     25
#define MOTOR_E_FRENTE  18
#define MOTOR_E_RE      19

#define MOTOR_D_PWM     26
#define MOTOR_D_FRENTE  21
#define MOTOR_D_RE      22

// Se algum motor estiver girando ao contrário, troque false para true.
#define INVERTER_MOTOR_E false
#define INVERTER_MOTOR_D false

// =====================================================
// PWM, VELOCIDADES E ACELERAÇÃO
// =====================================================
// A API ledcAttach usa o próprio pino como referência no Arduino-ESP32 3.x.
// Com resolução de 8 bits, o duty vai de 0 até 255.

#define PWM_FREQ        5000
#define PWM_RESOLUTION  8

#define VEL_PARADO      0
#define VEL_MEDIA       210
#define VEL_MAXIMA      255

// O comando do usuário continua tendo somente 0, 210 e 255.
// A aceleração usa valores intermediários internamente para suavizar o motor.
int velocidadeManual = VEL_MEDIA;

// Ajuste fino da aceleração.
// Quanto maior o passo, mais rápida e mais brusca a resposta.
// Quanto menor o passo, mais suave e mais lenta.
#define ACEL_INTERVALO_MS   25
#define ACEL_PASSO_SUBIDA   12
#define ACEL_PASSO_DESCIDA  18

int alvoVelEsq = 0;
int alvoVelDir = 0;
int atualVelEsq = 0;
int atualVelDir = 0;
unsigned long ultimoPassoAceleracao = 0;

// =====================================================
// PARÂMETROS DO SEGUIDOR WI-FI
// =====================================================
// RSSI é negativo.
// Quanto MENOS negativo, melhor o sinal.
// Exemplo: -40 é melhor/mais perto que -75.

#define RSSI_SEM_CLIENTE -100

#define RSSI_ALVO_PADRAO        -45
#define RSSI_LONGE_PADRAO       -62
#define RSSI_MUITO_PERTO_PADRAO -30

// Presets de distância para o modo seguir.
// Perto: tenta manter o celular com sinal mais forte.
// Médio: equilíbrio.
// Longe: aceita o celular mais distante antes de avançar.
#define PRESET_PERTO_ALVO        -38
#define PRESET_PERTO_LONGE       -55
#define PRESET_PERTO_MUITO_PERTO -27

#define PRESET_MEDIO_ALVO        -45
#define PRESET_MEDIO_LONGE       -62
#define PRESET_MEDIO_MUITO_PERTO -30

#define PRESET_LONGE_ALVO        -55
#define PRESET_LONGE_LONGE       -72
#define PRESET_LONGE_MUITO_PERTO -35

int rssiAlvoDin = RSSI_ALVO_PADRAO;
int rssiLongeDin = RSSI_LONGE_PADRAO;
int rssiMuitoPertoDin = RSSI_MUITO_PERTO_PADRAO;

bool rssiManual = true;
String rssiPresetAtual = "manual";

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

// Segurança manual: se o celular parar de mandar comandos, o carrinho para.
#define TIMEOUT_MANUAL_MS       900

// =====================================================
// ESTADO GLOBAL DO CONTROLE
// =====================================================

bool modoSeguir = false;
bool estadoEspera = true;

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
unsigned long ultimoComandoManual = 0;

String ultimaAcao = "Em espera";

// Protótipos importantes para evitar problema de ordem em alguns preprocessadores.
void resetarSeguidor();
void entrarEmEspera();

// =====================================================
// HTML DA PÁGINA WEB
// =====================================================
// Rotas usadas pela interface:
//   /espera
//   /cmd?acao=frente|re|esquerda|direita|parar
//   /analog?x=...&y=...
//   /velocidade?v=0|210|255
//   /seguir?ativo=1|0
//   /rssi?alvo=...&longe=...&perto=...
//   /preset?p=perto|medio|longe
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
    background: #07111f;
    color: #e8eefc;
    touch-action: none;
  }
  h2 { margin: 8px 0 12px; color: #67e8f9; }
  h3 { margin: 8px 0 12px; }
  .card {
    max-width: 460px;
    margin: 10px auto;
    padding: 14px;
    background: #111c30;
    border: 1px solid #24364f;
    border-radius: 16px;
    box-shadow: 0 10px 30px rgba(0,0,0,0.22);
  }
  .aba { display: none; }
  .aba.ativa { display: block; }
  .menu {
    max-width: 460px;
    margin: 8px auto 12px;
    display: grid;
    grid-template-columns: 1fr 1fr 1fr;
    gap: 7px;
  }
  button {
    padding: 12px 14px;
    margin: 4px;
    border: 0;
    border-radius: 12px;
    font-weight: bold;
    color: white;
    background: #2563eb;
    min-height: 44px;
  }
  button:active { transform: scale(0.98); }
  button.sec { background: #334155; }
  button.stop { background: #dc2626; }
  button.on { background: #16a34a; }
  button.off { background: #475569; }
  button.ativo { background: #0891b2; outline: 2px solid #67e8f9; }
  .row {
    display: flex;
    gap: 8px;
    justify-content: center;
    flex-wrap: wrap;
    align-items: center;
  }
  .info {
    text-align: left;
    line-height: 1.75;
    font-size: 15px;
  }
  label { display: block; margin-top: 10px; font-size: 13px; color: #a9bbd8; }
  input[type=range] { width: 100%; }
  .mini { color: #a9bbd8; font-size: 12px; line-height: 1.45; }
  .painelControle { display: none; }
  .painelControle.ativo { display: block; }
  canvas {
    background: #0b1324;
    border: 2px solid #334766;
    border-radius: 50%;
    touch-action: none;
    max-width: 100%;
  }
  .setas {
    display: grid;
    grid-template-columns: 78px 78px 78px;
    gap: 8px;
    justify-content: center;
    align-items: center;
    margin: 12px auto;
  }
  .setas button { width: 78px; height: 62px; font-size: 24px; margin: 0; }
  .mutedBox {
    background: #0b1324;
    border: 1px solid #24364f;
    border-radius: 12px;
    padding: 10px;
    margin-top: 8px;
  }
  .grid2 {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 8px;
  }
</style>
</head>
<body>

<h2>Carrinho ESP32</h2>

<div class="menu">
  <button id="mControle" class="ativo" onclick="abrirAba('controle')">Controle</button>
  <button id="mRssi" class="sec" onclick="abrirAba('rssi')">RSSI</button>
  <button id="mStatus" class="sec" onclick="abrirAba('status')">Status</button>
</div>

<section id="abaControle" class="aba ativa">
  <div class="card">
    <h3>Controle manual</h3>
    <p class="mini">Ao trocar de aba, o carrinho entra em espera e para até você mexer nos controles ou ligar o seguir automático.</p>
    <div class="row">
      <button id="btnJoy" class="ativo" onclick="mostrarControle('joystick')">Joystick</button>
      <button id="btnSetas" class="sec" onclick="mostrarControle('setas')">Controle de setas</button>
    </div>

    <div id="painelJoystick" class="painelControle ativo">
      <canvas id="joy" width="240" height="240"></canvas>
      <p class="mini">Arraste para controlar. Solte para parar.</p>
    </div>

    <div id="painelSetas" class="painelControle">
      <div class="setas">
        <div></div>
        <button data-acao="frente">▲</button>
        <div></div>
        <button data-acao="esquerda">◀</button>
        <button class="stop" data-acao="parar">■</button>
        <button data-acao="direita">▶</button>
        <div></div>
        <button data-acao="re">▼</button>
        <div></div>
      </div>
      <p class="mini">Segure uma seta para andar. Solte para parar.</p>
    </div>
  </div>

  <div class="card">
    <h3>Velocidade manual</h3>
    <div class="row">
      <button onclick="setVel(0)">0</button>
      <button onclick="setVel(210)">210</button>
      <button onclick="setVel(255)">255</button>
    </div>
    <p>Velocidade atual: <b id="velAtual">210</b></p>
  </div>
</section>

<section id="abaRssi" class="aba">
  <div class="card">
    <h3>Distância do modo seguir</h3>
    <p class="mini">Manual e presets são exclusivos. Quando você mexe no manual, os presets desligam. Quando escolhe um preset, o manual fica travado.</p>

    <div class="row">
      <button id="btnManual" class="ativo" onclick="ativarManualRSSI()">Manual</button>
      <button id="pPerto" class="sec" onclick="presetRSSI('perto')">Perto</button>
      <button id="pMedio" class="sec" onclick="presetRSSI('medio')">Médio</button>
      <button id="pLonge" class="sec" onclick="presetRSSI('longe')">Longe</button>
    </div>

    <div id="manualRSSI" class="mutedBox">
      <label>RSSI alvo: <b id="vAlvo">-45</b></label>
      <input id="rAlvo" type="range" min="-80" max="-20" value="-45" oninput="enviaRSSI()">

      <label>RSSI longe: <b id="vLonge">-62</b></label>
      <input id="rLonge" type="range" min="-95" max="-45" value="-62" oninput="enviaRSSI()">

      <label>RSSI muito perto: <b id="vPerto">-30</b></label>
      <input id="rPerto" type="range" min="-55" max="-15" value="-30" oninput="enviaRSSI()">
    </div>

    <div class="mutedBox info">
      <div>Configuração ativa: <b id="modoRSSI">manual</b></div>
      <div>Alvo: <b id="cfgAlvo">-45</b> dBm</div>
      <div>Longe: <b id="cfgLonge">-62</b> dBm</div>
      <div>Muito perto: <b id="cfgPerto">-30</b> dBm</div>
    </div>
  </div>
</section>

<section id="abaStatus" class="aba">
  <div class="card">
    <h3>Seguir automático</h3>
    <div class="row">
      <button id="btnSeguir" class="off" onclick="toggleSeguir()">Seguir: OFF</button>
      <button class="stop" onclick="pararTudo()">PARAR</button>
    </div>
    <p class="mini">Conecte apenas um celular na rede do carrinho para evitar confusão no RSSI.</p>
  </div>

  <div class="card info">
    <h3>Status do carrinho</h3>
    <div>Estado: <b id="estado">--</b></div>
    <div>Clientes conectados: <b id="clientes">0</b></div>
    <div>RSSI bruto: <b id="rssiRaw">--</b> dBm</div>
    <div>RSSI suavizado: <b id="rssiSuave">--</b> dBm</div>
    <div>Ação: <b id="acao">--</b></div>
    <div>Motor esquerdo: <b id="motorE">0</b></div>
    <div>Motor direito: <b id="motorD">0</b></div>
    <div>Velocidade manual: <b id="velStatus">210</b></div>
    <div>RSSI ativo: <b id="rssiAtivo">manual</b></div>
  </div>
</section>

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
let abaAtual = 'controle';
let tipoControle = 'joystick';
let ultimoX = 0;
let ultimoY = 0;
let comandoSeta = '';
let rssiManualAtivo = true;

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

function marcarMenu(nome) {
  const pares = [
    ['controle', 'mControle'],
    ['rssi', 'mRssi'],
    ['status', 'mStatus']
  ];

  pares.forEach(p => {
    const botao = document.getElementById(p[1]);
    botao.className = p[0] === nome ? 'ativo' : 'sec';
  });
}

function abrirAba(nome) {
  if (nome !== abaAtual) {
    entrarEspera();
  }

  abaAtual = nome;
  document.getElementById('abaControle').classList.toggle('ativa', nome === 'controle');
  document.getElementById('abaRssi').classList.toggle('ativa', nome === 'rssi');
  document.getElementById('abaStatus').classList.toggle('ativa', nome === 'status');
  marcarMenu(nome);
  atualizarStatus();
}

function entrarEspera() {
  modoSeguir = false;
  comandoSeta = '';
  pararJoystickLocal();
  atualizarBotaoSeguir();
  fetch('/espera').catch(() => {});
}

function mostrarControle(tipo) {
  tipoControle = tipo;
  entrarEspera();
  document.getElementById('painelJoystick').classList.toggle('ativo', tipo === 'joystick');
  document.getElementById('painelSetas').classList.toggle('ativo', tipo === 'setas');
  document.getElementById('btnJoy').className = tipo === 'joystick' ? 'ativo' : 'sec';
  document.getElementById('btnSetas').className = tipo === 'setas' ? 'ativo' : 'sec';
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

function pararJoystickLocal() {
  kx = cx;
  ky = cy;
  ultimoX = 0;
  ultimoY = 0;
  arrastando = false;
  desenhar();
}

function pararJoystick() {
  pararJoystickLocal();
  enviarAnalogico(0, 0);
}

function enviarCmd(acao) {
  if (modoSeguir) desligarSeguir();
  fetch('/cmd?acao=' + acao).catch(() => {});
}

function pararTudo() {
  modoSeguir = false;
  comandoSeta = '';
  pararJoystickLocal();
  atualizarBotaoSeguir();
  fetch('/parar').catch(() => {});
}

function setVel(v) {
  document.getElementById('velAtual').textContent = v;
  document.getElementById('velStatus').textContent = v;
  fetch('/velocidade?v=' + v).catch(() => {});
}

function toggleSeguir() {
  modoSeguir = !modoSeguir;
  comandoSeta = '';
  pararJoystickLocal();
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

function travarSlidersRSSI(travar) {
  document.getElementById('rAlvo').disabled = travar;
  document.getElementById('rLonge').disabled = travar;
  document.getElementById('rPerto').disabled = travar;
}

function pintarModoRSSI(modo) {
  document.getElementById('btnManual').className = modo === 'manual' ? 'ativo' : 'sec';
  document.getElementById('pPerto').className = modo === 'perto' ? 'ativo' : 'sec';
  document.getElementById('pMedio').className = modo === 'medio' ? 'ativo' : 'sec';
  document.getElementById('pLonge').className = modo === 'longe' ? 'ativo' : 'sec';
  travarSlidersRSSI(modo !== 'manual');
}

function ativarManualRSSI() {
  rssiManualAtivo = true;
  pintarModoRSSI('manual');
  enviaRSSI();
}

function enviaRSSI() {
  const alvo = document.getElementById('rAlvo').value;
  const longe = document.getElementById('rLonge').value;
  const perto = document.getElementById('rPerto').value;

  document.getElementById('vAlvo').textContent = alvo;
  document.getElementById('vLonge').textContent = longe;
  document.getElementById('vPerto').textContent = perto;

  document.getElementById('cfgAlvo').textContent = alvo;
  document.getElementById('cfgLonge').textContent = longe;
  document.getElementById('cfgPerto').textContent = perto;
  document.getElementById('modoRSSI').textContent = 'manual';
  document.getElementById('rssiAtivo').textContent = 'manual';

  rssiManualAtivo = true;
  pintarModoRSSI('manual');

  fetch('/rssi?alvo=' + alvo + '&longe=' + longe + '&perto=' + perto).catch(() => {});
}

function presetRSSI(nome) {
  rssiManualAtivo = false;
  pintarModoRSSI(nome);
  fetch('/preset?p=' + nome)
    .then(() => atualizarStatus())
    .catch(() => {});
}

function atualizarStatus() {
  fetch('/status')
    .then(r => r.json())
    .then(d => {
      document.getElementById('clientes').textContent = d.clientes;
      document.getElementById('rssiRaw').textContent = d.rssiRaw;
      document.getElementById('rssiSuave').textContent = d.rssiSuavizado;
      document.getElementById('acao').textContent = d.acao;
      document.getElementById('estado').textContent = d.estado;
      document.getElementById('motorE').textContent = d.motorE;
      document.getElementById('motorD').textContent = d.motorD;
      document.getElementById('velAtual').textContent = d.velocidade;
      document.getElementById('velStatus').textContent = d.velocidade;

      document.getElementById('cfgAlvo').textContent = d.rssiAlvo;
      document.getElementById('cfgLonge').textContent = d.rssiLonge;
      document.getElementById('cfgPerto').textContent = d.rssiMuitoPerto;
      document.getElementById('modoRSSI').textContent = d.rssiPreset;
      document.getElementById('rssiAtivo').textContent = d.rssiPreset;

      document.getElementById('rAlvo').value = d.rssiAlvo;
      document.getElementById('rLonge').value = d.rssiLonge;
      document.getElementById('rPerto').value = d.rssiMuitoPerto;
      document.getElementById('vAlvo').textContent = d.rssiAlvo;
      document.getElementById('vLonge').textContent = d.rssiLonge;
      document.getElementById('vPerto').textContent = d.rssiMuitoPerto;

      pintarModoRSSI(d.rssiPreset);

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

document.querySelectorAll('.setas button').forEach(btn => {
  btn.addEventListener('pointerdown', e => {
    e.preventDefault();
    const acao = btn.dataset.acao;
    comandoSeta = acao === 'parar' ? '' : acao;
    enviarCmd(acao);
  });

  btn.addEventListener('pointerup', e => {
    e.preventDefault();
    comandoSeta = '';
    enviarCmd('parar');
  });

  btn.addEventListener('pointerleave', e => {
    if (!comandoSeta) return;
    e.preventDefault();
    comandoSeta = '';
    enviarCmd('parar');
  });

  btn.addEventListener('pointercancel', e => {
    e.preventDefault();
    comandoSeta = '';
    enviarCmd('parar');
  });
});

// Mantém o comando vivo enquanto o dedo estiver segurando seta ou joystick.
setInterval(() => {
  if (abaAtual !== 'controle') return;

  if (tipoControle === 'setas' && comandoSeta) {
    enviarCmd(comandoSeta);
  }

  if (tipoControle === 'joystick' && arrastando) {
    enviarAnalogico(ultimoX, ultimoY);
  }
}, 180);

setInterval(atualizarStatus, 500);
desenhar();
atualizarStatus();
</script>
</body>
</html>
)rawliteral";

// =====================================================
// 1. FUNÇÕES DE VELOCIDADE
// =====================================================
// Os comandos externos continuam limitados a três níveis:
//   0   = parado
//   210 = velocidade média
//   255 = velocidade máxima
//
// A aceleração progressiva gera valores intermediários internamente.

int normalizarVelocidade(int velocidade) {
  velocidade = constrain(velocidade, VEL_PARADO, VEL_MAXIMA);

  if (velocidade == VEL_PARADO) return VEL_PARADO;
  if (velocidade <= VEL_MEDIA) return VEL_MEDIA;
  return VEL_MAXIMA;
}

// Interpreta o par de comandos usado por cada motor:
//   (1, 0) = frente
//   (0, 1) = ré
//   (0, 0) = parado
//   (1, 1) = inválido, portanto parado por segurança
int obterDirecaoMotor(bool comandoFrente, bool comandoRe) {
  if (comandoFrente && !comandoRe) return 1;
  if (!comandoFrente && comandoRe) return -1;
  return 0;
}

// =====================================================
// 2. FUNÇÕES DOS MOTORES
// =====================================================
// motorEsquerdo() e motorDireito() definem apenas o alvo de cada roda.
// A aceleração progressiva leva o PWM atual até esse alvo.
//
// A inversão é aplicada aqui uma única vez. A função física que escreve
// nos pinos não deve inverter novamente.

int criarAlvoMotor(
  bool comandoFrente,
  bool comandoRe,
  int velocidade,
  bool inverter
) {
  int direcao = obterDirecaoMotor(comandoFrente, comandoRe);

  if (direcao == 0) {
    return VEL_PARADO;
  }

  int velocidadePermitida = normalizarVelocidade(velocidade);

  if (velocidadePermitida == VEL_PARADO) {
    return VEL_PARADO;
  }

  if (inverter) {
    direcao = -direcao;
  }

  return direcao * velocidadePermitida;
}

void motorEsquerdo(bool comandoFrente, bool comandoRe, int velocidade) {
  alvoVelEsq = criarAlvoMotor(
    comandoFrente,
    comandoRe,
    velocidade,
    INVERTER_MOTOR_E
  );
}

void motorDireito(bool comandoFrente, bool comandoRe, int velocidade) {
  alvoVelDir = criarAlvoMotor(
    comandoFrente,
    comandoRe,
    velocidade,
    INVERTER_MOTOR_D
  );
}

// Aplica diretamente a velocidade atual produzida pela rampa.
// Valor positivo = pino de frente.
// Valor negativo = pino de ré.
// Zero = motor sem acionamento.
void aplicarMotorFisico(
  int pinoFrente,
  int pinoRe,
  int pinoPWM,
  int velocidadeAtual
) {
  int pwm = constrain(abs(velocidadeAtual), VEL_PARADO, VEL_MAXIMA);

  if (pwm == VEL_PARADO) {
    digitalWrite(pinoFrente, LOW);
    digitalWrite(pinoRe, LOW);
    ledcWrite(pinoPWM, VEL_PARADO);
    return;
  }

  if (velocidadeAtual > 0) {
    digitalWrite(pinoFrente, HIGH);
    digitalWrite(pinoRe, LOW);
  } else {
    digitalWrite(pinoFrente, LOW);
    digitalWrite(pinoRe, HIGH);
  }

  ledcWrite(pinoPWM, pwm);
}

void aplicarPWMAtual() {
  aplicarMotorFisico(
    MOTOR_E_FRENTE,
    MOTOR_E_RE,
    MOTOR_E_PWM,
    atualVelEsq
  );

  aplicarMotorFisico(
    MOTOR_D_FRENTE,
    MOTOR_D_RE,
    MOTOR_D_PWM,
    atualVelDir
  );
}

// =====================================================
// 3. FUNÇÕES DE ACELERAÇÃO E PARADA
// =====================================================

int aproximarPWM(int atual, int alvo) {
  if (atual == alvo) return atual;

  // Se a direção mudar, o motor primeiro desacelera até zero.
  if (atual != VEL_PARADO && alvo != VEL_PARADO) {
    bool sinaisOpostos = (atual > 0 && alvo < 0) ||
                         (atual < 0 && alvo > 0);

    if (sinaisOpostos) {
      alvo = VEL_PARADO;
    }
  }

  bool aumentandoModulo = abs(alvo) > abs(atual);
  int passo = aumentandoModulo ? ACEL_PASSO_SUBIDA : ACEL_PASSO_DESCIDA;

  if (atual < alvo) {
    atual += passo;
    if (atual > alvo) atual = alvo;
  } else {
    atual -= passo;
    if (atual < alvo) atual = alvo;
  }

  return atual;
}

void atualizarAceleracao() {
  unsigned long agora = millis();

  if (agora - ultimoPassoAceleracao < ACEL_INTERVALO_MS) {
    return;
  }

  ultimoPassoAceleracao = agora;

  atualVelEsq = aproximarPWM(atualVelEsq, alvoVelEsq);
  atualVelDir = aproximarPWM(atualVelDir, alvoVelDir);
  aplicarPWMAtual();
}

void parar() {
  // Parada suave: os alvos vão para zero e a rampa desacelera as rodas.
  motorEsquerdo(0, 0, VEL_PARADO);
  motorDireito(0, 0, VEL_PARADO);
}

void pararImediato() {
  // Parada de segurança: zera alvo e PWM sem aguardar a rampa.
  alvoVelEsq = VEL_PARADO;
  alvoVelDir = VEL_PARADO;
  atualVelEsq = VEL_PARADO;
  atualVelDir = VEL_PARADO;
  aplicarPWMAtual();
}

// =====================================================
// 4. FUNÇÕES DE MOVIMENTO
// =====================================================

void frente(int velocidade) {
  motorEsquerdo(1, 0, velocidade);
  motorDireito(1, 0, velocidade);
}

void re(int velocidade) {
  // Ré disponível somente para o controle manual.
  motorEsquerdo(0, 1, velocidade);
  motorDireito(0, 1, velocidade);
}

void virarEsquerda(int velocidade) {
  // Curva por pivô: roda esquerda parada e direita para frente.
  // O modo seguir usa esta função e, portanto, não aciona ré.
  motorEsquerdo(0, 0, VEL_PARADO);
  motorDireito(1, 0, velocidade);
}

void virarDireita(int velocidade) {
  // Curva por pivô: roda direita parada e esquerda para frente.
  // O modo seguir usa esta função e, portanto, não aciona ré.
  motorEsquerdo(1, 0, velocidade);
  motorDireito(0, 0, VEL_PARADO);
}

void girarEsquerda(int velocidade) {
  // Giro no próprio eixo, usado somente no controle manual pelo joystick.
  motorEsquerdo(0, 1, velocidade);
  motorDireito(1, 0, velocidade);
}

void girarDireita(int velocidade) {
  // Giro no próprio eixo, usado somente no controle manual pelo joystick.
  motorEsquerdo(1, 0, velocidade);
  motorDireito(0, 1, velocidade);
}

void curvaSuave(int direcao) {
  // Usada pelo modo seguir Wi-Fi sem acionar ré.
  // direcao < 0: roda esquerda mais lenta.
  // direcao > 0: roda direita mais lenta.
  direcao = constrain(direcao, -1, 1);

  if (direcao < 0) {
    motorEsquerdo(1, 0, VEL_MEDIA);
    motorDireito(1, 0, VEL_MAXIMA);
  } else if (direcao > 0) {
    motorEsquerdo(1, 0, VEL_MAXIMA);
    motorDireito(1, 0, VEL_MEDIA);
  } else {
    frente(VEL_MEDIA);
  }
}

void registrarComandoManual() {
  modoSeguir = false;
  estadoEspera = false;
  ultimoComandoManual = millis();
}

void aplicarJoystick() {
  // Joystick mantém comandos principais em 0, 210 e 255.
  // A aceleração suaviza internamente até chegar ao alvo.

  registrarComandoManual();

  int y = constrain(joyY, -100, 100);
  int x = constrain(joyX, -100, 100);

  const int zonaMorta = 8;
  const int limiteCurva = 25;

  if (abs(x) < zonaMorta && abs(y) < zonaMorta) {
    parar();
    ultimaAcao = "Manual parado";
    return;
  }

  if (velocidadeManual == VEL_PARADO) {
    parar();
    ultimaAcao = "Velocidade manual zero";
    return;
  }

  int base = velocidadeManual;
  int lenta = (base == VEL_MAXIMA) ? VEL_MEDIA : VEL_PARADO;

  if (abs(y) < limiteCurva) {
    if (x > 0) {
      girarDireita(base);
      ultimaAcao = "Girando direita";
    } else {
      girarEsquerda(base);
      ultimaAcao = "Girando esquerda";
    }
    return;
  }

  if (y > 0) {
    if (x > limiteCurva) {
      motorEsquerdo(1, 0, base);
      motorDireito(1, 0, lenta);
      ultimaAcao = "Curva direita";
    } else if (x < -limiteCurva) {
      motorEsquerdo(1, 0, lenta);
      motorDireito(1, 0, base);
      ultimaAcao = "Curva esquerda";
    } else {
      frente(base);
      ultimaAcao = "Frente";
    }
    return;
  }

  if (y < 0) {
    if (x > limiteCurva) {
      motorEsquerdo(0, 1, base);
      motorDireito(0, 1, lenta);
      ultimaAcao = "Ré curva direita";
    } else if (x < -limiteCurva) {
      motorEsquerdo(0, 1, lenta);
      motorDireito(0, 1, base);
      ultimaAcao = "Ré curva esquerda";
    } else {
      re(base);
      ultimaAcao = "Ré";
    }
    return;
  }
}

void comandoSetas(String acao) {
  registrarComandoManual();

  if (acao == "frente") {
    frente(velocidadeManual);
    ultimaAcao = "Setas: frente";
    return;
  }

  if (acao == "re") {
    re(velocidadeManual);
    ultimaAcao = "Setas: ré";
    return;
  }

  if (acao == "esquerda") {
    virarEsquerda(velocidadeManual);
    ultimaAcao = "Setas: esquerda";
    return;
  }

  if (acao == "direita") {
    virarDireita(velocidadeManual);
    ultimaAcao = "Setas: direita";
    return;
  }

  parar();
  ultimaAcao = "Manual parado";
}

void entrarEmEspera() {
  modoSeguir = false;
  estadoEspera = true;
  joyX = 0;
  joyY = 0;
  pararImediato();
  resetarSeguidor();
  ultimaAcao = "Em espera";
}

// =====================================================
// RSSI DO CELULAR CONECTADO
// =====================================================

int obterRSSI() {
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

void aplicarPresetRSSI(String preset) {
  rssiManual = false;
  rssiPresetAtual = preset;

  if (preset == "perto") {
    rssiAlvoDin = PRESET_PERTO_ALVO;
    rssiLongeDin = PRESET_PERTO_LONGE;
    rssiMuitoPertoDin = PRESET_PERTO_MUITO_PERTO;
    return;
  }

  if (preset == "longe") {
    rssiAlvoDin = PRESET_LONGE_ALVO;
    rssiLongeDin = PRESET_LONGE_LONGE;
    rssiMuitoPertoDin = PRESET_LONGE_MUITO_PERTO;
    return;
  }

  // Médio também é o padrão se vier valor estranho.
  rssiPresetAtual = "medio";
  rssiAlvoDin = PRESET_MEDIO_ALVO;
  rssiLongeDin = PRESET_MEDIO_LONGE;
  rssiMuitoPertoDin = PRESET_MEDIO_MUITO_PERTO;
}

// =====================================================
// MODO SEGUIR WI-FI
// =====================================================

int velocidadePorErroRSSI(int erroAbsoluto) {
  if (erroAbsoluto <= ZONA_MORTA_RSSI) return VEL_PARADO;
  if (erroAbsoluto <= 10) return VEL_MEDIA;
  return VEL_MAXIMA;
}

void buscarDirecao() {
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
    // No modo automático o carrinho nunca anda de ré.
    // Quando estiver mais perto que o alvo, ele apenas para.
    parar();
    ultimaAcao = "Perto do alvo: parado";
  }
}

void seguirDispositivo() {
  estadoEspera = false;

  if (WiFi.softAPgetStationNum() == 0) {
    pararImediato();
    resetarSeguidor();
    ultimaAcao = "Aguardando celular";
    return;
  }

  if (!atualizarRSSI()) {
    pararImediato();
    resetarSeguidor();
    ultimaAcao = "RSSI inválido";
    return;
  }

  if (rssiSuavizado > rssiMuitoPertoDin) {
    // Segurança: o seguir automático não recua.
    // Se o celular estiver muito perto, o PWM é zerado imediatamente.
    pararImediato();
    inicioBusca = 0;
    faseCorrecao = 0;
    emCorrecao = false;
    ultimaAcao = "Muito perto: parado";
    return;
  }

  if (rssiSuavizado < rssiLongeDin) {
    buscarDirecao();
    faseCorrecao = 0;
    emCorrecao = false;
    return;
  }

  inicioBusca = 0;

  if (emCorrecao) {
    if (millis() - tempoInicioFase < DURACAO_CORRECAO_MS) {
      return;
    }

    emCorrecao = false;
    parar();
  }

  controlarDistancia();

  if (!emCorrecao && rssiSuavizado < rssiAlvoDin) {
    corrigirDirecao();
  }
}

// =====================================================
// ROTAS DO SERVIDOR WEB
// =====================================================

String escaparJSON(String texto) {
  texto.replace("\\", "\\\\");
  texto.replace("\"", "\\\"");
  texto.replace("\n", " ");
  texto.replace("\r", " ");
  return texto;
}

void handleRoot() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", pagina);
}

void handleEsperar() {
  entrarEmEspera();
  server.send(200, "text/plain", "espera");
}

void handleParar() {
  modoSeguir = false;
  estadoEspera = true;
  joyX = 0;
  joyY = 0;
  pararImediato();
  resetarSeguidor();
  ultimaAcao = "Parado";
  server.send(200, "text/plain", "ok");
}

void handleVelocidade() {
  if (server.hasArg("v")) {
    int v = server.arg("v").toInt();

    if (v <= 0) velocidadeManual = VEL_PARADO;
    else if (v <= VEL_MEDIA) velocidadeManual = VEL_MEDIA;
    else velocidadeManual = VEL_MAXIMA;
  }

  server.send(200, "text/plain", String(velocidadeManual));
}

void handleCmd() {
  String acao = server.hasArg("acao") ? server.arg("acao") : "parar";
  comandoSetas(acao);
  server.send(200, "text/plain", "ok");
}

void handleAnalog() {
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
      estadoEspera = false;
      ultimaAcao = "Seguidor iniciado";
    }

    if (!novoModo) {
      pararImediato();
      resetarSeguidor();
      estadoEspera = true;
      ultimaAcao = "Seguidor desligado";
    }

    joyX = 0;
    joyY = 0;
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

  rssiManual = true;
  rssiPresetAtual = "manual";
  server.send(200, "text/plain", "ok");
}

void handlePreset() {
  String p = server.hasArg("p") ? server.arg("p") : "medio";
  aplicarPresetRSSI(p);
  server.send(200, "text/plain", rssiPresetAtual);
}

void handleStatus() {
  int clientes = WiFi.softAPgetStationNum();
  int rssiRaw = obterRSSI();

  String estado = "Manual";
  if (estadoEspera) estado = "Espera";
  if (modoSeguir) estado = "Seguindo";

  String json = "{";
  json += "\"clientes\":" + String(clientes) + ",";
  json += "\"rssiRaw\":" + String(rssiRaw) + ",";
  json += "\"rssiSuavizado\":" + String(rssiSuavizado) + ",";
  json += "\"velocidade\":" + String(velocidadeManual) + ",";
  json += "\"motorE\":" + String(atualVelEsq) + ",";
  json += "\"motorD\":" + String(atualVelDir) + ",";
  json += "\"rssiAlvo\":" + String(rssiAlvoDin) + ",";
  json += "\"rssiLonge\":" + String(rssiLongeDin) + ",";
  json += "\"rssiMuitoPerto\":" + String(rssiMuitoPertoDin) + ",";
  json += "\"rssiManual\":";
  json += (rssiManual ? "true" : "false");
  json += ",\"rssiPreset\":\"" + escaparJSON(rssiPresetAtual) + "\",";
  json += "\"estadoEspera\":";
  json += (estadoEspera ? "true" : "false");
  json += ",\"seguir\":";
  json += (modoSeguir ? "true" : "false");
  json += ",\"estado\":\"" + escaparJSON(estado) + "\"";
  json += ",\"acao\":\"" + escaparJSON(ultimaAcao) + "\"";
  json += "}";

  server.send(200, "application/json", json);
}

// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(MOTOR_E_FRENTE, OUTPUT);
  pinMode(MOTOR_E_RE, OUTPUT);
  pinMode(MOTOR_D_FRENTE, OUTPUT);
  pinMode(MOTOR_D_RE, OUTPUT);

  bool pwmE = ledcAttach(MOTOR_E_PWM, PWM_FREQ, PWM_RESOLUTION);
  bool pwmD = ledcAttach(MOTOR_D_PWM, PWM_FREQ, PWM_RESOLUTION);

  if (!pwmE) Serial.println("Erro ao configurar PWM do motor esquerdo");
  if (!pwmD) Serial.println("Erro ao configurar PWM do motor direito");

  pararImediato();

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
  server.on("/espera", HTTP_GET, handleEsperar);
  server.on("/parar", HTTP_GET, handleParar);
  server.on("/velocidade", HTTP_GET, handleVelocidade);
  server.on("/cmd", HTTP_GET, handleCmd);
  server.on("/analog", HTTP_GET, handleAnalog);
  server.on("/seguir", HTTP_GET, handleSeguir);
  server.on("/rssi", HTTP_GET, handleRssi);
  server.on("/preset", HTTP_GET, handlePreset);
  server.on("/status", HTTP_GET, handleStatus);

  server.begin();

  Serial.println("Carrinho ESP32 iniciado com motores por comandos frente/re, abas e aceleração progressiva.");
  Serial.println("Conecte no Wi-Fi do ESP32 e abra http://192.168.4.1");
}

// =====================================================
// LOOP
// =====================================================

void loop() {
  server.handleClient();
  atualizarAceleracao();

  // Se o controle manual parar de mandar sinal, reduz o alvo para zero.
  // A página envia batimentos enquanto seta ou joystick estão ativos.
  if (!modoSeguir && !estadoEspera && (alvoVelEsq != 0 || alvoVelDir != 0)) {
    if (millis() - ultimoComandoManual > TIMEOUT_MANUAL_MS) {
      pararImediato();
      ultimaAcao = "Manual timeout";
      estadoEspera = true;
    }
  }

  if (modoSeguir) {
    static unsigned long ultimoCicloSeguidor = 0;

    if (millis() - ultimoCicloSeguidor >= 70) {
      ultimoCicloSeguidor = millis();
      seguirDispositivo();
    }
  }
}
