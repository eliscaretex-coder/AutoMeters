# XIAO ESP32-C6 + Atom: gravar e testar na bancada

Como funciona:

```
XIAO (bateria) --ESP-NOW, sem roteador--> AtomS3R (USB, no Wi-Fi) --internet--> Supabase
```

- A **XIAO** (firmware `xiao_c6_water_meter`, versão 3.1.0-c6) conta os pulsos dormindo e, a
  cada 15 minutos, manda os totais por **ESP-NOW** direto para o Atom. O rádio fica ligado só
  uma fração de segundo. Ela não precisa da senha do Wi-Fi.
- O **Atom** (firmware `atoms3r_water_meter`, versão 2.2.0) fica ligado no USB e no Wi-Fi.
  Ele recebe as leituras das XIAO, responde a cada uma e envia tudo ao Supabase. Ele não lê
  medidor nenhum (dá para ligar isso na página: "This Atom also reads a machine").
- **Tela do Atom:** a primeira página lista as máquinas com o total de litros (em laranja se
  ela não manda nada há mais de 2 envios). Cada toque na tela mostra uma máquina com Fria,
  Morna e Quente, quando foi o último envio e a bateria. Depois da última, volta para a lista.
  Se o Atom reiniciar, a lista começa vazia e vai enchendo conforme as XIAO mandam (até 15 min).
- A XIAO descobre sozinha o canal Wi-Fi do Atom e guarda esse canal. Se o roteador mudar de
  canal, ela procura de novo.
- As duas placas precisam da mesma **Network key** (padrão `autometers-net`). Placas com outra
  chave são ignoradas.

## 1. Ligação da XIAO

| | AtomS3R | XIAO ESP32-C6 |
|---|---|---|
| Fria / Morna / Quente | G5 / G6 / G7 | **D0 / D1 / D2** |
| GND | GND | GND |
| Tela | sim | não: o **LED amarelo** pisca a cada pulso (nos 10 min de teste) |
| Botões | tela | **B** (BOOT) e **R** (RESET), dos lados do USB-C |

A ligação de cada medidor é a mesma do Atom: **pino → diodo 1N4148 → terminal 2 do 7111**
(listra do diodo do lado do 7111) e **GND → terminal 6**.

## 2. Arduino IDE (uma vez)

1. Em **File > Preferences > Additional boards manager URLs**:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
2. Em **Tools > Board > Boards Manager**, instale **esp32** (Espressif), versão 3.x ou mais nova.
3. Em **Tools > Manage Libraries**, instale **ArduinoJson** e, para o Atom, **M5Unified**.

## 3. Gravar o Atom (primeiro)

1. Abra `atoms3r_water_meter/atoms3r_water_meter.ino`. Na mesma pasta precisam estar
   `secrets.h`, `supabase_roots.h` e `espnow_link.h`.
2. Placa: a mesma de antes (AtomS3R / AtomS3), com **USB CDC On Boot: Enabled**. Clique em Upload.
   Se aparecer "Sketch too big", escolha **Tools > Partition Scheme > Huge APP** e grave de novo.
3. Ele mantém as configurações que já tinha. No Serial Monitor (115200) deve aparecer
   `Gateway on: listening for readers on channel N`.
4. Na página do Atom (`http://<ip-do-atom>/`) aparece a seção **Battery readers**. A opção
   **Gateway** já vem marcada.

## 4. Gravar a XIAO

1. Abra `xiao_c6_water_meter/xiao_c6_water_meter.ino`. Na mesma pasta precisa estar
   `espnow_link.h` (o `secrets.h` é opcional).
2. **Tools > Board > esp32 > XIAO_ESP32C6** (não a ESP32C3). **USB CDC On Boot: Enabled**.
3. Clique em Upload. Se não gravar: segure **B**, aperte e solte **R**, solte **B** e tente de novo.
4. Abra o Serial Monitor em 115200 e aperte **R**.

## 5. Configurar a XIAO (uma vez por placa)

1. Na primeira vez ela entra sozinha no SETUP (LED piscando devagar). Se ela já foi configurada
   antes, aperte **R** e, enquanto o LED pisca rápido (3 s), aperte **B**.
2. No celular, conecte na rede **WaterMeter-XXXX** (senha `watermeter`). A página abre sozinha
   (ou abra `http://192.168.4.1`).
3. Preencha o **Machine name** (ex.: `Machine 02`). A **Network key** precisa ser igual à do
   Atom. Clique em **Save and restart**.

## 6. Testar

1. Uns 3 s depois de reiniciar, a XIAO manda a primeira leitura. O Serial Monitor mostra
   `Reading 1 -> gateway OK (ch 6, -48 dBm)`, e o LED dá **3 piscadas curtas**.
   Uma piscada longa quer dizer que o Atom não respondeu (veja o item 8).
2. No Atom, o Serial Monitor mostra `Reader 10:BD:A3:... (Machine 02): reading 1`. Na página
   do Atom, a XIAO aparece na tabela **Battery readers**, e a coluna **Supabase** muda para
   `sent hh:mm`.
3. Encoste **D0** no GND algumas vezes (o LED pisca a cada pulso) e aperte **B** rapidinho para
   mandar na hora. O número de pulsos na tabela do Atom deve subir.
4. **Teste dormindo:** depois de 10 minutos ela dorme (`going to deep sleep` e a porta COM some).
   Faça 5 pulsos em D0. Ela manda sozinha a cada 15 min. Para testar mais rápido, ponha
   **Send every** em 1 ou 2 no SETUP. O total no Atom e no Supabase deve subir 5.

## 7. Bateria

- Solde o vermelho no pad **BAT+** e o preto no **BAT−**, embaixo da placa. Atenção à
  polaridade: invertida, queima a placa.
- Leitura da tensão (opcional): dois resistores de 1 MΩ em série entre BAT+ e GND, com o ponto
  do meio no pad **MTMS**. A tensão aparece na tabela do Atom e vai ao Supabase como `battery_v`.
- Consumo dormindo: multímetro em µA em série com o fio vermelho. O esperado é de 15 a 40 µA.

## 8. Se a XIAO não achar o Atom

- Confira se o Atom está no Wi-Fi e se mostrou `Gateway on`.
- Confira se a **Network key** é igual nas duas. Na página do Atom aparece
  "N with a different network key" quando chega leitura com outra chave.
- **Alcance:** o ESP-NOW tem o alcance de um Wi-Fi comum. Teste com as placas perto e depois
  vá afastando. Caixa metálica e paredes grossas reduzem muito o sinal. Se uma máquina ficar
  longe, a XIAO tem um conector U.FL para antena externa.
