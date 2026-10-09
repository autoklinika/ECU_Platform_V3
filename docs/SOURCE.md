# Pochodzenie kodu

## Zaimportowane bez zmian z ECU Platform V2

Źródło: `autoklinika/ECU_Platrorm_V2`, gałąź
`integration/ecu-v2-operational-candidate-20261009`,
commit `c03a68274554ea7f9f4c43a999ca0024f87d67ef` (2026-10-09).

| Katalog V3 | Katalog V2 |
|---|---|
| `src/core_v2/` | `src/core_v2/` |
| `src/bench/` | `src/bench/` |
| `src/dut_profile/` | `src/dut_profile/` |
| `src/dut_profiles/daf_sac/` | `src/dut_profiles/daf_sac/` |
| `src/platform/linux/v2/` | `src/platform/linux/v2/` |
| `tests/*_tests.cpp` (29 plików) | odpowiadające testy z `tests/` |

Zmieniony jest tylko sposób budowania (`CMakeLists.txt`, `tests/CMakeLists.txt`).
Kod źródłowy i testy są identyczne z V2. Poprawki w tych warstwach będą
wprowadzane w V3 osobnymi, opisanymi commitami.

## Świadomie niezaimportowane

| Element V2 | Powód |
|---|---|
| `src/api/`, `deploy/webgui/static_server.py` | Zastępuje je API wbudowane w Bench Runtime (jeden punkt wejścia) |
| `deploy/sac_connect/` (adapter Python), sondy `tests/*physical_probe*` | Proces na każdy odczyt — właśnie to zmienia V3 |
| `src/applications/daf_sac/` | Logika operacji przechodzi do Bench Runtime; część kodu zostanie przeniesiona w krokach 2–3 |
| `webgui/` | GUI zostanie przeniesione w kroku 5 jako cienki klient (bez własnej logiki świeżości danych) |
| `src/core/`, `src/ecu/sac/`, `src/platform/linux/socketcan/` | Starsze warstwy, w V2 już tylko referencyjne |
| skrypty `deploy_cm5_*`, `scripts/` | Zastąpi je jeden instalator (krok 8) |
| Bench Agent | Drugi niezależny właściciel CAN — w V3 niedopuszczalny |

## Wiedza z ECU Platform V1

`autoklinika/ecu_platform` (`release/v1.8`) jest źródłem wiedzy, nie kodu:

- wzorzec `VirtualCockpit`: jeden trwały silnik, kolejka komend, migawka stanu;
- ciągły odbiór FEAE zamiast okna czasowego;
- pasywne wykrywanie prędkości z `CANScannerEngine`;
- `CyclicTxScheduler` z gałęzi EGR jako wzorzec wiadomości cyklicznych;
- raporty incydentów EGR jako uzasadnienie oddzielenia GUI od sterowania.
