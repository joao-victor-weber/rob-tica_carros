# Carrinho ESP32 com Controle Web

Projeto de carrinho robótico usando **ESP32**, controle por **Wi-Fi próprio** e interface web acessada pelo celular.
O carrinho pode ser controlado manualmente por um joystick virtual e também possui um modo experimental de **seguir Wi-Fi**, usando a força do sinal RSSI do celular conectado.

## Funcionalidades

* Cria uma rede Wi-Fi própria no ESP32.
* Interface de controle acessada pelo navegador.
* Controle manual por joystick virtual.
* Controle de dois motores DC com driver L298N ou semelhante.
* Três níveis de velocidade: parado, média e máxima.
* Botão de parada.
* Modo experimental para seguir o celular usando RSSI.
* Ajuste dos limites de RSSI pela própria página web.

## Componentes necessários

* 1 ESP32
* 1 driver de motor L298N ou semelhante
* 2 motores DC
* Fonte ou bateria para os motores
* Jumpers
* Chassi para carrinho
* Celular ou computador para acessar a página de controle

## Ligações sugeridas

### Motor esquerdo

| Função | Pino ESP32 |
| ------ | ---------- |
| ENA    | GPIO25     |
| IN1    | GPIO18     |
| IN2    | GPIO19     |

### Motor direito

| Função | Pino ESP32 |
| ------ | ---------- |
| ENB    | GPIO26     |
| IN3    | GPIO21     |
| IN4    | GPIO22     |

## Alimentação

Os motores devem ser alimentados por uma fonte ou bateria separada.
O **GND do ESP32 precisa estar ligado ao GND do driver dos motores**, para que o controle funcione corretamente.

## Como usar

1. Abra o arquivo `carrinho_esp32_pronto_para_rodar.ino` na Arduino IDE.
2. Selecione a placa ESP32 correta.
3. Envie o código para o ESP32.
4. Depois de gravar, conecte o celular ou computador na rede Wi-Fi:

```txt
Nome da rede: ESP32_Car
Senha: 12345678
```

5. Abra o navegador e acesse:

```txt
http://192.168.4.1
```

6. Use o joystick virtual para controlar o carrinho.

## Modos de controle

### Modo manual

No modo manual, o carrinho é controlado pelo joystick da página web.
Também é possível escolher entre três velocidades:

* 0: parado
* 127: velocidade média
* 255: velocidade máxima

### Modo seguir Wi-Fi

O modo seguir Wi-Fi tenta fazer o carrinho se aproximar ou se afastar do celular usando a força do sinal Wi-Fi.

Importante: o ESP32 não consegue saber exatamente a direção do celular usando apenas uma antena.
Por isso, o código faz pequenos testes para a esquerda e para a direita, compara o RSSI e escolhe o lado onde o sinal melhora.

Esse modo é experimental e pode variar bastante dependendo do ambiente, distância, obstáculos e interferências.

## Rotas usadas pela página web

A interface web envia comandos para o ESP32 usando rotas HTTP:

```txt
/analog?x=...&y=...
/velocidade?v=0|127|255
/seguir?ativo=1|0
/rssi?alvo=...&longe=...&perto=...
/status
/parar
```

## Observações

* O código usa PWM com resolução de 8 bits.
* Os valores de velocidade usados são 0, 127 e 255.
* Os pinos 12 e 15 foram evitados porque podem causar problemas no boot de algumas placas ESP32.
* O modo seguir Wi-Fi depende da leitura de RSSI e não deve ser tratado como localização precisa.

## Arquivo principal

```txt
carrinho_esp32_pronto_para_rodar.ino
```

## Status do projeto

Projeto funcional e pronto para testes em bancada ou em um carrinho físico com ESP32.
