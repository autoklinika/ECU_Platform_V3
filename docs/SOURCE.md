# Pochodzenie kodu i wiedzy

## Kod przejęty: tylko Core V2

Źródło: `autoklinika/ECU_Platrorm_V2`, gałąź
`integration/ecu-v2-operational-candidate-20261009`,
commit `c03a68274554ea7f9f4c43a999ca0024f87d67ef` (2026-10-09).

| Katalog V3 | Katalog V2 | Zmiany |
|---|---|---|
| `src/core_v2/` | `src/core_v2/` | brak (identyczny bajt w bajt) |
| `tests/core_v2_*_tests.cpp` (20 plików) | te same pliki w `tests/` | brak |

Zmiany w Core V2 będą wprowadzane w V3 osobnymi, opisanymi commitami
z testem, który pokazuje powód zmiany.

Dlaczego Core V2 zostaje: jest przenośny (nie zależy od Linuksa ani GUI),
ma rozbudowane testy regresyjne i zgodności z normami, a jego interfejs
sterownika (`ICanDriver`) pozwala podłączyć nową warstwę sprzętową V3 bez
zmian w rdzeniu.

## Kod napisany dla V3

Wszystko poza powyższym: `src/runtime/`, `src/platform/linux/runtime/`,
`apps/`, `deploy/`, testy `runtime_*` i `linux_runtime_*`, CMake, CI,
dokumentacja. Nie zawiera kodu skopiowanego z V1 ani V2.

## Z V2 świadomie nie przejęto

Bench, profile DUT (w tym DAF SAC), aplikacja SAC, warstwa Linux V2
(adapter SocketCAN, zapytanie o łącze), API HTTP, WebGUI, adapter Python,
Bench Agent, skrypty wdrożeniowe. Powstaną od nowa w kolejnych krokach,
o ile będą potrzebne.

## Wiedza wykorzystana z V1 i V2 (bez kodu)

- **Błędy V2**, których V3 ma unikać: proces na każdy odczyt, `UP/DOWN`
  łącza jako mutex, `can0` pozostawiony UP po restarcie, okno 800 ms na FEAE,
  stan sesji rozproszony między procesami, aktywne UDS na nieznanej prędkości.
- **Wzorzec V1** `VirtualCockpit`: jeden trwały silnik, kolejka komend,
  migawka stanu, ciągły odbiór ramek.
- **Incydenty EGR w V1**: sterowanie czasowe nie może zależeć od wątku GUI.
- **Fakty o DAF SAC** potwierdzone fizycznie w V1/V2: adresy UDS
  `0x18DA30F9` / `0x18DAF930`, DID F190/F188/F192 (VIN/SW/HW), FE96
  (napięcia, 0,1 V/bit), PGN 0xFEAE od SA 0x30 (bajty 3–4, 0,08 bar/bit,
  0xFB–0xFF = niedostępne), warianty 250 i 500 kbit/s.
- **Polecenia iproute2** do konfiguracji łącza CAN, działające na CM5.
