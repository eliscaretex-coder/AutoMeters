# XIAO ESP32-C6: gravar e testar na bancada

Firmware `xiao_c6_water_meter` (versão 3.0.0-c6). Faz o mesmo que o do AtomS3R (conta os
pulsos das 3 entradas e envia ao Supabase), mas sem tela e com **deep sleep** para durar na
bateria. Os dados chegam na mesma página e na mesma tabela; cada XIAO aparece como um novo
aparelho (pelo MAC).

## 1. O que muda em relação ao Atom

| | AtomS3R | XIAO ESP32-C6 |
|---|---|---|
| Fria / Morna / Quente | G5 / G6 / G7 | **D0 / D1 / D2** |
| GND | GND | GND |
| Tela | sim | não: o **LED amarelo** pisca a cada pulso |
| Entrar no SETUP | segurar a tela 5 s | apertar **RESET** e, enquanto o LED pisca rápido (3 s), apertar **BOOT** |
| Energia | sempre ligado | dorme entre os pulsos; liga o Wi-Fi só para enviar |

A ligação de cada medidor continua a mesma: **pino → diodo 1N4148 → terminal 2 do 7111**
(listra do diodo do lado do 7111), e **GND da XIAO → terminal 6**.

Só os pinos GPIO0 a GPIO7 conseguem acordar o C6 do deep sleep. Na XIAO isso é D0, D1, D2 e os
pads de baixo (MTDI, MTCK, MTDO). Por isso as entradas ficaram em D0/D1/D2.

## 2. Preparar o Arduino IDE (uma vez)

1. **File > Preferences > Additional boards manager URLs**, adicione:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
2. **Tools > Board > Boards Manager**: procure **esp32** (da Espressif) e instale a versão 3.x.
3. **Tools > Manage Libraries**: instale **ArduinoJson** (Benoit Blanchon), versão 7.
4. Abra `xiao_c6_water_meter/xiao_c6_water_meter.ino`. Os três arquivos (`.ino`,
   `supabase_roots.h`, `secrets.h`) precisam estar na mesma pasta, com o mesmo nome da pasta.
5. Coloque o **token** no `secrets.h` (o mesmo do Atom). Assim cada XIAO nova só precisa do Wi-Fi.

## 3. Gravar

1. Ligue a XIAO no USB-C (cabo de dados, não só de carga).
2. **Tools > Board > esp32 > XIAO_ESP32C6**.
3. **Tools > USB CDC On Boot > Enabled** (para ver as mensagens no Serial Monitor).
4. **Tools > Port**: a porta que apareceu.
5. Clique em **Upload**.
   Se não gravar: segure **BOOT**, aperte e solte **RESET**, solte **BOOT** e tente de novo.
6. Abra o **Serial Monitor** em **115200**.

## 4. Primeiro teste (sem nada ligado)

1. Depois de gravar, sem Wi-Fi salvo, ela entra sozinha no **SETUP**: o LED pisca devagar.
2. No celular, conecte na rede **WaterMeter-XXXX**, senha `watermeter`. A página abre sozinha
   (ou abra `http://192.168.4.1`).
3. Preencha **Machine name**, **Wi-Fi** e senha. Deixe **Power mode** em **Battery**.
   Clique em **Save and restart**.
4. O LED pisca rápido por 3 s e ela entra na rede. No Serial Monitor aparece o IP e, uns 20 s
   depois, **Send OK**.
5. Confira na página do GitHub (ou na tabela `readings` do Supabase) que o aparelho novo
   apareceu.

## 5. Testar os pulsos

Durante os **10 minutos** depois de ligar ou apertar RESET, ela fica acordada para teste:

1. Com um fio, encoste **D0 no GND** e solte. O LED pisca, e o Serial Monitor mostra
   `Pulse Cold: 1 pulses`. Faça o mesmo com **D1** (Warm) e **D2** (Hot).
2. Abra `http://<ip-da-xiao>/` no navegador (usuário `admin`, senha `watermeter` se você não
   mudou). A tabela mostra os pulsos e se o contato está aberto ou fechado.
3. Clique em **Send a reading now** e confira no Supabase.

## 6. Testar o deep sleep

1. Depois dos 10 minutos, o Serial Monitor mostra `Test window over: going to deep sleep` e a
   porta USB some (isso é normal: no deep sleep o USB desliga).
2. Encoste D0 no GND umas 5 vezes, devagar (uma por segundo). O LED **não** pisca agora, para
   economizar.
3. Espere o próximo envio (pelo **Send every (min)**; para o teste, ponha **2** na página antes)
   ou aperte **RESET**. O total de Cold no Supabase / na página deve ter subido 5.

Se ao apertar RESET o total voltar a um número menor, me avise: é sinal de que algo não foi
salvo antes de dormir.

## 7. Bateria

- **Bateria**: solde o vermelho no pad **BAT+** e o preto no **BAT−**, embaixo da placa.
  Cuidado com a polaridade: invertida, queima a placa. A XIAO carrega a bateria pelo USB-C.
- **Leitura da tensão (opcional)**: dois resistores de 1 MΩ em série entre **BAT+** e **GND**,
  com o ponto do meio no pad **MTMS** (embaixo da placa). A tensão aparece na página
  (`Battery: 3.95 V`) e vai no envio como `battery_v`. Sem os resistores, a página mostra
  `no divider`, e tudo o mais funciona.
- **Medir o consumo**: ponha o multímetro em µA, em série com o fio vermelho da bateria, com a
  XIAO em deep sleep. O esperado é algo entre 15 e 40 µA. Se der mais de 1 mA, ela não está
  dormindo.
- **Se um contato ficar fechado** (ímã parado em cima do sensor), o pull-up gasta uns 70 µA a
  mais enquanto ele estiver fechado. Isso ainda dá meses de bateria.

## 8. Dicas

- **Instalação com fonte USB**: mude **Power mode** para **Always on**. Ela não dorme, e a
  página fica sempre disponível. Segure BOOT 5 s para entrar no SETUP.
- **Antena**: o firmware usa a antena interna. Se o sinal ficar fraco dentro da máquina, a
  XIAO tem um conector U.FL para antena externa; é só me avisar que eu mudo uma linha.
- **Litros por pulso** e o **ajuste para o número do 7111** funcionam como no Atom, pela página.
