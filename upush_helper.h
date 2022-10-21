#ifndef UPUSH_HELPER_H
#define UPUSH_HELPER_H

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <netdb.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <limits.h>

// define denne til 1 for aa faa debug prints
#define VERBOSE 0

// -------------------- DEFINES --------------
/* Ulike verdier for type:
 * - 0: registreringsmelding (PKT nummer REG from_nick)
 * - 1: affirmative ack (ACK nummer OK)
 * - 2: lookup (PKT nummer LOOKUP to_nick)
 * - 3: mislykka lookup (ACK nummer NOT FOUND)
 * - 4: vellykka lookup (ACK nummer NICK to_nick adresse PORT port)
 * - 5: tekstmelding (PKT nummer FROM from_nick TO to_nick MSG tekst)
 * - 6: wrong name (ACK nummer WRONG NAME)
 * - 7: wrong format (ACK nummer WRONG FORMAT)
 * - 8: tekstmelding 2 (samme som vanlig men skal ikke forsoeke ny lookup)
 */
#define MSG_REGISTER 0
#define MSG_AFFIRM_ACK 1
#define MSG_LOOKUP 2
#define MSG_LOOKUP_FAIL 3
#define MSG_LOOKUP_OK 4
// tekstmelding av type 1
#define MSG_TEXT 5
#define MSG_WR_NAME 6
#define MSG_WR_FORM 7
// brukes for aa skille mellom foerste forsoek paa aa sende en melding
// og forsoeket vi gjoer etter vi har gjort en ny lookup
#define MSG_TEXT_2 8 

// deconstruct_message returns:
#define DECON_SUCCESS 0
#define DECON_WR_FORM 1
#define DECON_CRIT_FAIL -1

// send_active_msg returns
#define ACTIVE_MSG_SENT 0
#define ACTIVE_NO_MSG 1
#define ACTIVE_MSG_EXPIRED 2
#define ACTIVE_SERV_NO_RESPONSE 3

// Constants
#define ONE_MILLION 1000000

// localhost
#define LOCALHOST "127.0.0.1"

/* Maksimal lengde paa en melding er ca 1462, men vi runder opp til 1500
 * fordi aa finne en eksakt mengde hadde bare gjort det mer sannsynlig aa faa
 * feil og er ikke saa meningsfylt
 */
#define BUFSIZE 1500

// --------------- STRUCTS -------------------

// klient for server eller klienters liste
struct Client {
	char nick[21];
	struct sockaddr_in* addr;
	unsigned char last_pkt;
	// brukes bare av server
	long ticks_to_live;
	// message head. malloca
	struct Node *messages;
	// meldingen som ligger aa venter paa ack i henhold til stop
	// and wait
	struct Message *active_msg;
	// brukes for aa sjekke hvor lenge man har venta paa ACK
	unsigned long pkt_wait;
};

// node for generisk lenkeliste
struct Node {
	void *ptr;
	// en void-peker som vi caster avhengig av typen
	struct Node *next;
};

struct Message {
	/* Ulike verdier for type:
	 * - 0: registreringsmelding (PKT nummer REG nick)
	 * - 1: affirmative ack (ACK nummer OK)
	 * - 2: lookup (PKT nummer LOOKUP nick)
	 * - 3: mislykka lookup (ACK nummer NOT FOUND)
	 * - 4: vellykka lookup (ACK nummer NICK nick adresse PORT port)
	 * - 5: tekstmelding (PKT nummer FROM fra_nick TO to_nick MSG tekst)
	 * - 6: wrong name (ACK nummer WRONG NAME)
	 * - 7: wrong format (ACK nummer WRONG FORMAT)
	 */
	char type;

	unsigned char number;
	/* Dette er en noe ueffektiv maate aa lagre meldingene paa. Hver
	 * hver meldingsstruct har altsaa allokert 1400 bytes for text,
	 * selv om bare en (to) av meldingstypene faktisk har text i seg.
	 * Det burde aapenbart vaert en charpointer som peker paa et malloca
	 * omraede, slik at hver message bare hadde hatt en char pointer som
	 * ikke blir brukt, istedenfor en 1.4 kB med ubrukte chars. Selv
	 * for faktiske meldinger som blir sendt er det usannsynlig at de
	 * bruker mer en kanskje 100 av plassene i dette arrayet. Dette er 
	 * aapenbart ikke optimalt, men jeg lar det staa fordi jeg kun kom
	 * til aa tenke paa dette etter aa ha gjort ferdig hele eksamen.
	 * Om jeg hadde gjort oppgaven paa nytt eller hatt litt mer tid hadde
	 * jeg endret dette til en charpointer og malloca etter behov, men jeg
	 * lar det staa fordi jeg ikke har tid eller kapasitet til aa gaa gjennom
	 * alle stedene dette ville krevd en endring paa, og fiksa mulige minne-
	 * lekkasjer og lignende. Det skal sies at antallet meldinger
	 * som eksisterer i programmet paa en tid er relativt lite,
	 * som oftest under 10, saa minnebruken er ikke spesielt stor,
	 * men stoerre enn den kunne vaert om jeg brukte en malloca pointer
	 * istedenfor aa ha det som en del av structen. I minnelekasjer
	 * hadde dette vaert en mye stoerre problem, men saa vidt jeg vet
	 * har jeg ikke noen saerlige av disse. Naar det blir sendt over
	 * nettet har dette ingen betydning, jeg sender selvfoelgelig bare
	 * fram til nullpointern.
	 */
	char text[1401];
	/* det er viktig aa paapeke at for en gitt melding vil mange av disse
	 * vaere undefined, ettersom de ikke blir satt med mindre de maa. Det
	 * gaar bra saa lenge type alltid er satt saa man vet hvilken melding
	 * man har med aa gjoere
	 */
	char from_nick[21];
	char to_nick[21];
	// host byte order
	unsigned short port;
	// human readable xxx.xxx.xxx.xxx\0
	char ip[16];

	// dest_addr, brukes bare av avsender
	struct sockaddr_in dest_addr;

	// antall ganger en melding blir forsoekt sendt foer den kastes ut
	int attempts;
	// settes til 1 om den har blitt forsoekt sendt foer aa det ble lookup
	int looked_up;
	// pakken som foerte til en lookup. Brukes bare av lookup for aa putte
	// en verdi i last_pkt slik at client starter med en gyldig verdi der
	unsigned char triggering_pkt;
};

// --------------- FUNCTIONS -------------------

// sjekker og bruker perror for error checking
void check_error(int res, char* msg);
// wrapper for strtol, se upush_helper.c
int str_to_long(const char *str, long *longaddr);

// wrapper for strtof, se upush_helper.c
int str_to_float(const char *str, float *floataddr);

// lager og binder en socket
int create_and_bind_sock(unsigned short port);

// bruker str_to_long og sjekker om den er i range for en unsigned short
unsigned short str_to_port(char *str);

int check_nick(const char *nick);

int read_from_stdin(char *buf, int size);

// ----- lenkelistefunksjoner ------

struct Node *initialize(void);

// disse funskjonene er selvforklarende
void **append(struct Node *head);

void **prepend(struct Node **headptr);

void rm_first(struct Node **headptr);

void rm_element(struct Node *victim, struct Node *prev);

void print_blocked_nicks(struct Node *head);

void print_messages(struct Node *head);

void print_clients(struct Node *head);

struct Node *find_client(struct Node *head,char *nick, struct Node **prevaddr);

struct Node *find_client_by_pkt(struct Node *head,unsigned char number, struct Node **prevaddr);

void add_client(struct Node *clients, char *nick, struct sockaddr_in *addr, unsigned long client_max_ticks, unsigned char triggering_pkt);

void free_client_ptrs(struct Client *client);

// ----- meldingsfunksjoner ----

char *construct_message(struct Message *msg);

int deconstruct_message(char *str, struct Message **msgptr);

#endif /* UPUSH_HELPER_H */
