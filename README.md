# Upush Chat Service
Dette var min eksamen for IN2140 - Operativsystemer og Datakommunikasjon. Oppgaven gikk ut på å implementere sin egen stop and wait protocol på applikasjonslaget. Alle meldinger blir send over UDP og serveren og clientene sender de nødvendige ACKs for å forsikre seg om at meldingene kommer frem.

## Kjøring av programmet

```
$ make
// kjoerer upush_server med valgrind
$ make val_client_server // lossless
// kjoerer upush_client med valgrind med nick joe/moe/roe
$ make val_client_joe
$ make val_client_moe
$ make val_client_roe
```

eller

```
$ make
// kjoerer upush_server med valgrind
$ make val_client_server // 20% loss rate
// kjoerer upush_client med valgrind med nick joe/moe/roe og 20% loss rate
$ make val_client_joe_lossy
$ make val_client_moe_lossy
$ make val_client_roe_lossy
```

Jeg vil anbefale å endre VERBOSE til 1 i upush_helper.h for å kunne lettere se hva som blir sendt. Man bør kjøre "make clean" og "make" etter å ha endret denne for å garantere at alle filer har blitt rekompilert med endringen.

## Antagelser

Jeg har valgt å gi mer enn ett forsøk til registreringsmeldingen. Dette er fordi det ellers er veldig sannsynlig en klient ikke får en ACK på sin registrering og gir opp med én gang. Med for eksempel 20% loss rate er det 36% sannsynlighet at enten klientens registrering eller serverens ACK går tapt.

Jeg har antok lenge at man ikke kan bruke recvfrom hos klientene for å finne ip-en til andre klienter. Denne antagelsen gjorde jeg på grunn av hendelsesflyten i eksempelscenarioet i oppgaveteksten: Her mottar Alice en pakke fra Bob (10), men hun svarer ikke Bob med en ACK med én gang, selvom hun i teorien kunne brukt recvfrom og fått IP-en med én gang. I stedet sender hun en lookup til serveren (11) før hun svarer med en ACK til Bob (13). I beskrivelsen over står det eksplisitt: "[Alice] er  i stand til å svare [Bob] etter nytt oppslag - denne gangen med suksess". Dette har implikasjonen at hun ikke var i stand til å svare før dette, altså at hun ikke kan bruke recvfrom for å få addressen til Bob. Denne antagelsen førte til to ting: at klienter gjorde lookup når de skulle svare med en ACK dersom de ikke kunne addressen, og at hver klient har en global PKT_nr, altså at meldinger til ulike klienter aldri har samme pakkenummer. Det sistnevnte fordi man uten å kunne bruke recvfrom for å få IP-addressen til en ACK ikke har noen måte å vite hvem som sendte den. Dette virker rart med tanke på en praktisk implementasjon av stop-and-wait, men det er slik jeg tolka oppgaveteksten og eksempelscenarioet som beskrevet. Det gjorde det også vanskelig å svare med en ACK WRONG FORMAT fordi man ikke nødvendigvis hadde en velformatert nick og gjøre lookup på. I det hele tatt skapte det en del problemer. Jeg mener eksempelscenarioet burde vært konstruert mer entydig slik at man unngikk å måtte gjøre slike antagelser. Senere har jeg fått vite at det er greit å bruke recvfrom i klientene. Jeg har valgt å bare implementere dette for sending av ACKs, slik at cachen ikke blir oppdatert av informasjonen man får fra recvfrom. Dette er slik jeg forstår det i tråd med intensjonen i oppgaven, men jeg syntes uansett eksamen er dårlig formulert. Jeg har ikke endra på det med å ha globale pakkenummer. Dette vil aldri være et problem med mindre man har 256 meldinger in flight uansett. Dersom det er andre ting som virker ulogisk med implementasjonen, tenk på om det ikke var mulig å sende ACKs uten lookup og tenk om det hadde vært logisk da.

Jeg har også valgt å seede med srand48 i main. Dette er fordi drand48 uten seed vil gi svært lave verdier de første gangene. En klient med 20% loss rate vil miste sine første fire pakker hver gang. Jeg seeder srand48 med time(NULL).

Jeg har bare implementert IPv4 funksjonalitet ettersom det er det eneste som er på IFI sitt nettverk.

Jeg har valgt å bruke en separat timeout i select som gjør at server og klient kan gjøre ting hvert 'tick' istedenfor bare om man leser fra stdin eller sock_fd. Denne har jeg satt til 60 ticks per sekund. Dette gjør programmet mer responsivt og gjør at vi kan gjøre flere ting "samtidig" med kun én tråd. En konsekvens av dette er at jeg ikke har brukt noen global tidsverdi når jeg sjekker hvor lenge en melding har ligget og ventet på en ACK, jeg teller bare antall ticks. Dette kan være unøyaktig, ettersom et tick ikke nødvendigvis alltid er 1/60 av et sekund. For eksempel vil en lesing av stdin eller sock_fd alltid hoppe ut av selectløkka før ett tick er gått, samtidig som tiden tar å eksekvere det som skjer i selectløkka tar tid, som gjør at et tick vil bli lengre. Jeg har valgt å ignorere dette ettersom de fleste tick sannsynligvis er sånn ca. 1/60 av et sekund ettersom det som oftest ikke skjer noe som helst hvert tick. De funksjonene som blir kalt er også ikke ekstremt lange, og vi koder jo i C som er legendarisk raskt så det går nok bra.

Andre antagelser vil være beskrevet i kommentarene i koden.
