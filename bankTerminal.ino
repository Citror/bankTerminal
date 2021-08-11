/*
 * soubor bankTerminal.ino
 * 
 * Obslužný kód pro terminál
 *
 * vývojová verze
 *
 * Author: Vojtěch Vilímek 
 * License: GNU GLP v3
 *
 */

// inkluze knihoven
#include <SPI.h>
#include <LiquidCrystal.h>
#include <MFRC522.h>
#include <EEPROM.h>


// hlavičkový soubor obsahující české znaky
#include "znaky.h"


/*
 * definice ladících konstant
 */

#define DEBUG 0  // nechej na nule, jinak bude program nestabilní z důvodu nedostatku paměti
#define SERIAL 1  // normálně 1, radši neměň!


/*
 *  Další definice konstant:
 *    VELIKOST_DATOVEHO_TYPU
 *       pro lepší flexibilitu a možnost snažšího rozšíření limitu 
 *       (unsigned int může nést max. 65 535)
 *    VELIKOST
 *       obsahuje počet bytů UID karet
 *    RYCHLOST_POSUNU
 *       určuje, jak rychle se posouvá text po obrazovce
 *    DEBOUNCE
 *       mechanická tlačítka se reálně stisknou několikrát (chvění plíšku)
 *       nejsnažší cesta je problém řešit softwareově pomocí čekací lhůty
 *    CHYBA_LIMITU
 *       chybová hláška při dosaženi limitu při zadávání hodnoty
 *    KOEFICIENT
 *       číselná hodnota pro prodloužení zobrazení zprávy od úplného začátku
 *       před zahájení pohybu po obrazovce
 *      
 */

#define VELIKOST_DATOVEHO_TYPU  2 // (sizeof(unsigned int))

#define VELIKOST               5

#define RYCHLOST_POSUNU        500
#define DEBOUNCE               390
#define CHYBA_LIMITU           "Nelze zpracovat!"

#define KOEFICIENT             RYCHLOST_POSUNU * 0.6


/*
 * informace ohledně ukládání množství peněz na MIFARE 1k karty
 *
 * zápis/čtení probíhá v první dvou bytech sektoru 1 bloku 4
 *
 * hodnota je uložena ve formátu unsigned int formou LSB
 *
 * mapa daného bloku
 * Byte - 0   1   2   3   4   6   7   8   9   10  11  12  13  14  15  16
 *        LSB MSB --- --- --- --- --- --- --- --- --- --- --- --- --- ---
 */
 
#define SEKTOR          1
#define ADRESA_BLOKU    4
#define KONTROLNI_BLOK  7


/* 
 * čtečky RFID karet - modul RC522
 *   tento modul využívá čipu NPX MFRC522 (odtud jméno knihovny)
 * 
 * obě čtečky jsou zapojeny na stejný SPI bus a navíc na společný reset
 * 
 * každá čtečka musí mít vlastní linku pro výběr 
 *
 * vysvětlivky: RST - reset
 *  SS_0 - slave select pro 1. čtečku
 *  SS_1 - stejné jako výše pro 2. (někdy se také označuje CS od Chip Select)
 *  MOSI - master out slave in
 *  MISO - master in slave out
 *  SCK - serial clock, pin pro synchrozaci (na zvestupné hraně pro mod=0)
 *
 */

#define SPI_RST    9
#define SPI_SS_0  A5
#define SPI_SS_1  10
#define SPI_MOSI  11
#define SPI_MISO  12
#define SPI_SCK   13

#define POCET_CTECEK  2

const byte ssPiny[] = { SPI_SS_0, SPI_SS_1 };


/*
 * LCD Display - typ 1602A
 *   tento displej má šestnáct znaků ve dvou řádcích
 *
 * je připojen na společný bus s klávesnicí
 * 
 * Zapojení (pro lepší přestavu viz obrázek lcd.png)
 * DISPLAY | význam  |  Arduino pin  |  přípojka
 * --------+---------+---------------+---------------
 *    0    |  GND    |  GND          | 2pin-0 
 *    1    |  +5V    |  5V           | 2pin-1
 *    2    |  V0     |  NC[*]        |
 *    3    |  RS     |  A2           | 3pin-2
 *    4    |  RW     |  A1           | 3pin-1
 *    5    |  EN     |  A0           | 3pin-0
 *  6 - 9  |  D0-D3  |               |
 *   10    |  D4     |  2            | 4pin-0
 *   11    |  D5     |  3            | 4pin-1
 *   12    |  D6     |  4            | 4pin-2
 *   13    |  D7     |  5            | 4pin-3
 *   14    |  AN     |               |
 *   15    |  CA     |               |
 *
 * pozn. display je ovládán v 4 bitovém módu, proto není třeba připojovat D0-D4 (datové linky nižšího nibblu).
 * vysvětlivky: RS - register select (vybírá jestli se jedná o příkaz, nebo o data)
 *   RW - řídá jestli je o zápis, nebo čtení
 *   EN = ENABLE signál pro potvzení příkazu (na vzestupné hraně)
 * D4, D5, D6, D7 - datové linky vyššího nibblu
 * AN je anoda led podsvícení, je připojena u displeje z přívodního napětí (přes 2pin)
 * CA je katoda led podsvícení je připojena stejně jako anoda
 *
 */

#define LCD_ENABLE  A0
#define LCD_RW      A1
#define LCD_REGSEL  A2 // REGister SELect

#define BUS_0 2
#define BUS_1 3
#define BUS_2 4
#define BUS_3 5


/*
 * Klávesnice - maticový typ
 * 
 *  klávesnice je připojena sedmi piny
 *  pro ušetření hardwarových pinů je použit společná sběrnice s LCD
 *
 * pro lepší čitelnost nadefinované konstanty
 *
 * stisknuté klávesy jsou čteny na základě kontroly průchodu proudu maticí
 * to je realizováno pomocí krátkých impluzů, které jsou čteny na vstupních pinech
 * impulzy jsou postupně vysílány na všechny "výstupní" piny
 *
 * jednotlivé piny klávesnice jsou uspořádány do polí Pulzni, resp. Cteci
 *
 * více viz funkci ctiKlavesu()
 */

#define KB_READ_0 7  // piny jsou na konektoru přeházené, rychlejší zapojení
#define KB_READ_1 8
#define KB_READ_2 6

const byte PulzniPiny[] = {BUS_3, BUS_2, BUS_1, BUS_0};
const byte CteciPiny[] = {KB_READ_0, KB_READ_1, KB_READ_2};


/*
 * Data pro identifikaci privilegovaných karet
 *    jsou obsažena v seznamu privilegovaneKarty
 *    jeho délka je jasně daná v POCET_PRIVILEGOVANYCH_KARET
 * 
 * 
 * Identifikace karty se zpravou
 *    je obsažena v UidKartySeZpravou
 *    pozn.: v aktuální verzi je podporována pouze jedna karta
 * 
 */

const byte privilegovaneKarty[][10] = {
    {0x83, 0x5E, 0xDD, 0x16},
    {0x69, 0xC5, 0xBA, 0xB3},
};

#define POCET_PRIVILEGOVANYCH_KARET 2

const byte UidKartySeZpravou[] = {
    0x8C, 0x52, 0xC0, 0x02
};


/*
 * mapa pro převod elektrických signálů na čísla
 */

const byte Map[4][3] = {
    { 1,  2,  3},
    { 4,  5,  6},
    { 7,  8,  9},
    {11, 10, 12}
};


/*
 * Globální proměnné
 *
 *  stav - určuje pracovní fázi - čtení karty, zobrazení finančního stavu, ...
 *  minuly - proměnná držící čas pro časování posunu znaků na obrazovce
 *  aktualni - opět čas, tentokrát pro debounce tlačítek
 *  *stiskla - proměnná obsahující naposledy stisknutou klávesu (pointer)
 *  vstupniKarta, vystupniKarta - pole obsahujici identifikační čísla karet pro jejich rozlišení
 *  vstupni - mnozství peněz na vložené kartě
 *  prevod - peníze určené k převodu
 *  vystupni - peníze obsažené v kartě, která má obdržet peníze ze vložené karty
 *  pozice - pozice určuje buňku EEPROM paměti, která se právě používá
 *
 *  pozn. karty jsou sice pojmenované vstupní a výstupní, to ale neznamená, že 
 *     by se na jednu pouze psalo, nebo z ní četlo
 *     názvy byly takto vybrány kvůli peněžním pochodům
 */

byte stav;
unsigned long minuly, aktualni;
unsigned long skok;
byte *stiskla; 
unsigned int vstupni, prevod, vystupni;
int pozice;
bool privilegovany;


/*
 *  Vytvoření objektů
 *    pro obrazovku
 *    pro pole karet
 * 
 * LCD obrazovka, popsáno již dříve zde pouze způsob volání:
 *   LiquidCrystal(rs, rw, enable, d4, d5, d6, d7);
 *
 * deklarace pole objektů MFRC522 (čtečky)
 *
 * deklarace pole pro reprezentaci klíče pro ověřenou komunikace s kartami
 *
 */

LiquidCrystal lcd(LCD_REGSEL, LCD_RW, LCD_ENABLE, BUS_0, BUS_1, BUS_2, BUS_3);

MFRC522 ctecky[POCET_CTECEK];
MFRC522::MIFARE_Key klic;

MFRC522::Uid vstupniKarta;
MFRC522::Uid vystupniKarta;


/*
 * setup - příprava na běh nekonečné smyčky
 */
void setup() {
    // příprava pinů pro klávesnici
    pinMode(KB_READ_0, INPUT);
    pinMode(KB_READ_1, INPUT);
    pinMode(KB_READ_2, INPUT);

    #if SERIAL
        Serial.begin(9600);
        Serial.println();
        Serial.println(F("Spojení navázáno."));
        Serial.println();
        Serial.println();
    #endif 

    // zahájení a zobrazení úvodní zpravy na LCD
    lcd.begin(16, 2);
    lcd.noCursor();
    lcd.clear();
    lcd.home();

    lcd.write("Inicializace ...");
    

    SPI.begin();

    #if DEBUG
        Serial.println(F("SPI sběrnice připravena."));
        Serial.println(F("Zahajuji přípravy RFID čteček ..."));
    #endif

    for (byte index = 0; index < POCET_CTECEK; index++) {
        ctecky[index].PCD_Init(ssPiny[index], SPI_RST);

        #if SERIAL
            Serial.print(F("Čtečka "));
            Serial.print(index);
            Serial.print(F(": "));
            ctecky[index].PCD_DumpVersionToSerial();
        #endif
        delay(100);
    } // for (byte index = 0; ...
        

    // příprava klíče
    for (byte i = 0; i < 6; i++)
        klic.keyByte[i] = 0xFF;

    #if DEBUG
        Serial.println("Čtečky jsou připraveny k použití");
    #endif

    // naplnění identifikačních polí pro karty práznými hodnotami
    /*
    for (byte i = 0; i < 16; i++) {
        vstupniKarta[i] = 0x00;
        vystupniKarta[i] = 0x00;
    }
    */
    
    // vložení do paměti displeje znak '€'
    lcd.createChar(7, Euro);

    // příprava nulových hodnot pro globální proměnné
    vstupni = 0, prevod = 0, vystupni = 0;

    minuly = 0, aktualni = 0;

    stiskla = NULL;
    while (stiskla == NULL) {
      stiskla = (byte *) malloc(sizeof(byte));
    }

    vstupniKarta.size = 0;
    vystupniKarta.size = 0;
    vstupniKarta.sak = 0;
    vystupniKarta.sak = 0;

    for (byte i = 0; i < 10; i++) {
        vstupniKarta.uidByte[i] = 0x00;
        vystupniKarta.uidByte[i] = 0x00;
    }

    
    *stiskla = 0;

    /* nalezení aktuální pozice pro ukládání dočasných dat */
    pozice = najdiPoziciEEPROM();

    skok = 0;

    stav = 0;

    privilegovany = false;

    delay(1000);
} // void setup()

/*
 * znovupřihlašování (problém při znovu vložení karty - isNewCardPresent()
debug privilegovaných karet
obecná stabilita
funkčnost powerbanek
poslední verze MRFC522 knihovny


 */


/*
 * Hlavní programová smyčka, donekonečka se opakuje
 */
void loop() { 
    Serial.println(stav);
    switch (stav) {
        case 0:
            /* zobrazí na obrazovce úvodní zprávu */
            #if DEBUG
                Serial.println("Stav 0: vlozteKartu(), přecházím na hledání karty ");
            #endif

            /* kontrola správnosti průběhu zápisů */
            if (nastalProblem()) {
              stav = 0xF0;
            }

            vlozteKartu(0);
            stav++;
            break;

        case 1:
            /* čekání na vložení a úspěšné přečtení nové karty na levé čtečce */
            if (ctiKartu(0, 0, &vstupni, &vstupniKarta)) {
                #if DEBUG
                    Serial.println("Stav 1: podmíněný ctiKartu(), karta nalezena");
                #endif
                
                #if SERIAL
                    for (byte i = 0; i < vstupniKarta.size; i++) {
                        Serial.print(vstupniKarta.uidByte[i], HEX);
                        Serial.print("  ");
                    }
                    Serial.println();
                #endif

                if (jePrivilegovana(vstupniKarta)) {
                    privilegovany = true;
                }

                /* zobrazí zprávu */
                nevyjimejteKartu();
                if (privilegovany)
                    delay(400);
                else
                    delay(2400);

                /* uložení millis() pro zahájení na záčátku zprávy v beziciMoznosti() */
                skok = millis() - KOEFICIENT;
                
                if (kartaSeZpravou(vstupniKarta)) {
                    Serial.println("karta se zpravou");
                    stav = 0xEE;
                    return;
                }

                if (privilegovany) {
                    privilegovaneMenu();
                } else {
                    stavKonta(vstupni);
                }

                stav++;

            }  // if (ctiKartu(0, 0, &vstupni, vstupniKarta))
            break;

        case 2:
            /* posouvá po obrazovce zprávu do zadání vstupu */
            if (privilegovany)
                beziciPrivilegovaneMoznosti();
            else
                beziciMoznosti();

            /* obsluha stisknuté klávesy */
            if (ctiKlavesu(stiskla)) {
                if (*stiskla == 1) {
                    #if DEBUG
                        Serial.println("Stav 2: ctiKlavesu() přecházím na převod");
                    #endif

                    lcd.clear();

                    minuly = millis();
                    *stiskla = 0;
                    zadejMnozstvi(&prevod, stiskla);

                    /* uložení skoku pro zahájení na začátku zprávy v beziciInformace() */
                    skok = millis() + 20;
                    
                    stav++;

                } else if(*stiskla == 9) {
                    #if DEBUG
                        Serial.println("Stav 2: ctiKlavesu() provádím odhlášení");
                    #endif

                    ctecky[0].PICC_HaltA();
                    ctecky[0].PCD_StopCrypto1();
                    
                    odhlasuji();
                    delay(3600);

                    vstupni = 0;
                    prevod = 0;
                    vystupni = 0;

                    privilegovany = false;

                    stav = 0; 
                } else if (privilegovany && *stiskla == 5) {
                    /* výpočet skoku pro správné zobrazování */
                    skok = millis() - KOEFICIENT;
                    
                    stav = 0xA0;
                }

            } // if (ctiKlavesu(stiskla))
            break;

        case 3:
            /* zajišťuje čtení kláves při zadávání množství, zobrazuje ovládání */
            beziciInformace();
            
            if (byte docasny = zadejMnozstvi(&prevod, stiskla)) {
                if (docasny == 1) {
                    #if DEBUG
                        Serial.println("Stav 3: zadejMnozstvi() přecházím k potvrzení");
                    #endif

                    /* nulový převod nemá smysl */
                    if (prevod > 0) {
                      /* zobrazeni zpravy */
                      if (!privilegovany)
                        potvrzeniPresunu();
  
                      stav++; 
                    }
                } // if (docasny == 1)
                
                else if (docasny == 2) {
                    #if DEBUG
                        Serial.println("Stav 3: zadejMnozstvi() návrat k účtu");
                    #endif

                    /* vyčisti celý display */
                    lcd.clear();

                    if (!privilegovany)
                        stavKonta(vstupni);
                    else 
                        privilegovaneMenu();

                     /* uložení millis() pro zahájení na záčátku zprávy v beziciMoznosti() */
                    skok = millis() - KOEFICIENT;

                    /* zdrž zobrazení aby tolik neblikal */
                    delay(30);

                    stav--;
                } // else if (docasny == 2)

            } // if (byte docasny = zadejMnozstvi(&prevod, stiskla))
            break;

        case 4:
            /* obsluha klávsenice pro potvrzeniPresunu() */
            if (privilegovany) {
                stav++;
            }
            
            else if (ctiKlavesu(stiskla)) {
                if (*stiskla == 11) {
                    #if DEBUG
                        Serial.println("Stav 4: ctiKlavesu() převod potvzen, pokračuji");
                    #endif

                    stav++;
                }
                else if (*stiskla == 12) {
                    #if DEBUG
                        Serial.println("Stav 4: ctiKlavesu() převod přerušen");
                    #endif

                    ukoncujiPrevod();
                    delay(2400);

                    stavKonta(vstupni);

                    minuly = millis();
                    *stiskla = 0;

                    prevod = 0;

                    stav = 2;
                }
            } // if (ctiKlavesu(stiskla))
            break;

        case 5:
            /* kontrola dostatku financí */
            if (privilegovany) {
                stav++;
            }
            else if (provedKontrolu(vstupni, prevod)) {
                #if DEBUG
                    Serial.println("Stav 5: provedKontrolu() dostatek financí na účtu");
                #endif

                /* příprava znaků pro vlozteKartu() */
                lcd.createChar(0, HacekZ);
                
                ctecky[1].uid.size = 0;
                ctecky[1].uid.sak = 0;
                ctecky[1].PCD_StopCrypto1();

                vlozteKartu(1);

                // temp
                ctecky[1].PICC_HaltA();
                ctecky[1].PCD_StopCrypto1();

                stav++;
            } else {
                #if DEBUG
                    Serial.println("Stav 5: provedKontrolu() nedostatek financí!");
                #endif

                nedostatekFinanci();
                delay(2400);

                /* příprava znaků pro beziciInformace (při stavu == 3) */
                lcd.createChar(4, PraveTlacitko0);
                lcd.createChar(5, PraveTlacitko1);

                stav = 3;
            }
            break;

        case 6:
            if (ctiKlavesu(stiskla)) {
                if (*stiskla == 12) {
                    prevod = 0;
                    stav = 2;
                    
                    if (privilegovany)
                        privilegovaneMenu();
                    else
                        stavKonta(vstupni);

                    skok = millis() - KOEFICIENT;
                    return;
                }
            }

            if (privilegovany && rozdilCasu(minuly, millis()) < 2000) {
                return;
            }
            
            /* čeká na vložení druhé karty */
            if (ctiKartu(1, 0, &vystupni, &vystupniKarta)) {
                #if DEBUG
                    Serial.println("Stav 6: podmíněný ctiKartu(), karta nalezena");
                #endif
                #if SERIAL /*
                    Serial.println("Druhá karta nalezena!"); */
                #endif

                /* zobrazí zprávu */
                nemanipulujte();
                
                stav++;
            }

            break;

        case 7:
            /* vlastní převod - srhnutí peněz */
            if (privilegovany) {
                if (jePrivilegovana(vystupniKarta)) {
                    
                    lcd.clear();

                    lcd.setCursor(0, 1);
                    lcd.write(CHYBA_LIMITU);
                    stav--;

                    minuly = millis();
                    
                    return;
                }
                stav++;
            }
            else if (zapisNaKartu(0, 1, ( vstupni - prevod ), vstupniKarta)) {
                /* uložení stržených peněz */
                zapisData(dalsi(pozice), prevod);

                
                #if DEBUG
                    Serial.println();
                    Serial.println();
                    Serial.println("Strhnutí OK");
                    Serial.println(); 
                #endif
                
                stav++;
            }
            /* jestliže je stisknuta klávesa zpět, odejdi do výběru akcí */
            if (ctiKlavesu(stiskla)) {
                if (*stiskla == 12) {
                    prevod = 0;
                    vstupni = 0;
                    vystupni = 0;

                    stav = 2;

                    lcd.clear();
                    privilegovaneMenu();
                    skok = millis() - KOEFICIENT;
                }
            }
            break;

        case 8:
            /* vlastní převod - zvýšení stavu na druhé kartě */
            if (zapisNaKartu(1, 1, ( vystupni + prevod ), vystupniKarta)) {
                if (!privilegovany) {
                    zapisData(pozice, 0);
                    pozice = dalsi(pozice);
                }

                
                #if DEBUG
                    Serial.println();
                    Serial.println();
                    Serial.println("Navýšení OK");
                    Serial.println();
                #endif
                stav++;
            }
            break;

        case 9:
            if (privilegovany)
                delay(200);
            else
                delay(800);
                
            uspesnyPrubeh();
            
            if (privilegovany)
                delay(400);
            else
                delay(2400);

            vstupni = 0;
            if (!privilegovany)
                prevod = 0;
            vystupni = 0;

            if (!privilegovany)
                vstupniKarta.size = 0;
            vystupniKarta.size = 0;

            for (byte i = 0; i < 10; i++) {
                if (!privilegovany)
                    vstupniKarta.uidByte[i] = 0x00;
                vystupniKarta.uidByte[i] = 0x00;
            }


            /* zajištění funkčnosti i při dalším použití pomocí znovu inicializace čteček */
            if (!privilegovany)
                ctecky[0].PCD_Init(ssPiny[0], SPI_RST);
            ctecky[1].PCD_Init(ssPiny[1], SPI_RST);
            
            if (privilegovany)
                stav = 6;
            else
                stav = 0;
            break;

        case 0xA0:
            beziciInformace();
            
            if (byte docasny = zadejMnozstvi(&prevod, stiskla)) {
                if (docasny == 1) {
                    if (prevod > 0) {
                        sniz(prevod);
                        
                        lcd.setCursor(0, 1);
                        lcd.write("                ");
                        
                        stav++;
                    }
                }
                else if (docasny == 2) {
                    prevod = 0;
                    
                    lcd.clear();

                    privilegovaneMenu();

                    /* skok, aby běžící text se začal zobrazovat od začátku */
                    skok = millis() - KOEFICIENT;

                    stav = 2;
                }
            }
            break;

        case 0xA1:
            /* po 2 sekundách vyčistí druhý řádek */
            if (rozdilCasu(minuly, millis()) > 2000) {
                lcd.setCursor(0, 1);
                lcd.write("                ");
            }

            /* po 3 vteřinách dovolí další sthnutí */
            if (rozdilCasu(minuly, millis() > 3000)) {
                
                if (ctiKartu(1, 0, &vystupni, &vystupniKarta)) {
                    stav++;
                }
                
            }

            /* jestliže je stisknuta klávesa zpět, odejdi do výběru akcí */
            if (ctiKlavesu(stiskla)) {
                if (*stiskla == 12) {
                    prevod = 0;
                    vstupni = 0;
                    vystupni = 0;

                    stav = 2;

                    lcd.clear();
                    privilegovaneMenu();
                    skok = millis() - KOEFICIENT;
                }
            }

            break;

        case 0xA2:
            if (jePrivilegovana(vystupniKarta)) {
                minuly = millis();
                stav--;
                return;
            }
            
            if (provedKontrolu(vystupni, prevod)) {
                stav++;
            } else {
                lcd.setCursor(0, 1);
                lcd.write(" NEDOSTATEK \x07\x24  ");
                delay(1000);
                minuly = millis();
                stav--;
            }

        case 0xA3:
            /* vlastní vzýšení stavu účtu nalezené karty */
            if (zapisNaKartu(1, 1, (vystupni + prevod), vystupniKarta)) {
                stav = 0xA1;

                uspesnyPrivilegovanyPrevod();
                minuly = millis();
            }

            /* jestliže je stisknuta klávesa zpět, odejdi do výběru akcí */
            if (ctiKlavesu(stiskla)) {
                if (*stiskla == 12) {
                    prevod = 0;
                    vstupni = 0;
                    vystupni = 0;

                    stav = 2;

                    lcd.clear();
                    privilegovaneMenu();
                    skok = millis() - KOEFICIENT;
                }
            }
            break;

        case 0xEE:
            /* zatím nefunguje */
            stav = 2;
            break;

        case 0xF0:
            reseniProblemu();
            stav++;
            break;

        case 0xF1:
            if (ctiKartu(1, 0, &vystupni, &vystupniKarta)) {
                nevyjimejteKartu();
                delay(150);

                prevod = ziskejData(dalsi(pozice));
            
                stav++;
            }
            break;

        case 0xF2:
            if (zapisNaKartu(1, 1, (vystupni + prevod), vystupniKarta)) {
                zapisData(pozice, 0);
                pozice = dalsi(pozice);

                stav = 0;
            }
            break;

    } // switch (stav)  
} // void loop()


/*
 * ============================================================================
 * 
 * FUNKCE PRACUJÍCÍ S ČTEČKAMI
 * 
 */


byte zapisNaKartu (byte index, byte indexKlice, unsigned int penize,
                   MFRC522::Uid karta) {
  #if SERIAL
    Serial.print("Zahajuji zápis na ");
    Serial.print(index + 1);
    Serial.println(". kartu ...");
  #endif  
  
  byte sektor       = SEKTOR;
  byte adresaBloku  = ADRESA_BLOKU;
  byte konecnyBlok  = KONTROLNI_BLOK;

  MFRC522::StatusCode status;

  byte bufferATQA[4];
  byte bufferSize = sizeof(bufferATQA);
  
  byte buffer[18];
  byte size = sizeof(buffer);


  status = ctecky[index].PICC_WakeupA(bufferATQA, &bufferSize);
  if (status != MFRC522::STATUS_OK) {
    #if SERIAL
      Serial.print("PICC_WakeupA() selhala: ");
      Serial.println(ctecky[index].GetStatusCodeName(status));
    #endif
    return 0;
  }

  status = ctecky[index].PICC_Select(&karta);
  if (status != MFRC522::STATUS_OK) {
    #if SERIAL
      Serial.print("PICC_Select() selhala: ");
      Serial.println(ctecky[index].GetStatusCodeName(status));
    #endif
    return 0;
  }

  /*
  if ( ! ctecky[index].PICC_ReadCardSerial() ) {
    #if SERIAL
      Serial.println("Seriová identifikace karty nelze přečíst!");
    #endif
    return 0;
  } */
  
  // prevod int -> pole
  byte polePenez[18];
  prevodNaPole(penize, polePenez);
  for (byte i = 0; i < 14; i++) {
    polePenez[i + 2] = 0x00;
  }
  
  // autorizace
  if (indexKlice == 0) {
    status = (MFRC522::StatusCode) ctecky[index].PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_B, konecnyBlok, &klic, &(ctecky[index].uid));
  } else if (indexKlice == 1) {
    status = (MFRC522::StatusCode) ctecky[index].PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_B, konecnyBlok, &klic, &(ctecky[index].uid));
  }

  if (status != MFRC522::STATUS_OK) {
    #if SERIAL
      Serial.print(F("PCD_Authenticate() selhala: "));
      Serial.println(ctecky[index].GetStatusCodeName(status));
    #endif
    
    ctecky[index].PICC_HaltA();
    ctecky[index].PCD_StopCrypto1();
    return 0;
  }

  // vlastní zápis
  #if SERIAL
    Serial.print(F("Probíhá vlastní zápis na kartu: "));
  #endif 

  status = (MFRC522::StatusCode) ctecky[index].MIFARE_Write(adresaBloku, polePenez, 16);
  if (status != MFRC522::STATUS_OK) {
    #if SERIAL
      Serial.print(F("MIFARE_Write() selhala: "));
      Serial.println(ctecky[index].GetStatusCodeName(status));
    #endif

    ctecky[index].PICC_HaltA();
    ctecky[index].PCD_StopCrypto1();
    return 0;
  }
  #if SERIAL
    Serial.println(F("HOTOVO!"));
    Serial.println();
  #endif

  // kontrolní čtení
  #if SERIAL
    Serial.print("Začíná kontrolní čtení: ");
  #endif
  status = (MFRC522::StatusCode) ctecky[index].MIFARE_Read(adresaBloku, buffer, &size);
  if (status != MFRC522::STATUS_OK) {
    #if SERIAL
      Serial.print(F("MIFARE_Read() selhala: "));
      Serial.println(ctecky[index].GetStatusCodeName(status));
    #endif

    ctecky[index].PICC_HaltA();
    ctecky[index].PCD_StopCrypto1();
    return 0;
  }
  
  #if SERIAL
    Serial.println(F("HOTOVO!"));
    //tiskni_byte_pole(buffer, 16);
    Serial.println();
  #endif

  // kontrola zápisu (porovnání zapisovaného a přečteného)
  #if SERIAL
    Serial.println(F("Kontroluji výsledky zápisu ...")  );
  #endif 

  // ukončení komunikace přes čtečku
  ctecky[index].PICC_HaltA();
  ctecky[index].PCD_StopCrypto1();

  byte pocet = 0;
  for (byte i = 0; i < VELIKOST_DATOVEHO_TYPU; i++) {
    // porovnání polí
    if (buffer[i] == polePenez[i])
      pocet++;
  }
  
  if (pocet == 2) {
    #if SERIAL
      Serial.println(F("Zápis poběhl úplně v pořádku. "));
    #endif
    return 1;
  } else {
    #if SERIAL
      Serial.print("Kontrola shledala neúplnou shodu (shodných: ");
      Serial.print(pocet);
      Serial.println(F(")."));
    #endif
    return 0;
  }
}


byte ctiKartu (byte index, byte indexKlice, unsigned int *penize,
               MFRC522::Uid *karta) {
  /* funkce zajišťující čtení karet, jak identifikačního čísla, tak obsahu */
  unsigned int cas;

  if ( ! ctecky[index].PICC_IsNewCardPresent() ) {
    Serial.println("odchazim");
    return 0;
  }

  if ( ! ctecky[index].PICC_ReadCardSerial() ) {
    Serial.println("noserial");
    return 0;
  }

  /* kopíruje identifikační údaje z přečtené karty */
  karta->size = ctecky[index].uid.size;
  for (byte i = 0; i < karta->size; i++) {
    karta->uidByte[i] = ctecky[index].uid.uidByte[i];
  }
  karta->sak = ctecky[index].uid.sak;

  byte sektor        = SEKTOR;
  byte adresaBloku   = ADRESA_BLOKU;
  byte konecnyBlok   = KONTROLNI_BLOK;
  MFRC522::StatusCode status;
  byte buffer[18];
  byte size = sizeof(buffer);

  if (indexKlice == 0) {
    status = ctecky[index].PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, konecnyBlok, &klic, &(ctecky[index].uid));
  } else if (indexKlice == 1) {
    status = ctecky[index].PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_B, konecnyBlok, &klic, &(ctecky[index].uid));    
  } else {
    return 0;
  }
  
  if (status != MFRC522::STATUS_OK) {
    #if DEBUG
      Serial.print(F("PCD_Authenticate() selhala: "));
      Serial.println(ctecky[index].GetStatusCodeName(status));
    #endif

    ctecky[index].PICC_HaltA();
    ctecky[index].PCD_StopCrypto1();
    return 0;
  }

  status = ctecky[index].MIFARE_Read(adresaBloku, buffer, &size);
  if (status != MFRC522::STATUS_OK) {
    #if DEBUG
      Serial.print(F("MIFARE_Read() selhala: "));
      Serial.println(ctecky[index].GetStatusCodeName(status));
    #endif

    ctecky[index].PICC_HaltA();
    ctecky[index].PCD_StopCrypto1();
    return 0;
  }

  ctecky[index].PICC_HaltA();
  ctecky[index].PCD_StopCrypto1();

  prevodNaCislo(buffer, penize);

  #if 0 // bylo dříve #if SERIAL
    Serial.print("Obsah přečteného bloku: ");
    //tiskni_byte_pole(buffer, size);
    Serial.println();
  #endif


  #if DEBUG
    Serial.println(F("    ctiNovouKartu() čtení proběhlo v pořádku "));
  #endif

  return 1;
}


/*
 * ============================================================================
 * 
 * POMOCNÉ FUNKCE
 * 
 */


byte ctiKlavesu(byte *stiskla) {
  byte docasny = 0;
  
  *stiskla = 0;
  
  if (rozdilCasu(aktualni, millis()) < DEBOUNCE) {
    return 0;
  }

  for (byte i = 0; i < 4; i++) {
    digitalWrite(PulzniPiny[i], HIGH);
    for (byte j = 0; j < 3; j++) {     
      if (digitalRead(CteciPiny[j]) == HIGH)
        docasny = Map[i][j];
    }

    digitalWrite(PulzniPiny[i], LOW);
  }

  if (docasny != 0) {
    #if DEBUG
      Serial.print("Stisknuta klavesa ");
      Serial.println(docasny);
    #endif
    
    aktualni = millis();
    *stiskla = docasny;
    return 1;
  }

  return 0;
} // ctiKlavesu(byte *stiskla)


byte provedKontrolu(unsigned int penize, unsigned int presun) {
  if (presun <= penize) {
    return 1;
  }
  else {
    return 0;
  }
}


byte kartaSeZpravou(MFRC522::Uid karta) {
    for (byte i = 0; i < karta.size; i++) {
        if (karta.uidByte[i] != UidKartySeZpravou[i])
            return 0;
    }

    return 1;
}


byte jePrivilegovana(MFRC522::Uid karta) {
    Serial.println("Zahajuji ověření");
    byte nalezena = 1;

    for (byte i = 0; i < POCET_PRIVILEGOVANYCH_KARET; i++) {
        /* postupně projdi jednotlivé části identifikace */
        for (byte j = 0; j < karta.size; j++) {
            /* jestliže se údaje nerovnají, stáhni ,, vlaku '' */
            if ((nalezena) && (karta.uidByte[j] != privilegovaneKarty[i][j])) {
                nalezena = 0;
                continue;
            }
        }

        /* když je vlajka stále vytažená */
        if (nalezena == 1) {
            Serial.println("uspěšný konec ověření");
            return 1; }
        /* jinak znovu vytáhni vlajku, pro další klíč */
        else
            nalezena = 1;
    }

    Serial.println(" konec ověření");
    return 0;
}


void prevodNaCislo(byte *pole, unsigned int *cislo) {
  *cislo = pole[0] | (pole[1]  << 8);
}


void prevodNaPole(unsigned int cislo, byte *pole) {
  *pole       = cislo & 0xFF;
  *(pole + 1) = cislo >> 8;
}


unsigned long rozdilCasu(unsigned long minuly, unsigned long aktualni) {
  if (minuly > aktualni) {
    return (4294967295 - minuly + aktualni + 1);
  } else {
    return (aktualni - minuly);
  }
}


/*
void tiskni_byte_pole(byte *pole, byte velikost) {
  for (byte i = 0; i < velikost; i++) {
    Serial.print(pole[i] < 0x10 ? " 0" : " ");
    Serial.print(pole[i], HEX);
  }
} */


/*
 * ============================================================================
 *
 *  FUNKCE VYPISUJÍCÍ NA LCD DISPLAY
 *
 */


void beziciInformace() {
  char *zprava = "Pro odstran#n# posledn# ##slice stiskn#te ##,  pro potvrzen# stiskn#te ##,  pro ukon#en# dr#te ##;  ";
  byte posun;

  if (rozdilCasu(minuly, millis()) < RYCHLOST_POSUNU) {
    return;
  }
  else
    minuly = millis();

  lcd.setCursor(0, 1);
  posun = (( (millis() - skok) / RYCHLOST_POSUNU) % 100);
  
  for (byte i = 0; i < 16; i++) {
    switch (posun + i) {
      case 11:
      case 111:
        lcd.write(byte(3));
        break;
      case 13:
      case 113:
        lcd.write(byte(6));
        break;
      case 22:
        lcd.write(byte(6));
        break;
      case 24:
        lcd.write(byte(1));
        break;
      case 25:
        lcd.write(byte(6));
        break;
      case 38:
        lcd.write(byte(3));
        break;
      case 42: 
        lcd.write(byte(4));
        break;
      case 43: 
         lcd.write(byte(5));
        break;
      case 59: 
        lcd.write(byte(6));
        break;
      case 67:
        lcd.write(byte(3));
        break;
      case 71:
        lcd.write('\x7e');  // šipka doprava
        break;
      case 72:
        lcd.write('\xa5');  // kolečko
        break;
      case 84:
        lcd.write(byte(1));
        break; 
      case 87:
        lcd.write(byte(6));
        break;
      case 91:
        lcd.write(byte(0));
        break;
      case 95:
        lcd.write(byte(4));
        break;
      case 96: 
        lcd.write(byte(5));
        break;
        
      default:
        if (posun + i > 99) 
          lcd.write(zprava[posun + i - 100]);
        else 
          lcd.write(zprava[posun + i]);
        break;
    }  // switch (posun + i)
  }  // for (byte i = 0; ...
}  // void beziciInformace()

 
void beziciMoznosti() {
  char zprava[] = "Pro p#evod stiskn#te 1, pro odhl##en# 9;  ";
  byte posun;

  if (rozdilCasu(minuly, millis()) < RYCHLOST_POSUNU) 
    return;
  else
    minuly = millis();

  lcd.setCursor(0, 1);
  posun = (( (millis() - skok) / RYCHLOST_POSUNU) % 42);

  for (byte i = 0; i < 16; i++) {
    switch (posun + i) {
      case 5:
      case 47:
        lcd.write(byte(2));
        break;
      case 17:
        lcd.write(byte(3));
        break;
      case 32:
        lcd.write(byte(4));
        break;
      case 33:
        lcd.write(byte(5));
        break;
      case 36:
        lcd.write(byte(6));
        break;
      default:
        if (posun + i > 41)
          lcd.write(zprava[posun + i - 42]);
        else 
          lcd.write(zprava[posun + i]);
        break;
    } // switch (posun + i)
  }  // for (byte i = 0; ...
  //lcd.write(radek);

}  // void beziciMoznosti()


byte zadejMnozstvi(unsigned int *prevod, byte *stiskla) {
  // Funkce ošetřující zadání a zobrazení množství peněz pro převod
  char radek[12];

  /* příprava znaků */
  lcd.createChar(0, HacekZ);
  lcd.createChar(1, HacekC); 
  lcd.createChar(2, HacekR); 
  lcd.createChar(3, HacekE); 
  lcd.createChar(4, PraveTlacitko0);
  lcd.createChar(5, PraveTlacitko1);
  lcd.createChar(6, CarkaI); 

  if (ctiKlavesu(stiskla)) {
    if (*stiskla > 0 && *stiskla < 10) {
      /* kontrola překročení limitů datového typu */
      if ((*prevod > 6553) || (*prevod > 6552 && *stiskla > 5)) {
        #if DEBUG
          Serial.println("zadejMnozstvi() nelze provést, překročení limitů [1]");
        #endif
        lcd.setCursor(0, 1);
        lcd.write(CHYBA_LIMITU);
        delay(2000);
        return 0;
      }

      // vynásobí deseti a přičte zvolený počet
      *prevod = *prevod * 10 + *stiskla;     
    }
    else if (*stiskla == 10) {
      /* kontrola překročení limitu datového typu */
      if (*prevod > 6553) {
        #if DEBUG
          Serial.println("zadejMnostvi() nelze provést, překoročení limitů [0]");
        #endif
        lcd.setCursor(0, 1);
        lcd.write(CHYBA_LIMITU);
        delay(2000);
        return 0;
      }

      // vynásobí deseti
      *prevod *= 10;           // *prevod = *prevod * 10
    }
    
    else if (*stiskla == 11) {
      return 1;
    }
    else if (*stiskla == 12 && *prevod == 0) {
      return 2;
    }
    else if (*stiskla == 12) {
      // dělení desíti, desetinná část je zahozena pryč
      *prevod /= 10;          // *prevod = *prevod / 10
    }
  } // if (ctiKlavesu(stiskla))
  
  lcd.home();
  sprintf(radek, "evod: %5u \x07\x24", *prevod);
  lcd.write('P');
  lcd.write(byte(2));
  lcd.write(radek);
  
  return 0;
} // byte zadejMnozstvi(unsigned int *penize, byte *stiskla)


void stavKonta(unsigned int penize) {
  char radek[17];

  /* příprava znaků */
  lcd.createChar(0, CarkaU);
  lcd.createChar(1, HacekC);
  lcd.createChar(2, HacekR);
  lcd.createChar(3, HacekE);
  lcd.createChar(4, CarkaA);
  lcd.createChar(5, HacekS);
  lcd.createChar(6, CarkaI);
  
  
  lcd.home();
  switch ((int) log10(penize)) {
    case 0:
    case 1:
      sprintf(radek, "tu: %2d \x07\x24P", penize);
      lcd.write("Stav ");
      lcd.write(byte(0));
      lcd.write(byte(1));
      lcd.write(radek);
      break;
    case 2:
    case 3:
    case 4:
      sprintf(radek, "Finance: %4d \x07\x24", penize);
      lcd.write(radek);
      break;   
  }
}

void vlozteKartu(byte index) {
    /* vypíše 
        " A R A S A K A "
        "Vložte kartu >#"
     * nebo pro index == 1
        " A R A S A K A "
        "Vložte 2. kartu"
    */

    /* příprava znaků */
    lcd.createChar(0, HacekZ);

    lcd.clear();
    lcd.home();

    lcd.write(" A R A S A K A");
    lcd.setCursor(0, 1);

    lcd.write("Vlo");
    lcd.write(byte(0));
    lcd.write("te ");

    if (index == 0) {
        lcd.write("kartu \xdb\x7e");
    }
    else {
        lcd.write("2. kartu");
    }
} // void vlozteKartu(byte index)


void nevyjimejteKartu() {
    /* vypíše
        "Načítání údajů "
        "nevyjímejte #  "
     */
    lcd.createChar(0, HacekC);
    lcd.createChar(1, CarkaI);
    lcd.createChar(2, CarkaA);
    lcd.createChar(3, CarkaU);
    lcd.createChar(4, KrouzekU);

    lcd.clear();
    lcd.home();

    lcd.write("Na");
    lcd.write(byte(0));
    lcd.write(byte(1));
    lcd.write('t');
    
    lcd.write("\x02n\x01 \x03\daj\x04");

    lcd.setCursor(0, 1);

    lcd.write("nevyj\x01mejte \xdb");
}


void potvrzeniPresunu() {
    /* vypíše jestli je částka 4 místná (a míň)
        "Potvrďte ???? €$"
        " ANO >° X NE ## "
     * jinak vypíše
        "Platím: 12345 €$"
        " ANO >° X NE ## 
     */    
    char radek[17];

    /* příprava znaku */
    lcd.createChar(0, HacekD);
    lcd.createChar(1, CarkaI);

    lcd.clear();
    lcd.home();
    
    if (log10(prevod) > 4) {
      sprintf(radek, "m: %5u \x07\x24", prevod);
      
      lcd.write("Plat");
      lcd.write(byte(1));
      lcd.write(radek);      
    } else {
      sprintf(radek, "te %4u \x07\x24", prevod);

      lcd.write("Potvr");
      lcd.write(byte(0));
      lcd.write(radek);
    }
   
    lcd.setCursor(0, 1);

    lcd.write(" ANO \x7e\xa5 X NE \x04\x05");
}


void nemanipulujte() {
    /* vypíše
        " NEMANIPULUJTE  "
        "   S KARTAMI    " 
     */

    lcd.clear();
    lcd.home();

    lcd.write(" NEMANIPULUJTE  ");

    lcd.setCursor(0, 1);

    lcd.write("   S KARTAMI    ");
}


void ukoncujiPrevod() {
    /* vypíše
        "Ukončuji převod "
        "vračím se k účtu"
     */

    /* obnova znaků pro stavKonta() a posouvající se text */
    lcd.createChar(0, CarkaU);
    lcd.createChar(1, HacekC);
    lcd.createChar(2, HacekR); 
    lcd.createChar(3, HacekE); 
    lcd.createChar(4, CarkaA);
    lcd.createChar(6, CarkaI);

    lcd.clear();
    lcd.home();

    lcd.write("Ukon\x01uji p");
    lcd.write(byte(2));
    lcd.write("evod");
    lcd.setCursor(0, 1);
    lcd.write("vrac\x06m se k ");
    lcd.write(byte(0));
    lcd.write("\x01tu");
}


void odhlasuji() {
  /* vypíše 
      "   Odhlašuji    "
      "                "
   */

   lcd.createChar(5, HacekS);

   lcd.clear();
   lcd.home();

   lcd.write("   Odhla\x05uji    ");
}


void nedostatekFinanci() {
    /* vypíše
        " Bohužel nemáte "
        "dostatek financí"
     */

    /* příprava znaků */
    lcd.createChar(0, HacekZ);
    lcd.createChar(4, CarkaA);
    lcd.createChar(6, CarkaI);

    lcd.clear();
    lcd.home();

    lcd.write(" Bohu");
    lcd.write(byte(0));
    lcd.write("el nem\x04te ");

    lcd.setCursor(0, 1);
    lcd.write("dostatek financ\x06");
}


void uspesnyPrubeh() {
    /* vypíše
        "Převod proběhl"
        "   úspěšně !  "
     */

    lcd.createChar(0, CarkaU);
    lcd.createChar(2, HacekR);
    lcd.createChar(3, HacekE);
    lcd.createChar(5, HacekS);

    lcd.clear();
    lcd.home();

    lcd.write('P');
    lcd.write(byte(2));
    lcd.write("evod prob\x03hl");

    lcd.setCursor(0, 1);

    lcd.write("   ");
    lcd.write(byte(0));
    lcd.write("sp\x03\x05n\x03 !  ");
}


void reseniProblemu() {
    /* vypíše
        " Nastal problém "
        " Vložte 2. kartu"
     */
  lcd.createChar(0, CarkaE);
  lcd.createChar(1, HacekZ);

  lcd.clear();
  lcd.home();

  lcd.write(" Nastal probl");
  lcd.write(byte(0));
  lcd.write("m");

  lcd.setCursor(0, 1);

  lcd.write("Vlo");
  lcd.write(byte(1));
  lcd.write("te 2. kartu");
}

/* PRIVILEGOVANÉ ZPRÁVY */
void beziciPrivilegovaneMoznosti() {
  char zprava[] = "p#evod 1, sn##en# 5, odhl##en# 9;  ";
  byte posun;

  if (rozdilCasu(minuly, millis()) < RYCHLOST_POSUNU) 
    return;
  else
    minuly = millis();

  lcd.setCursor(0, 1);
  posun = (( (millis() - skok) / RYCHLOST_POSUNU) % 35);

  for (byte i = 0; i < 16; i++) {
    switch (posun + i) {
      case 1:
      case 36: // ř
        lcd.write(byte(2));
        break;
      case 12: // í
      case 47:
        lcd.write(byte(6));
        break;
      case 13: // ž
      case 48:
        lcd.write(byte(0));
        break;
      case 16: // í
        lcd.write(byte(6));
        break;
      case 25: // á
        lcd.write(byte(4));
        break;
      case 26: // š
        lcd.write(byte(5));
        break;
      case 29: // í
        lcd.write(byte(6));
        break; 
      default:
        if (posun + i > 34)
          lcd.write(zprava[posun + i - 35]);
        else 
          lcd.write(zprava[posun + i]);
        break;
    } // switch (posun + i)
  }  // for (byte i = 0; ...
  //lcd.write(radek);

} //void beziciPrivilegovaneMoznosti()


void privilegovaneMenu() {
    /* vypíše
        "[P]  stav: ~ €$ "
        "                "
     */

    lcd.clear();
    lcd.home();
    lcd.write("\x5BP\x5D  stav\x3A \xF3 \x07\x24");

    lcd.createChar(0, HacekZ);
    lcd.createChar(2, HacekR);
    lcd.createChar(4, CarkaA);
    lcd.createChar(5, HacekS);
    lcd.createChar(6, CarkaI);
}


void uspesnyPrivilegovanyPrevod() {
    /* vypíše
        "                "
        "   stržení OK   "
     */
     
    lcd.createChar(0, HacekZ);
    lcd.createChar(6, CarkaI);
    
    lcd.setCursor(0, 1);

    lcd.write("   str");
    lcd.write(byte(0));
    lcd.write("en\x06 OK   "); 
}

void sniz(unsigned int prevod) {
    /* vypíše
        "srážím: 12345 €$"
        "                "
     */

    char radek[17];

    lcd.createChar(0, HacekZ);
    lcd.createChar(4, CarkaA);
    lcd.createChar(6, CarkaI);
    
    lcd.home();

    sprintf(radek, "\x06m: %5u \x07\x24", prevod);

    lcd.write("sr\x04");
    lcd.write(byte(0));
    lcd.write(radek);
}
/*
 * ============================================================================
 * 
 * FUNKCE PRO PRÁCI S EEPROM
 * 
 */


int najdiPoziciEEPROM() {
  for (int index = 0; (index + 1) < EEPROM.length(); index += 2) {
    if (ziskejData(index) != 0) {
      return index; 
    }
  }

  return 0;
}

/* 
 *  dříve dalsiPozice()
 *  přejmenováno z důvodu lepší čitelnosti
 *    ,, pozice = dalsi(pozice); ''
 *  oproti
 *    ,, pozice = dalsiPozice(pozice);
 */
int dalsi(int pozice) {
  if ((pozice + 2) >= EEPROM.length()) {
    return 0;
  }
  else {
    return (pozice + 2);
  }
}

int predchoziPozice(int pozice) {
  if (pozice == 0) {
    return (EEPROM.length() - 2);
  }
  else {
    return (pozice - 2);
  }
}


byte nastalProblem() {
  /* dalsiPozice() ~ dalsi()*/
  int pozice = dalsi(najdiPoziciEEPROM());

  if (ziskejData(pozice) != 0) {
    return 1;
  }
  else {
    return 0;
  }
}

unsigned int ziskejData(int pozice) {
  unsigned int penize = 0;
  byte data[2];

  /* čtení z EEPROM */
  data[0] = EEPROM.read(pozice);
  data[1] = EEPROM.read(pozice + 1);

  prevodNaCislo(data, &penize);

  return penize;
}

void zapisData(int pozice, unsigned int penize) {
  byte data[2]= {0x00, 0x00};

  prevodNaPole(penize, data);

  EEPROM.write(pozice    , data[0]);
  EEPROM.write(pozice + 1, data[1]);
  
}
