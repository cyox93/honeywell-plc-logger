# plc-log — 보일러 버스 PLC 패킷 로거 (ESP32-C3 Super Mini)

보일러 버스(MC200 · DT-200 · LT200SPD)의 PLC 프레임을 받아 **그룹마다 한 줄**로
OpenLog(microSD)에 기록하고, 메시지에서 읽어 낸 **각방 상태를 OLED에 표시**한다. `x3-thermostat`의 PLC 드라이버(`components/plc/`)와
plcloop의 UART0 덤프를 떼어 C3 보드 하나로 옮긴 것이다. 파형 · 메시지 규칙은
[docs/plc-protocol.md](docs/plc-protocol.md) (`x3-thermostat`에서 복사).

## 연결

| C3 핀 | 신호 | 방향 | 상대 | 비고 |
|---|---|---|---|---|
| GPIO5 | PLC `/SCTL` | → | PLC 모듈 J4-2 | **오픈드레인 전용** — 모듈 안 10 kΩ → +5 V 풀업. LOW = 캐리어 송신 |
| GPIO6 | PLC `DOUT_BUF` | ← | PLC 모듈 J4-3 | HIGH = 캐리어. RMT RX |
| GPIO21 | UART0 TX | → | OpenLog RXI | 로그 |
| GPIO20 | UART0 RX | ← | OpenLog TXO | 로깅에는 쓰지 않는다. `openlog` 진단 명령만 읽는다 |
| GPIO8 | I2C SDA | ↔ | OLED SDA | SSD1306 0.96" 128×64, 주소 0x3C. 보드 LED가 통신 때 깜빡인다 |
| GPIO9 | I2C SCL | → | OLED SCL | 400 kHz. BOOT 버튼과 같은 핀 |
| 3V3 | | | OLED VCC | |
| GND | | | PLC 모듈 · OpenLog · OLED | 공통 |
| USB-C | USB Serial/JTAG | | PC | 콘솔 · 플래시 |

- 핀 번호는 `components/include/board.h`에만 있다 — 바꾸면 이 표도 같이 고친다.
- PLC 모듈 J4-1(+5 V)은 연결하지 않는다.
- GPIO8/9는 스트래핑 핀이지만 OLED 모듈의 풀업이 리셋 때 HIGH로 잡아 정상 부팅된다.
  OLED를 꽂은 채 BOOT 버튼을 눌러 다운로드 모드로 들어가는 것도 된다.
- GPIO20/21은 C3의 기본 UART0 핀이라 **리셋 때 ROM 부트 메시지**가 OpenLog에도 찍힌다
  (파일 첫머리의 `ESP-ROM:...` 몇 줄). 앱 · 부트로더 로그는 USB로만 나간다.

## OpenLog 설정

**`config.txt`를 직접 만들지 않는다.** 빈 FAT32 카드를 꽂으면 OpenLog가 스스로 115200
설정으로 `config.txt`를 만든다 — 펌웨어도 115200 고정이라 그대로 맞는다.

- 직접 쓴 `config.txt`가 든 카드에서는 OpenLog가 SD 초기화 직후 리셋을 반복했다
  (TXO에 `1212…`, `<` 없음, 로그 파일 없음). 콘솔 `openlog`로 확인할 수 있다.
- OpenLog가 쓰다 멈추면 FAT가 어긋난다. Mac에서 `diskutil verifyVolume` / `repairVolume`,
  심하면 다시 포맷한다.

## 로그 형식

시각은 **부팅 후 ms** (보드에 시계가 없다).

```
812 # boot PLC-LOG 0.1.0
1043 83 43 46 | E4 13 37 2E                  그룹 = 폴링 + 응답, 메시지는 '|'로 구분
1297 83 01 04 | E4 21 00 06!                 '!' = 길이 · 체크섬이 맞지 않는 메시지
5310 # no-start H913 L116 H76 ...            해독 실패 버스트의 원시 펄스 (µs, H = 캐리어)
60812 # stat frames 940 groups 236 bad 0 no-start 0 bits 0 overflow 0 echo 0
```

- 그룹 = 100 ms 조용하면 닫는다 (plc-protocol.md §6.1).
- `# stat`은 60초마다 — 버스가 조용해도 로거가 살아 있었음을 남긴다 (`SC_PLCLOG_STAT_S`, 0 = 끔).
- OpenLog 상태는 확인하지 않는다 — 전원을 켠 직후 OpenLog가 SD를 여는 동안 들어온 첫 줄은 빠질 수 있다.

## OLED 화면

```
BOILER ON  HW 58.0        보일러 가동 (MC200 메시지의 0x10) · 온수 온도
LV 23.5/24.0 HEAT *       거실(DT-200, 주소 1): 실내/설정 · 모드 · * = 난방 요구
R1 -                      방 1 (주소 2): 아직 버스에 나타나지 않음
R2 21.0/22.0 AWAY         방 2 (주소 3)
R3   --/17.0 TMR  ?       ? = 10초 넘게 응답 없음, -- = 아직 모름
R4 -
R5 -
up 1:23:45 err 0          가동 시간 · 해독 오류 수
```

- 모드: `HEAT` 난방 · `AWAY` 외출 · `TMR` 타이머 · `RSV` 예약 · `OFF` 장기 정지 (그 밖은 `x..` 원시 값).
- **설정 온도와 모드는 처음엔 `--`다.** 버스에 나오는 때가 방이 값을 바꿔 알릴 때(`C5`),
  MC200이 전달할 때(`A5`), 전체 상태를 읽을 때(`CA … 1E`)뿐이라서다. 실내 온도는 몇 초 안에 찬다.
- 1초마다 다시 그리고, 번인을 줄이려고 1분마다 글자를 0 → 1 → 2 px 옮긴다.
- 화면이 뒤집혀 나오면 `menuconfig` → *PLC Log* → `SC_PLCLOG_OLED_FLIP`.
- OLED가 없거나 응답하지 않아도 로깅은 그대로 동작한다.
- 해석 규칙: `main/rooms.c` (근거는 plc-protocol.md §5).

## 빌드 · 플래시

```sh
source ~/.espressif/tools/activate_idf_v6.0.sh
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

ESP-IDF v6.0, 타깃 `esp32c3` (Flash 4 MB). 설정은 `sdkconfig.defaults` · `main/Kconfig.projbuild`.

## 콘솔 (USB)

| 명령 | 동작 |
|---|---|
| `plc` | 마지막 그룹 · 카운트 · 캐리어 · OpenLog UART 송신 중 여부 |
| `openlog [s]` | OpenLog가 TXO로 보낸 것 (부팅 후 쌓인 것 + s초 더). `12<` = 정상 준비, 같은 숫자 반복 = OpenLog 리셋 반복 |
| `rooms` | OLED와 같은 방별 상태 + 마지막 응답 시각 · 등록 · 타이머 · 예약 |
| `plc_mon <on\|off>` | 프레임마다 출력 |
| `plc_send <hex>...` | 바이트마다 PLC 프레임 송신. **실제 버스에 나간다** |
| `get_ver` · `help` | 버전 · 전체 목록 |
