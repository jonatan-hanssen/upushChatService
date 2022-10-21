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
#include <errno.h>

#include "upush_helper.h"

void check_error(int res, char* msg) {
	if (res == -1) {
		perror(msg);
		exit(EXIT_FAILURE);
	}
}

/* str_to_long og str_to_float er mer eller mindre wrappers for strol og
 * strtof. Grunnen til at jeg foelte at det var noedvendig aa gjoere
 * dette er paa grunn av maaten disse funksjonene indikerer feil:
 * Returverdien til strtol og strtof er 0 ved mislykket konversjon,
 * men kan ogsaa vaere det ved inputverdien 0. ERRNO blir satt om vi er
 * out of range, men ikke om strengen ikke inneholder tall (eller
 * begynner med en bokstav). I dette tilfellet vil endptr og nptr
 * vaere like. Konklusjonen av alt dette er at vi ikke kan bruke
 * check_error for aa sjekke om strtol eller strtof var vellykkede,
 * saa jeg lager en wrapper som gjoer at vi kan gjoere dette, og som
 * gjoer at vi slipper en masse ekstra kode i upush_server og
 * upush_client
 */
int str_to_long(const char *str, long *longaddr) {
	errno = 0;
	char *endptr = NULL;
	long result;

	// strtol vil endre endptr til aa peke paa den foerste
	// ugyldige verdien
	result = strtol(str,&endptr,10);

	/* Det boer paapekes at strtol godkjenner 1234aaa og vil returnere
	 * 1234 i dette tilfellet. Jeg velger aa godta dette som gyldig input
	 * ogsaa.
	 */

	// dette vil gjoere at vi kan catche erroren med check_error
	if (errno) return -1;
	// dersom den foerste ugyldige verdien er den samme som den foerste
	// verdien har vi aapenbart mislykket i vaar konversjon.
	if (str == endptr) {
		/* Vi setter errno for aa kunne bruke check_error.
		 * Jeg var litt skeptisk til aa sette errno selv, men
		 * av det jeg har sett er det ikke noe direkte galt med 
		 * aa gjoere det
		 */
		errno = EINVAL;
		return -1;
	}

	*longaddr = result;
	return 0;
}

int str_to_float(const char *str, float *floataddr) {
	errno = 0;
	char *endptr = NULL;
	float result;

	// strtof vil endre endptr til aa peke paa den foerste
	// ugyldige verdien
	result = strtof(str,&endptr);

	// dette vil gjoere at vi kan catche erroren med check_error
	if (errno) return -1;
	// dersom den foerste ugyldige verdien er den samme som den foerste
	// verdien har vi aapenbart mislykket i vaar konversjon.
	if (str == endptr) {
		// vi setter errno for aa kunne bruke check_error
		errno = EINVAL;
		return -1;
	}

	*floataddr = result;
	return 0;
}

/* denne lager og binder en socket til localhost. Socketen
 * sin fildeskriptor blir returnert.
 */
int create_and_bind_sock(unsigned short port) {
	int fd,ret;

	fd = socket(AF_INET,SOCK_DGRAM,0);
	check_error(fd,"socket");

	// lager sockaddr som gjoer at vi lytter paa alle interfaces
	struct sockaddr_in my_addr;
	my_addr.sin_family = AF_INET;
	my_addr.sin_port = htons(port);
	// vi gjoer ingen errorchecking fordi vi vet at LOCALHOST er velformatert
	inet_pton(AF_INET,LOCALHOST,&my_addr.sin_addr.s_addr);

	ret = bind(fd,(struct sockaddr*) &my_addr, sizeof(struct sockaddr_in));
	check_error(ret,"bind");

	return fd;
}

// returnerer port i host byte order
unsigned short str_to_port(char *str) {
	int ret;
	// foerst som long fordi vi bruker strtol
	long port_long;

	ret = str_to_long(str,&port_long);
	check_error(ret,"str_to_long");

	// sjekker om den er i range. Vi sjekker ikke om den er en legal
	// port, dette gjoer bind()
	if (port_long < 0 || port_long > 65535) {
		fprintf(stderr,"error: port out of range of unsigned short\n");
		return EXIT_FAILURE;
	}

	return (unsigned short) port_long;
}

// ----------- generisk lenkeliste -------------

/* Min implementasjon av en generisk lenkeliste er som foelger:
 * Man lager en node head. Det vil alltid vaere en head (selvom lista
 * er tom). Dette har jeg gjort for aa unngaa aa maatte ha
 * en egen lenkelistestruct. Man legger til noder ved aa traversere
 * til slutten av lista, lage en ny node, og motta addressen til dennes ptr
 * slik at man kan kan endre den til aa peke paa det man vil. Naar man sletter 
 * noe gir man addressen til pointeren til head, slik at head kan endres.
 *
 * Riktig maate aa kalle denne funksjonen paa: struct Node *head = initialize()
 */
struct Node *initialize(void) {
	// lager og returnerer head
	struct Node *head;
	head = malloc(sizeof(struct Node));

	if (!head) {
		fprintf(stderr,"Malloc failed, aborting.\n");
		exit(EXIT_FAILURE);
	}
	head->next = NULL;
	head->ptr = NULL;
	return head;
}

/* Vi putter ting bakerst og tar ting vekk fra starten.
 * Dette gjoer jeg fordi jeg vil at aa hente elementet vi skal
 * sende skal vaere O(1). Meldinger blir bare putta paa lista naar vi venter
 * paa en ACK, saa da gaar det bra at vi maa traversere hele lista, men naar
 * vi foerst har mottatt denne vil vi ikke at det skal ta lang tid og finne
 * neste melding, saa vi bruker ikke pop() og push(), som ogsaa hadde gitt oss
 * en FIFO koe men med omvendt kompleksitet
 *
 * Riktig maate aa kalle denne funksjonen paa: *append(head) = ptr
 */
void **append(struct Node *head) {
	if (head->ptr == NULL) {
		return &(head->ptr);
	}
	// foerst bare traverserer vi til vi treffer slutten
	struct Node *cur;
	for (cur = head; cur->next != NULL; cur = cur->next);

	cur->next = malloc(sizeof(struct Node));

	if (!cur->next) {
		fprintf(stderr,"Malloc failed, aborting.\n");
		exit(EXIT_FAILURE);
	}

	cur->next->next = NULL;
	cur->next->ptr = NULL;
	return &(cur->next->ptr);
}

/* legger til noe forran. gir deg adressen til pointeren til nye head
 * og endrer head med headptr
 *
 * Riktig maate aa kalle denne funksjonen paa: *prepend(&head) = ptr
 */
void **prepend(struct Node **headptr) {
	if ((*headptr)->ptr == NULL) {
		return &((*headptr)->ptr);
	}

	struct Node *new_head;
	new_head = malloc(sizeof(struct Node));

	if (!new_head) {
		fprintf(stderr,"Malloc failed, aborting.\n");
		exit(EXIT_FAILURE);
	}
	new_head->next = *headptr;
	// new_head->ptr = NULL;
	*headptr = new_head;
	return &new_head->ptr;
	// return NULL;
}


// Riktig maate aa kalle denne funksjonen paa: rm_first(&head)
void rm_first(struct Node **headptr) {
	if ((*headptr)->next == NULL) {
		free((*headptr)->ptr);
		(*headptr)->ptr = NULL;
	}
	else {
		struct Node *new_head = (*headptr)->next;
		free((*headptr)->ptr);
		free(*headptr);
		*headptr = new_head;
	}
}

// fjerner victim fra lista
void rm_element(struct Node *victim, struct Node *prev) {
	free(victim->ptr);
	// dette kan vaere null om victim er den siste i lista
	// dette gaar bra
	prev->next = victim->next; // = prev->next->next;
	free(victim);
}

void print_clients(struct Node *head) {
	struct Node *cur = head;
	if (cur->ptr == NULL) {
		return;
	}
	printf("CLIENTS:\n");

	while (cur != NULL) {
		printf("nick: %s\n",((struct Client*)cur->ptr)->nick);
		printf("ticks to live: %lu\n",((struct Client*)cur->ptr)->ticks_to_live);
		cur = cur->next;
	}
}

void print_blocked_nicks(struct Node *head) {
	struct Node *cur = head;
	if (cur->ptr == NULL) {
		return;
	}

	printf("BLOCKED NICKS:\n");
	while (cur != NULL) {
		printf("nick: %s\n",(char*)cur->ptr);
		cur = cur->next;
	}
}

void print_messages(struct Node *head) {
	struct Node *cur = head;
	if (cur->ptr == NULL) {
		return;
	}

	printf("MESSAGES:\n");
	while (cur != NULL) {
		printf("type: %d\n",((struct Message*)cur->ptr)->type);
		printf("number: %d\n",((struct Message*)cur->ptr)->number);
		printf("attempts: %d\n",((struct Message*)cur->ptr)->attempts);
		printf("\n");
		cur = cur->next;
	}
}
// tar en meldingsstruct og lager en string av den saa man kan
// sende den over nettet
char *construct_message(struct Message *msg) {
	if (msg == NULL) {
		return NULL;
	}
	char *retmsg;
	int msglen;
	int ret;

	/* foerst regnet jeg ut eksakt hvor mange bytes hver melding ville
	 * ha og malloca eksakt denne mengden men jeg tror dette er meningsloest
	 * og vil bare oeke sannsynligheten for feil. Saa vi tar bare 50 med mindre
	 * det er tekstmelding, som kan vaere veldig stor
	 */
	msglen = 50;
	switch(msg->type) {
		case MSG_REGISTER: ;
			// vi mallocer max-meldingstoerrelse for denne
			retmsg = malloc(msglen);

			if (!retmsg) {
				fprintf(stderr,"Malloc failed, aborting.\n");
				exit(EXIT_FAILURE);
			}

			// %d istendenfor unsigned char fordi alle chars blir promota
			// til int naar de printes
			ret = snprintf(retmsg,msglen,"PKT %d REG %s",msg->number,msg->from_nick);
			if (ret < 0) {
				fprintf(stderr,"snprintf failed. Aborting.\n");
				exit(EXIT_FAILURE);
			}

			if (VERBOSE) printf("SENT: %s\n",retmsg);
			return retmsg;
			break;

		case MSG_AFFIRM_ACK: ;
			retmsg = malloc(msglen);

			if (!retmsg) {
				fprintf(stderr,"Malloc failed, aborting.\n");
				exit(EXIT_FAILURE);
			}


			ret = snprintf(retmsg,msglen,"ACK %d OK",msg->number);
			if (ret < 0) {
				fprintf(stderr,"snprintf failed. Aborting.\n");
				exit(EXIT_FAILURE);
			}

			if (VERBOSE) printf("SENT: %s\n",retmsg);
			return retmsg;
			break;

		case MSG_LOOKUP: ;
			retmsg = malloc(msglen);
			if (!retmsg) {
				fprintf(stderr,"Malloc failed, aborting.\n");
				exit(EXIT_FAILURE);
			}
			ret = snprintf(retmsg,msglen,"PKT %d LOOKUP %s",msg->number,msg->to_nick);
			if (ret < 0) {
				fprintf(stderr,"snprintf failed. Aborting.\n");
				exit(EXIT_FAILURE);
			}

			if (VERBOSE) printf("SENT: %s\n",retmsg);
			return retmsg;
			break;

		case MSG_LOOKUP_FAIL: ;
			retmsg = malloc(msglen);
			if (!retmsg) {
				fprintf(stderr,"Malloc failed, aborting.\n");
				exit(EXIT_FAILURE);
			}
			ret = snprintf(retmsg,msglen,"ACK %d NOT FOUND",msg->number);
			if (ret < 0) {
				fprintf(stderr,"snprintf failed. Aborting.\n");
				exit(EXIT_FAILURE);
			}

			if (VERBOSE) printf("SENT: %s\n",retmsg);
			return retmsg;
			break;

		case MSG_LOOKUP_OK: ;
			retmsg = malloc(msglen);
			if (!retmsg) {
				fprintf(stderr,"Malloc failed, aborting.\n");
				exit(EXIT_FAILURE);
			}
			ret = snprintf(retmsg,msglen,"ACK %d NICK %s %s PORT %hu",msg->number,msg->to_nick,msg->ip,msg->port);
			if (ret < 0) {
				fprintf(stderr,"snprintf failed. Aborting.\n");
				exit(EXIT_FAILURE);
			}

			if (VERBOSE) printf("SENT: %s\n",retmsg);
			return retmsg;
			break;

		case MSG_TEXT: ;
			retmsg = malloc(BUFSIZE);
			if (!retmsg) {
				fprintf(stderr,"Malloc failed, aborting.\n");
				exit(EXIT_FAILURE);
			}
			ret = snprintf(retmsg,BUFSIZE-1,"PKT %d FROM %s TO %s MSG %s",msg->number,msg->from_nick,msg->to_nick,msg->text);
			if (ret < 0) {
				fprintf(stderr,"snprintf failed. Aborting.\n");
				exit(EXIT_FAILURE);
			}

			if (VERBOSE) printf("SENT: %s\n",retmsg);
			return retmsg;
			break;

		case MSG_TEXT_2: ;
			retmsg = malloc(BUFSIZE);
			if (!retmsg) {
				fprintf(stderr,"Malloc failed, aborting.\n");
				exit(EXIT_FAILURE);
			}
			ret = snprintf(retmsg,BUFSIZE-1,"PKT %d FROM %s TO %s MSG %s",msg->number,msg->from_nick,msg->to_nick,msg->text);
			if (ret < 0) {
				fprintf(stderr,"snprintf failed. Aborting.\n");
				exit(EXIT_FAILURE);
			}

			if (VERBOSE) printf("SENT: %s\n",retmsg);
			return retmsg;
			break;

		case MSG_WR_NAME: ;
			retmsg = malloc(msglen);
			if (!retmsg) {
				fprintf(stderr,"Malloc failed, aborting.\n");
				exit(EXIT_FAILURE);
			}
			ret = snprintf(retmsg,msglen,"ACK %d WRONG NAME",msg->number);
			if (ret < 0) {
				fprintf(stderr,"snprintf failed. Aborting.\n");
				exit(EXIT_FAILURE);
			}

			if (VERBOSE) printf("SENT: %s\n",retmsg);
			return retmsg;
			break;

		case MSG_WR_FORM: ;
			retmsg = malloc(msglen);
			if (!retmsg) {
				fprintf(stderr,"Malloc failed, aborting.\n");
				exit(EXIT_FAILURE);
			}
			ret = snprintf(retmsg,msglen,"ACK %d WRONG FORMAT",msg->number);
			if (ret < 0) {
				fprintf(stderr,"snprintf failed. Aborting.\n");
				exit(EXIT_FAILURE);
			}

			if (VERBOSE) printf("SENT: %s\n",retmsg);
			return retmsg;
			break;
		// ingen default fordi alle returnerer noe saa derfor er default
		// etter }
	}
	return NULL;
}

/* leser en pakke og konstruerer en struct Message av den. Om pakken
 * ikke er velformatert returnerer den noe annet en 0 og setter msgptr
 * til aa peke paa null
 *
 * Dette er et monster av en funksjon paa over 200 linjer saa get ready
 * I denne funksjonen betyr returverdien DECON_WR_FORM at meldingen er har feil,
 * men ikke saa store feil at man ikke kan svare med WRONG FORMAT eller
 * lignende. DECON_CRIT_FAIL betyr at meldingen er formatert saa feil at 
 * man ikke engang kan svare paa meldingen, fordi pkt nummer er feilformatert
 * (det gir ingen mening aa sende en ACK tilbake dersom pakkenummeret 
 * ikke er et tall engang) Denne vil ofte gi DECON_WR_FORM om du sender 
 * med netcat fordi den sender med \n, som gjoer at strcmp feiler
 */
int deconstruct_message(char *str, struct Message **msgptr) {
	int ret;
	// keys er alle ting i allcaps, f.eks PKT eller REG
	// det kan vaere paa det meste fire keys (i typen tekstmelding)
	char *key0;
	char *key1;
	char *key2;
	char *key3;
	// andre variable
	unsigned char number;
	// host byte order
	unsigned short port;
	// human readable xxx.xxx.xxx.xxx\0
	char *possible_ip;

	char *possible_nick;

	char *possible_text;

	// for bruk av str_to_long
	long number_long;

	char *delim = " ";
	char *saveptr;

	key0 = strtok_r(str,delim,&saveptr);

	if (!key0) {
		*msgptr = NULL;
		return DECON_CRIT_FAIL;
	}

	// sjekker om det er PKT eller ACK
	int ack; // brukes som boolean
	ack = strcmp(key0,"ACK");
	if (ack) ack = 0; else ack = 1; // strcmp returnerer 0 om det er lik ack

	int pkt; // brukes som boolean
	pkt = strcmp(key0,"PKT");
	if (pkt) pkt = 0; else pkt = 1; // strcmp returnerer 0 om det er lik pkt

	if (pkt || ack) {
		struct Message* msg = malloc(sizeof(struct Message));

		if (!msg) {
			fprintf(stderr,"Malloc failed, aborting.\n");
			exit(EXIT_FAILURE);
		}

		char *number_string;
		number_string = strtok_r(NULL,delim,&saveptr);

		if (!number_string) {
			*msgptr = NULL;
			return DECON_CRIT_FAIL;
		}

		ret = str_to_long(number_string,&number_long);

		/* illdefined number
		 * om dette skjer staar det daarlig til, ettersom man skal svare
		 * en feilformatert pakke med ACK number WRONG FORMAT. Men hva om
		 * det er selve pakkenummeret som er feilformatert? Dette vil
		 * aldri skje i praksis med mine klienter, men jeg velger
		 * aa haandtere denne teoretiske feilen ved aa bare ikke svare
		 * paa en slik melding; den er rett aa slett for feilformatert til
		 * aa svares paa. Dermed vil klienten bare proeve et par ganger
		 * og til slutt gi opp
		 */
		if (ret == -1) {
			*msgptr = NULL;
			return DECON_CRIT_FAIL;
		}

		if (number_long < 0 || number_long > 255) {
			*msgptr = msg;
			return DECON_CRIT_FAIL;
		}
		number = (unsigned char) number_long;
		msg->number = number;

		key1 = strtok_r(NULL,delim,&saveptr);

		if (!key1) {
			*msgptr = msg;
			return DECON_WR_FORM;
		}

		// naa begynner vi en switch/case, men med if/else fordi vi har strings

		// PKT nummer REG nick
		if (!strcmp(key1,"REG") && pkt) {

			possible_nick = strtok_r(NULL,delim,&saveptr);

			if (!possible_nick) {
				*msgptr = msg;
				return DECON_WR_FORM;
			}

			ret = check_nick(possible_nick);

			// illdefined nick
			if (ret) {
				*msgptr = msg;
				return DECON_WR_FORM;
			}

			msg->type = MSG_REGISTER;
			msg->looked_up = 0;
			strcpy(msg->from_nick,possible_nick);
			*msgptr = msg;
			return DECON_SUCCESS;
		}
		// ACK nummer OK
		else if (!strcmp(key1,"OK") && ack) {
			msg->type = MSG_AFFIRM_ACK;
			msg->looked_up = 0;
			*msgptr = msg;
			return DECON_SUCCESS;
		}

		// PKT nummer LOOKUP nick
		else if (!strcmp(key1,"LOOKUP") && pkt) {

			possible_nick = strtok_r(NULL,delim,&saveptr);

			if (!possible_nick) {
				*msgptr = msg;
				return DECON_WR_FORM;
			}

			ret = check_nick(possible_nick);
			// illdefined nick
			if (ret) {
				*msgptr = msg;
				return DECON_WR_FORM;
			}


			strcpy(msg->to_nick,possible_nick);
			msg->type = MSG_LOOKUP;
			msg->looked_up = 0;

			*msgptr = msg;
			return DECON_SUCCESS;
		}

		// ACK nummer NOT FOUND
		else if (!strcmp(key1,"NOT") && ack) {
			key2 = strtok_r(NULL,delim,&saveptr);

			if (!key2) {
				*msgptr = msg;
				return DECON_WR_FORM;
			}

			if (!strcmp(key2,"FOUND")) {
				msg->type = MSG_LOOKUP_FAIL;
				msg->looked_up = 0;

				*msgptr = msg;
				return DECON_SUCCESS;
			}

			// wrong format
			*msgptr = msg;
			return DECON_WR_FORM;
		}

		// ACK nummer NICK nick adresse PORT port
		else if (!strcmp(key1,"NICK") && ack) {
			possible_nick = strtok_r(NULL,delim,&saveptr);

			if (!possible_nick) {
				*msgptr = msg;
				return DECON_WR_FORM;
			}

			ret = check_nick(possible_nick);
			// illdefined nick
			if (ret) {
				*msgptr = msg;
				return DECON_WR_FORM;
			}

			strcpy(msg->to_nick,possible_nick);

			possible_ip = strtok_r(NULL,delim,&saveptr);

			if (!possible_ip) {
				*msgptr = msg;
				return DECON_WR_FORM;
			}

			// vi bruker en dummy variable paa stacken for inet_pton, bare for
			// aa sjekke om det er en gyldig adresse

			int dummy;
			ret = inet_pton(AF_INET,possible_ip,&dummy);
			if (ret != 1) {
				// invalid ip
				*msgptr = msg;
				return DECON_WR_FORM;
			}
			strcpy(msg->ip,possible_ip);

			key2 = strtok_r(NULL,delim,&saveptr);

			if (!key2) {
				*msgptr = msg;
				return DECON_WR_FORM;
			}

			// ikke lik PORT
			if (strcmp(key2,"PORT")) {
				*msgptr = msg;
				return DECON_WR_FORM;
			}
			char *port_str;
			port_str = strtok_r(NULL,delim,&saveptr);

			if (!port_str) {
				*msgptr = msg;
				return DECON_WR_FORM;
			}

			ret = str_to_long(port_str,&number_long);
			// illdefined number
			if (ret == -1) {
				*msgptr = msg;
				return DECON_WR_FORM;
			}

			if (number_long < 0 || number_long > 65535) {
				*msgptr = msg;
				return DECON_WR_FORM;
			}

			port = (unsigned short) number_long;

			msg->port = port;
			msg->type = MSG_LOOKUP_OK;
			msg->looked_up = 0;

			*msgptr = msg;
			return DECON_SUCCESS;
		}

		// PKT nummer FROM from_nick TO to_nick MSG text
		else if (!strcmp(key1,"FROM") && pkt) {
			possible_nick = strtok_r(NULL,delim,&saveptr);

			if (!possible_nick) {
				*msgptr = msg;
				return DECON_WR_FORM;
			}

			ret = check_nick(possible_nick);
			// illdefined nick
			if (ret) {
				*msgptr = msg;
				return DECON_WR_FORM;
			}
			strcpy(msg->from_nick,possible_nick);

			key2 = strtok_r(NULL,delim,&saveptr);

			if (!key2) {
				*msgptr = msg;
				return DECON_WR_FORM;
			}

			if (!strcmp(key2,"TO")) {
				possible_nick = strtok_r(NULL,delim,&saveptr);

				if (!possible_nick) {
					*msgptr = msg;
					return DECON_WR_FORM;
				}

				ret = check_nick(possible_nick);
				// illdefined nick
				if (ret) {
					*msgptr = msg;
					return DECON_WR_FORM;
				}
				strcpy(msg->to_nick,possible_nick);

				key3 = strtok_r(NULL,delim,&saveptr);

				if (!key3) {
					*msgptr = msg;
					return DECON_WR_FORM;
				}

				if (!strcmp(key3,"MSG")) {
					possible_text = saveptr; // start of next token

					if (strlen(possible_text) > 1400) possible_text[1400] = '\0';

					for (int i = 0; possible_text[i] != 0; i++) {
						/* Dette er en ganske grei maate aa sjekke for ascii paa:
						 * ASCII-tegn er mellom 0 og 127. I en signed char 
						 * (som vi har) er dette alle positive tall vi har, 
						 * saa vi kan bare sjekke om tallet ikke er negativt. 
						 * Normalt ville man sjekke om tallet
						 * er stoerre en 127 i en unsigned char 
						 * for aa sjekke om det ikke
						 * er ASCII, men en unsigned char vil ha 
						 * MSB satt til 1 om det er
						 * stoerre en 127, som ogsaa er noe 
						 * alle negative tall i en signed
						 * char vil ha.
						 */
						if (possible_text[i] < 0 || possible_text[i] < 32) {
							*msgptr = msg;
							return DECON_WR_FORM;
						}
					}

					strcpy(msg->text,possible_text);
					msg->type = MSG_TEXT;
					msg->looked_up = 0;



					*msgptr = msg;
					return DECON_SUCCESS;
				}
			}
			else {
				*msgptr = msg;
				return DECON_WR_FORM;
			}
		}
		else if (!strcmp(key1,"WRONG")) {
			key2 = strtok_r(NULL,delim,&saveptr);

			if (!key2) {
				*msgptr = msg;
				return DECON_WR_FORM;
			}

			if (!strcmp(key2,"NAME")) {
				msg->type = MSG_WR_NAME;
				msg->looked_up = 0;

				*msgptr = msg;
				return DECON_SUCCESS;
			}
			else if (!strcmp(key2,"FORMAT")) {
				msg->type = MSG_WR_FORM;
				msg->looked_up = 0;

				*msgptr = msg;
				return DECON_SUCCESS;
			}
			else {
				*msgptr = msg;
				return DECON_WR_FORM;
			}
		}

		*msgptr = msg;
		return DECON_WR_FORM;
	}

	else {
		struct Message* msg = malloc(sizeof(struct Message));

		if (!msg) {
			fprintf(stderr,"Malloc failed, aborting.\n");
			exit(EXIT_FAILURE);
		}

		char *number_string;
		number_string = strtok_r(NULL,delim,&saveptr);

		if (!number_string) {
			*msgptr = msg;
			return DECON_CRIT_FAIL;
		}

		ret = str_to_long(number_string,&number_long);

		/* illdefined number
		 * om dette skjer staar det daarlig til, ettersom man skal svare
		 * en feilformatert pakke med ACK number WRONG FORMAT. Men hva om
		 * det er selve pakkenummeret som er feilformatert? Dette vil
		 * aldri skje i praksis med mine klienter, men jeg velger
		 * aa haandtere denne teoretiske feilen ved aa bare ikke svare
		 * paa en slik melding; den er rett aa slett for feilformatert til
		 * aa svares paa. Dermed vil klienten bare proeve et par ganger
		 * og til slutt gi opp
		 */
		if (ret == -1) {
			*msgptr = msg;
			return DECON_CRIT_FAIL;
		}
		*msgptr = msg;
		return DECON_WR_FORM;
	}
}


int read_from_stdin(char *buf, int size) {
	char* ret_ptr;
	char c;
	// vi leser fra stdin til vi treffer newline eller EOF
	ret_ptr = fgets(buf,size,stdin);
	if (!ret_ptr) {
		fprintf(stderr,"fgets: failure\n");
		exit(EXIT_FAILURE);
	}

	int amount_read = strlen(buf);
	// vi tar bort newlinen paa slutten
	if (buf[amount_read - 1] == '\n') {
		buf[amount_read - 1] = '\0';
	}

	else {
		// om vi ikke har newline tilslutt har vi skrevet mer en bufsize
		// da clearer vi resten av stdin med getchar
		c = getchar();
		while (c != EOF && c != '\n') {
			c = getchar();
		}
	}

	// sjekk om ASCII
	for (int i = 0; buf[i] != 0; i++) {
		/* Dette er en ganske grei maate aa sjekke for ascii paa:
		 * ASCII-tegn er mellom 0 og 127. I en signed char (som vi har)
		 * er dette alle positive tall vi har, saa vi kan bare sjekke om
		 * tallet ikke er negativt. Normalt ville man sjekke om tallet
		 * er stoerre en 127 i en unsigned char for aa sjekke om det ikke
		 * er ASCII, men en unsigned char vil ha MSB satt til 1 om det er
		 * stoerre en 127, som ogsaa er noe alle negative tall i en signed
		 * char vil ha.
		 */
		if (buf[i] < 0) return -1;
		else if (buf[i] < 32) return -2;
	}
	return 0;
}

int check_nick(const char *nick) {
	if (strlen(nick) > 20) return 1;
	// dette er sant dersom strchr returnerer noe annet enn NULL
	if (strchr(nick,' ')) return 2;
	if (strchr(nick,'\n')) return 2;
	if (strchr(nick,'\t')) return 2;

	for (int i = 0; nick[i] != 0; i++) {
		/* Dette er en ganske grei maate aa sjekke for ascii paa:
		 * ASCII-tegn er mellom 0 og 127. I en signed char (som vi har)
		 * er dette alle positive tall vi har, saa vi kan bare sjekke om
		 * tallet ikke er negativt. Normalt ville man sjekke om tallet
		 * er stoerre en 127 i en unsigned char for aa sjekke om det ikke
		 * er ASCII, men en unsigned char vil ha MSB satt til 1 om det er
		 * stoerre en 127, som ogsaa er noe alle negative tall i en signed
		 * char vil ha.
		 */
		if (nick[i] < 0) return 2;
		else if (nick[i] < 32) return 2;
	}
	return 0;
}

/* finner en klient ved aa se paa nicken. Legger prev i *prevaddr dersom vi
 * oensker aa slette denne senere, ettersom man trenger prev for aa slette
 * noe.
 */
struct Node *find_client(struct Node *head,char *nick, struct Node **prevaddr) {
	// vi setter *prevaddr til NULL, dersom head er den riktige blir den
	// aldri satt til noe annet
	*prevaddr = NULL;
	struct Node *cur;
	// cur er naa clients (= head)
	cur = head;
	while (cur && cur->ptr) {
		// enten er det riktig aa vi returnerer cur
		if (strcmp(((struct Client*)cur->ptr)->nick,nick) == 0) {
			return cur;
		}
		// eller saa er det ikke riktig og vi gaar videre, og setter
		// *prevaddr til den forrige
		*prevaddr = cur;
		cur = cur->next;
	}
	// om vi kommer hit har ingenting matcha
	return NULL;
}

struct Node *find_client_by_pkt(struct Node *head,unsigned char number, struct Node **prevaddr) {
	// vi setter *prevaddr til NULL, dersom head er den riktige blir den
	// aldri satt til noe annet
	*prevaddr = NULL;
	struct Node *cur_node;
	// cur er naa clients (= head)
	cur_node = head;
	while (cur_node && cur_node->ptr) {
		struct Client *cur_client;
		cur_client = (struct Client*) cur_node->ptr;

		if (cur_client->active_msg) {
			if (cur_client->active_msg->number == number) {
				return cur_node;
			}
		}
		// eller saa er det ikke riktig og vi gaar videre, og setter
		// *prevaddr til den forrige
		*prevaddr = cur_node;
		cur_node = cur_node->next;
	}
	// om vi kommer hit har ingenting matcha
	return NULL;
}

void add_client(struct Node *clients, char *nick, struct sockaddr_in *addr, unsigned long client_max_ticks,unsigned char triggering_pkt) {
	struct Node *dummy; // vi trenger ikke prev her
	struct Node *node;
	// vi finner en node med nicken vi fikk av klienten
	node = find_client(clients,nick,&dummy);

	// om en slik client allerede er registrert; oppdater addressen
	if (node) {
		free(((struct Client*) node->ptr)->addr);
		((struct Client*) node->ptr)->addr = addr;
		((struct Client*) node->ptr)->ticks_to_live = client_max_ticks;
	}

	// om ikke legger vi til en ny klient
	else {
		struct Client *client;
		client = malloc(sizeof(struct Client));

		if (!client) {
			fprintf(stderr,"Malloc failed, aborting.\n");
			exit(EXIT_FAILURE);
		}

		client->messages = malloc(sizeof(struct Node));
		client->messages->ptr = NULL;
		client->messages->next = NULL;
		client->active_msg = NULL;
		// vi vil at pakken skal bli sendt med en gang. Vi bruker ikke
		// ULONG_MAX i tilfelle vi med uhell overflower da
		client->pkt_wait = LONG_MAX;
		strcpy(client->nick,nick);
		client->addr = addr;
		client->ticks_to_live = client_max_ticks;
		client->last_pkt = triggering_pkt;
		*append(clients) = client;
	}
}

void free_client_ptrs(struct Client *client) {
	free(client->addr);
	free(client->active_msg);

	while(client->messages->ptr) {
		rm_first(&client->messages);
	}
	free(client->messages);
}
