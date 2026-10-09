# ECU Platform V3 — architektura

## 1. Dlaczego V3

| | V1.8 | V2 | V3 |
|---|---|---|---|
| Rdzeń protokołów | prosty, bez testów | Core V2, pełny, przetestowany | **Core V2** |
| Właściciel CAN | trwały silnik, ale w procesie GUI | nowy proces na każdy odczyt | **jeden trwały Bench Runtime** |
| Stan sesji | jedno miejsce (migawka) | rozproszony w 4 miejscach | **jedno miejsce** |
| GUI | steruje sprzętem (QML, `sudo`) | cienkie, ale samo pilnuje świeżości danych | **tylko wyświetla stan** |
| Wpływ GUI na czas sterowania | tak (incydenty EGR) | nie | **nie** |

V3 łączy to, co działało w obu wersjach: model trwałego silnika z V1
i rdzeń oraz separację GUI z V2.

## 2. Zasady (obowiązujące, sprawdzane w przeglądzie)

1. **Jeden właściciel stanu.** Stan sprzętu, sesji DUT, komunikacji i operacji
   istnieje wyłącznie w Bench Runtime. Pozostałe komponenty dostają jego
   migawkę i nie utrzymują własnej wersji tego stanu. GUI ma tylko stan
   prezentacji (zakładka, język, otwarte okna).
2. **Wyłączność sprzętu wymuszona przez system.** Tylko Bench Runtime ma
   `CAP_NET_ADMIN` i dostęp do `AF_CAN`; pozostałe usługi mają
   `RestrictAddressFamilies` bez `AF_CAN`. Dodatkowo `flock` na pliku blokady
   interfejsu. Stan `UP/DOWN` interfejsu nigdy nie jest mutexem.
3. **Zdefiniowany stan po starcie i zatrzymaniu.** Runtime przy starcie i przy
   SIGTERM doprowadza interfejsy do stanu bezpiecznego. Dla urządzeń
   z zasilaniem lub ruchem stan bezpieczny jest zdefiniowany per urządzenie
   (patrz zasada 7).
4. **Najpierw słuchaj, potem nadawaj.** Wykrywanie prędkości odbywa się w trybie
   listen-only. Nadawanie (UDS) dopiero na potwierdzonej prędkości. Po każdej
   operacji sprawdzane są liczniki błędów i bus-off.
5. **Pomiary żyją w pamięci.** Na flash zapisywane są tylko wyniki testów
   i dowody, nie pomiary bieżące. Każda wartość ma wiek i numer sekwencji
   nadany przez runtime (nie zegar ścienny).
6. **Czas sterowania jest niezależny od reszty.** Wiadomości cykliczne mają
   kontrakt „okres + termin” i są realizowane przez dedykowany mechanizm
   (wątek o podwyższonym priorytecie lub SocketCAN BCM), nigdy przez pętlę
   API ani renderowanie.
7. **Bezpieczeństwo operacji wysokiego ryzyka nie zależy tylko od Linuksa.**
   Power Manager i aktuatory muszą mieć własny stan bezpieczny po utracie
   heartbeat, a KL30 — sprzętowy wyłącznik awaryjny. Runtime jest drugą linią
   obrony.
8. **API przyjmuje komendy, nie surowe ramki.** Brak ogólnego „wyślij ramkę”.
   Każda komenda przechodzi przez politykę operacji (niżej). Kontrola Origin
   i Host obowiązuje dla każdego żądania zmieniającego stan.
9. **Modułowość nie oznacza wielu procesów.** Moduły mają twarde granice
   w kodzie; osobny proces tylko wtedy, gdy wymaga tego izolacja.
10. **Każdy krok kończy się działającym, przetestowanym stanem.** Bez kroków,
    które tylko „przygotowują strukturę”.

## 3. Polityka operacji

| Operacja | Ryzyko | Wymagania |
|---|---|---|
| Identyfikacja VIN/SW/HW | niskie | potwierdzona prędkość, profil |
| Odczyt parametrów | niskie | aktywna sesja, harmonogram CAN |
| Odczyt DTC | niskie | aktywna sesja |
| Kasowanie DTC | średnie | świeży odczyt DTC (≤ 180 s), jawne dwuetapowe potwierdzenie operatora, **obowiązkowy odczyt kontrolny po kasowaniu**, wynik zapisany jako dowód; początkowo tylko warianty z potwierdzonym fizycznie kasowaniem |
| Sterowanie KL30/KL15 | wysokie | interlock, limity, timeout, sprzętowy E-stop |
| Sterowanie EGR/VGT | wysokie | profil, limity, nadzór, kontrakt czasu |
| Zapis do ECU | bardzo wysokie | osobna polityka, poza zakresem |

Uwaga do kasowania DTC: w V2 jedno kasowanie dostało pozytywną odpowiedź
`0x54`, a niezależny odczyt nadal pokazał 12 DTC. Dlatego w V3 wynik kasowania
to zawsze para „odpowiedź ECU + odczyt kontrolny”, a GUI pokazuje wynik
odczytu kontrolnego, nie samą odpowiedź.

## 4. Struktura docelowa

```
WebGUI / Chromium kiosk     cienki klient: prezentacja i komendy operatora
        │  HTTP (komendy) + SSE (migawki stanu)
Bench Runtime (C++, jeden proces)
  ├─ API                    komendy, polityka operacji, strumień stanu
  ├─ Session Manager        DUT, tożsamość, cykl życia sesji
  ├─ Resource & Safety      wyłączność, blokady, limity, stan bezpieczny
  ├─ Measurement store      pomiary w pamięci, wiek, sekwencja
  ├─ (później) Test Engine  sekwencje, limity, OK/NOK
  └─ (później) Results      SQLite: wyniki i dowody
        │
Core V2 (bez zmian)         ISO-TP, UDS, J1939, ISOBUS
Platform                    Linux: SocketCAN, zegar; później RS485, Windows/J2534
```

Moduły „później” powstaną dopiero wtedy, gdy będą dwa rzeczywiste przypadki
użycia (SAC i pierwszy aktuator), z których da się wyprowadzić kontrakt.

## 5. Decyzje projektowe

**D1 — konfiguracja łącza przez `ip`, nie własny netlink (krok 1).**
Rdzeń V2 tylko sprawdza konfigurację łącza, nie ustawia jej. Do ustawiania V3
używa `ip` z dokładnie tymi poleceniami, które działały na CM5 w V2, ale
wywoływanymi wyłącznie przy zmianie konfiguracji (połączenie, zmiana
prędkości), a nie przy każdym pomiarze. Odczyt stanu idzie przez netlink
(kod V2). Własna implementacja zapisu przez netlink jest możliwa później za
tym samym interfejsem `ICanLinkControl`, gdy da się ją przetestować na sprzęcie.

**D2 — listen-only i brak ACK (krok 1 → 2).** W trybie listen-only kontroler
nie wysyła ACK. Jeśli sterownik jest jedynym innym węzłem, widzi wtedy brak
potwierdzenia — tak samo jak przy wyłączonym CM5. Przy nieznanej prędkości
to i tak najmniej inwazyjna opcja (tryb normal na złej prędkości wysyła ramki
błędów). Dlatego nasłuch ma trwać tylko do pierwszej poprawnej ramki, po czym
łącze przechodzi w tryb normal na wykrytej prędkości.

## 6. Czego V3 świadomie nie robi

- Nie zmienia Core V2 bez osobnego, uzasadnionego commitu z testem.
- Nie ma Bench Agenta ani żadnego drugiego procesu z dostępem do CAN.
- Nie przechowuje bieżących pomiarów w plikach.
- Nie ma ogólnego API ramek CAN.
- Nie wdraża łatek przez skrypty z wpisanymi na sztywno commitami.
