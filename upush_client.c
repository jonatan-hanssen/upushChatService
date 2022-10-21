#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>

#include "send_packet.h"
#include "upush_helper.h"

// sjekker om nick er velformatert
int check_nick(const char *nick);

/* Leser fra stdin dersom buf[0] == '@', lager en melding ut av det som ligger
 * der dersom det er velformatert, legger det bakerst i meldingskoeen
 */
struct Message *message_from_input(char *input,char *from_nick, struct Node *blocked_nicks, char *my_nick);

/* Leser det som kommer paa socket, dekonstruerer teksten til en meldingsstruct,
 * fjerner det som ligger i activemsg om det er en ACK, legger en ACK
 * i starten av meldingskoeen dersom det er en tekstmelding
 */
struct Message *handle_incoming(char *buf,struct Node *blocked_nicks,struct Node *clients,char *my_nick);

/* Sender meldingen som ligger i active_msg, dekrementerer attempts og fjerner
 * meldingen dersom attempts er 0. Dersom meldingen som fjernes er en
 * tekstmelding av type 1 blir en lookupmelding og en tekstmelding av type
 * 2 lagt i starten av meldingskoeen.
 */
int send_active_msg(struct Message *msg,int fd, struct Node **messages_addr);

/* Gaar gjennom lenkelisten av blocked nicks og finner en Node med lik nick
 * dersom en slik fins
 */
struct Node *find_blocked_nick(struct Node *head,char *nick, struct Node **prevaddr);

/* Skjer dersom stdin hverken var @nick msg eller QUIT. Sjekker om foerste token
 * er BLOCK eller UNBLOCK og legger til eller fjerner neste token fra blocklisten
 */
void manage_block_list(char *buf,struct Node *blocked_nicks);

/* Ser paa neste melding i meldingskoeen, ser om denne har en kjent addresse
 * (det er serveren eller en nick vi kjenner) og sender den dersom dette er
 * tilfellet. Om ikke, legg en lookupmelding i active_msg og sett looked_up = 1
 * i melding man saa paa. Ved neste kall paa denne funksjonen vil vi enten ha en kjent adresse til meldingen (lookup var successfull) eller vi
 * har ikke det og funksjonen vil kaste meldingen ut av koeen fordi
 * looked_up == 1
 */
struct Message *prepare_next_message(struct Node **messages_addr, struct sockaddr_in *serv_addr,struct Client *calling_client);

// legger til i koe
void add_to_queue(struct Node *clients, struct Message *message);

// utfoerer prepare_next_message og send_active_message per klient
int client_send(struct Client *client,struct sockaddr_in *serv_addr, int sock_fd, unsigned long timeout_ticks);

void remove_pkt(unsigned char incoming_nr, struct Node *clients);

// global variabel for pakkenummer
unsigned char PKT_NR;

int main(int argc, char *argv[]) {
	PKT_NR = 0;

	// returverdier, for check_error og andre ting
	int ret,rc;
	if (argc != 6) {
		printf("usage: ./upush_client <nick> <addresse> <port> <timeout> <tapssansynlighet>\n");
		return EXIT_SUCCESS;
	}

	// ---------- <NICK> -----------------------
	char *my_nick = argv[1];

	ret = check_nick(my_nick);
	if (ret) {
		if (ret == 1) fprintf(stderr, "error: invalid nick (too long)\n");
		if (ret == 2) fprintf(stderr, "error: invalid nick (contains whitespace)\n");
		return EXIT_SUCCESS;
	}

	// ---------- <ADDRESSE> -----------------------
	struct in_addr server_ip;
	ret = inet_pton(AF_INET,argv[2],&server_ip.s_addr);
	check_error(ret,"inet_pton");
	// inet_pton returnerer ogsaa 0 dersom strengen er feilformatert
	if (!ret) fprintf(stderr,"error: wrong format on ip address\n");

	// ---------- <PORT> -----------------------
	unsigned short server_port;
	server_port = str_to_port(argv[3]);

	// ---------- <TIMEOUT> -----------------------
	float timeout;
	ret = str_to_float(argv[4],&timeout);
	check_error(ret,"str_to_float");

	if (timeout < 0) {
		fprintf(stderr,"error: timeout can not be negative\n");
		return EXIT_FAILURE;
	}

	// ---------- <LOSS PERCENTAGE> -----------------------
	float loss_percentage;
	ret = str_to_float(argv[5],&loss_percentage);
	check_error(ret,"str_to_float");

	if (loss_percentage < 0 || loss_percentage > 100) {
		fprintf(stderr,"error: loss percentage out of range\n");
		return EXIT_FAILURE;
	}

	float loss_rate = loss_percentage/100;

	/* Her har vi noe som jeg bare kan anta er en feil fra de som har lagd
	 * eksamens side: drand48 blir aldri seeda, noe som gjoer at den
	 * returnerer de eksakt samme verdiene hver gang. Enda verre er at
	 * de foerste fire verdiene er veldig lave (>0.20), som gjoer at en
	 * client med 20% loss rate vil miste sine foerste 4 pakker hver eneste
	 * gang. Jeg har derfor valgt aa seede foer set loss probability, og jeg 
	 * bruker time(NULL) fordi den vil vaere forskjellig fra gang til gang
	 */
	srand48(time(NULL));
	set_loss_probability(loss_rate);

	// ------ LAGING AV VAAR EGEN SOCKET -----------------
	int sock_fd;

	// 0 fordi vi ber OS om aa gi oss en ledig port
	sock_fd = create_and_bind_sock(0);

	// ---------- LAGING AV SERVERENS IP ------------------

	struct sockaddr_in *serv_addr;
	serv_addr = malloc(sizeof(struct sockaddr_in));

	serv_addr->sin_family = AF_INET;
	serv_addr->sin_port = htons(server_port);
	serv_addr->sin_addr = server_ip;

	char buf[BUFSIZE];

	// --------------- MELDINGLISTE ------------------

	// clienter man kjenner til
	struct Node *clients = initialize();

	/* Pseudoclient som holder paa en koee for registreringsmeldinger,
	 * saann at jeg kan bruke samme struktur som andre klienter
	 * for aa sende disse
	 */
	add_client(clients,"reg_client",NULL,0,0);

	/* liste av char*
	 * Dette burde strengt tatt vaert en del av client-structen,
	 * men det er legacy code fra naar strukturen saa ganske annerledes
	 * ut. Det fungerer helt uten problem saa hvorfor endre det?
	 */
	struct Node *blocked_nicks = initialize();

	// registreringsmelding blir puttet i meldingskoen med en gang
	struct Message *reg_msg = malloc(sizeof(struct Message));

	if (!reg_msg) {
		fprintf(stderr,"Malloc failed, aborting.\n");
		exit(EXIT_FAILURE);
	}

	reg_msg->type = MSG_REGISTER;
	strcpy(reg_msg->from_nick,my_nick);
	reg_msg->number = PKT_NR;
	// saa vidt jeg kan se staar det ingenting om hvor mange ganger
	// man skal proeve aa sende reg til serveren saa jeg antar 3 ganger
	reg_msg->attempts = 3;
	// putter server addressen i reg_msg
	memcpy(&reg_msg->dest_addr,&serv_addr,sizeof(struct sockaddr_in));

	add_to_queue(clients,reg_msg);
	PKT_NR++;

	// --------------- LOOP SETUP -------------------

	// ------- timeout variable -------------
	// hvor lang en tick er i mikrosekunder
	unsigned long ticklen;

	// 16.66 ms, aka 60 fps (det er nok ikke akkuratt 60 fps fordi hver
	// operasjon mellom ticksene bruker tid)
	ticklen = 16666;


	// timeout i mikrosekunder
	unsigned long timeout_us;
	timeout_us = timeout * 1000000.0f;

	// vi vil heller ha timeouten i ticks fordi det er lettere aa holde styr
	// paa og mindre sannsynlig at det vil overflowe en unsigned long
	// (ikke at det er saa sannsynlig uansett)
	unsigned long timeout_ticks;
	timeout_ticks = (unsigned long) timeout_us/ticklen;

	// ------- heartbeat variable -------------
	unsigned long heartbeat_us;
	heartbeat_us = 10*ONE_MILLION;

	// antall ticks for et heartbeat
	unsigned long heartbeat_ticks;
	heartbeat_ticks = heartbeat_us/ticklen;

	unsigned long heartbeat_wait;
	heartbeat_wait = 0;


	printf("Welcome to the UPush service, %s.\n\n",my_nick);
	printf("- Use @<nick> <message> to send a message to <nick>\n");
	printf("- Use BLOCK <nick> or UNBLOCK <nick> to block or unblock a user.\n");
	printf("- Write QUIT to exit.\n");
	// --------------- SELECT LOOP -------------------
	while (1) {
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(sock_fd,&fds);
		FD_SET(STDIN_FILENO,&fds); // STDIN_FILENO == 0

		struct timeval tick;
		tick.tv_sec = 0;
		// 60 fps, hvorfor ikke
		tick.tv_usec = ticklen;
		// tick.tv_usec = 1000000;
		ret = select(FD_SETSIZE,&fds,NULL,NULL,&tick);
		check_error(ret,"select");

		// vi har faatt noe fra nettet
		if (FD_ISSET(sock_fd,&fds)) {

			struct sockaddr_in *src_addr;
			src_addr = malloc(sizeof(struct sockaddr_in));

			if (!src_addr) {
				fprintf(stderr,"Malloc failed, aborting.\n");
				exit(EXIT_FAILURE);
			}

			unsigned int addrlen = sizeof(struct sockaddr_in);

			rc = recvfrom(sock_fd,
					buf,
					BUFSIZE-1,
					0,
					(struct sockaddr*) src_addr,
					&addrlen);
			check_error(rc,"recvfrom");
			buf[rc] = '\0';

			struct Message *response = handle_incoming(buf,blocked_nicks,clients,my_nick);

			if (response) {
				char *payload;
				payload = construct_message(response);
				free(response);

				if (payload) {
					ret = send_packet(
							sock_fd,
							payload,
							strlen(payload),
							0,
							(struct sockaddr*) src_addr,
							addrlen
					);
					check_error(ret,"send_packet");
				}
				// add_to_queue(clients,response);
				free(payload);
			}
			free(src_addr);
		}

		// vi har skrevet noe i terminalen
		else if (FD_ISSET(STDIN_FILENO,&fds)) {
			ret = read_from_stdin(buf,BUFSIZE);
			if (ret == -1) {
				fprintf(stderr,"Non-ASCII character typed. No message sent.\n");
				continue;
			}
			else if (ret == -2) {
				fprintf(stderr,"ASCII escape sequence typed. No message sent.\n");
				continue;
			}
			// vi gaar ut av while loekka om dette skjer
			if (!strcmp(buf,"QUIT")) break;

			if (buf[0] == '@') {
				struct Message *msg;
				msg = message_from_input(buf,my_nick,blocked_nicks,my_nick);

				if (msg) {
					add_to_queue(clients,msg);
				}
			}
			// kommer vi hit er eneste mulighet at vi vil blokke noen
			else {
				manage_block_list(buf,blocked_nicks);
			}
		}

		// en ticklen har gaatt
		else {
			// vi skal naa gaa gjennom alle clientene og sjekke om
			// det er noen meldinger der aa sende
			struct Node *cur_node;
			cur_node = clients;

			int server_down;
			server_down = 0;

			while (cur_node && cur_node->ptr) {
				struct Client *cur_client;
				cur_client = (struct Client*) cur_node->ptr;

				// denne kjoerer prepare_next_message og send_active_message
				// per client. Som oftest er det ingenting som skjer, men
				// om det ligger meldinger der vil de bli sendt i henhold til
				// stop and wait
				server_down = client_send(cur_client,serv_addr,sock_fd,timeout_ticks);
				if (server_down) break;

				cur_node = cur_node->next;
			}

			if (server_down) {
				fprintf(stderr,"SERVER UNREACHABLE. SHUTTING DOWN.\n");
				break;
			}


			if (heartbeat_wait > heartbeat_ticks) {
				// vi lager en ny reg og legger den til i reg_client
				struct Message *reg_msg = malloc(sizeof(struct Message));

				if (!reg_msg) {
					fprintf(stderr,"Malloc failed, aborting.\n");
					exit(EXIT_FAILURE);
				}

				reg_msg->type = 0;
				strcpy(reg_msg->from_nick,my_nick);
				reg_msg->number = PKT_NR;
				reg_msg->attempts = 3;
				memcpy(&reg_msg->dest_addr,&serv_addr,sizeof(struct sockaddr_in));
				PKT_NR++;

				add_to_queue(clients,reg_msg);
				heartbeat_wait = 0;
			}

			heartbeat_wait += 1;
		}
	}

	// ------------ CLEANUP ------------------
	// vi kommer hit fordi vi skrev QUIT eller serveren er unreachable

	// freer blocked nicks
	while(blocked_nicks->ptr) {
		rm_first(&blocked_nicks);
	}
	free(blocked_nicks);

	// freer clients
	while (clients->ptr) {
		struct Client *client;
		client = (struct Client*) clients->ptr;
		free_client_ptrs(client);
		rm_first(&clients);
	}
	free(clients);

	// andre frees
	free(serv_addr);
	close(sock_fd);

	return EXIT_SUCCESS;
}

int send_active_msg(struct Message *msg,int fd, struct Node **messages_addr) {
	if (!msg) return ACTIVE_NO_MSG;
	if (msg->attempts <= 0) {
		switch (msg->type) {
			case MSG_LOOKUP: ;
				free(msg);
				return ACTIVE_SERV_NO_RESPONSE;
			case MSG_REGISTER: ;
				free(msg);
				return ACTIVE_SERV_NO_RESPONSE;
			case MSG_TEXT: ;
				/* her vil jeg egentlig bare returnere noe aa laa en annen
				 * funksjon legge til to nye ting i messages, men ettersom
				 * all informasjonen man trenger ligger i msg maa det bli saann
				 * her
				 */
				struct Message *lookup;
				lookup = malloc(sizeof(struct Message));

				if (!lookup) {
					fprintf(stderr,"Malloc failed, aborting.\n");
					exit(EXIT_FAILURE);
				}

				lookup->type = 2;
				strcpy(lookup->to_nick,msg->to_nick);
				lookup->number = PKT_NR;
				lookup->attempts = 3;
				lookup->triggering_pkt = msg->number;

				PKT_NR++;

				struct Message *msg2;
				msg2 = malloc(sizeof(struct Message));

				if (!msg2) {
					fprintf(stderr,"Malloc failed, aborting.\n");
					exit(EXIT_FAILURE);
				}

				memcpy(msg2,msg,sizeof(struct Message));
				msg2->attempts = 2;
				msg2->type = MSG_TEXT_2;
				msg2->looked_up = 1;

				*prepend(messages_addr) = msg2;
				*prepend(messages_addr) = lookup;

				free(msg);
				return ACTIVE_MSG_EXPIRED;
			case MSG_TEXT_2: ;
				fprintf(stderr,"NICK %s UNREACHABLE\n",msg->to_nick);
				free(msg);
				return ACTIVE_MSG_EXPIRED;
			default:
				free(msg);
				return ACTIVE_MSG_EXPIRED;
		}

	}

	// konstruerer en streng av denne
	char *payload = construct_message(msg);

	// det er hoeyst viktig at ingen meldinger blir puttet her uten
	// dest_addr satt
	if (payload) {
		send_packet(
				fd,
				payload,
				strlen(payload),
				0,
				(struct sockaddr*) &msg->dest_addr,
				sizeof(struct sockaddr_in)
		);
	}
	free(payload);

	msg->attempts--;
	return ACTIVE_MSG_SENT;
}

// Lager en struct Message av stdin input om det er skrevet riktig
struct Message *message_from_input(char *input,char *from_nick,struct Node *blocked_nicks,char *my_nick) {
	char *delim = " ";
	char *saveptr;

	char *possible_nick;
	possible_nick = strtok_r(input,delim,&saveptr);
	possible_nick++; // vi vil hoppe over @
	int nick_len;
	nick_len = strlen(possible_nick);
	if (nick_len > 20 || nick_len == 0) {
		fprintf(stderr,"Invalid nick. No message sent.\n");
		return NULL;
	}
	if (!strcmp(possible_nick,my_nick)) {
		fprintf(stderr,"Cannot send message to yourself. No message sent.\n");
		return NULL;
	}
	// sjekker om nicken er blokka
	struct Node *nick_node;
	struct Node *dummy;

	nick_node = find_blocked_nick(blocked_nicks,possible_nick,&dummy);
	if (nick_node) {
		fprintf(stderr,"Recipient blocked. No message sent.\n");
		return NULL;
	}

	// om vi kom hit var den ikke blokka

	char *text;
	text = saveptr;

	if (strlen(text) > 1400) text[1400] = '\0'; // den 1401. byten blir nullbyte

	// lager meldingsstruct
	struct Message *msg;
	msg = malloc(sizeof(struct Message));

	if (!msg) {
		fprintf(stderr,"Malloc failed, aborting.\n");
		exit(EXIT_FAILURE);
	}

	msg->type = MSG_TEXT;
	msg->number = PKT_NR;
	msg->attempts = 2;
	msg->looked_up = 0;
	strcpy(msg->to_nick,possible_nick);
	strcpy(msg->from_nick,from_nick);
	strcpy(msg->text,text);
	PKT_NR++;

	return msg;
}

struct Message *handle_incoming(char *buf,struct Node *blocked_nicks,struct Node *clients,char *my_nick) {

	if (VERBOSE) printf("RECIEVED: %s\n",buf);
	int ret;
	struct Message *incoming;
	ret = deconstruct_message(buf,&incoming);

	struct Message *response;
	response = (struct Message*) malloc(sizeof(struct Message));

	if (!response) {
		fprintf(stderr,"Malloc failed, aborting.\n");
		exit(EXIT_FAILURE);
	}

	if (ret == DECON_CRIT_FAIL) {
		if (VERBOSE) printf("Recieved critically illformed message. No response will be sent.\n");
		// ignorer, pakkenummer er feilformatert
		free(response);
		return NULL;
	}
	// pakkenummer er velformatert og vi setter det
	response->number = incoming->number;

	if (ret == DECON_SUCCESS) {

		switch(incoming->type) {
			// affirmative ack
			case MSG_AFFIRM_ACK: ;
				remove_pkt(incoming->number,clients);
				free(response);
				response = NULL;
				break;

			case MSG_TEXT: ;
				// vi maa kopiere denne saa vi vet hvem vi skal svare til
				strcpy(response->to_nick,incoming->from_nick);

				// sjekker om nicken er blokka og om vi har en siste pakke
				// mottatt fra denne
				struct Node *dummy;
				struct Node *block_node;
				struct Node *client_node;

				block_node = find_blocked_nick(blocked_nicks,incoming->from_nick,&dummy);

				client_node = find_client(clients,incoming->from_nick,&dummy);

				if (!strcmp(incoming->to_nick,my_nick)) {
					response->type = MSG_AFFIRM_ACK;
					response->looked_up = 0;
					// ingen attempts paa en ack
					response->attempts = 1;

					// enten er clienten i clients, og vi maa sjekke om siste
					// pakke er den samme som denne og om vi har blokka denne
					if (client_node && !block_node) {

						if ( ((struct Client*) client_node->ptr)->last_pkt != incoming->number) {
							printf("%s: %s\n",incoming->from_nick,incoming->text);
							((struct Client*) client_node->ptr)->last_pkt = incoming->number;
						}
						else {
							if (VERBOSE) printf("Last packet recieved: %d, incoming: %d. Duplicate ignored.\n",((struct Client*) client_node->ptr)->last_pkt,incoming->number);
						}
					}
					else if (!client_node && !block_node) {
						if (VERBOSE) printf("Adding client %s with no address\n",incoming->from_nick);
						add_client(clients,incoming->from_nick,NULL,0,incoming->number);
						printf("%s: %s\n",incoming->from_nick,incoming->text);

					}
				}
				else {
					response->type = MSG_WR_NAME;
					response->attempts = 1;
					response->looked_up = 0;
				}
				break;
			case MSG_LOOKUP_FAIL: ;
				// sjekk om denne nicken ligger i clientene med en utdatert ip
				struct Node *prev;
				prev = NULL;

				client_node = find_client_by_pkt(clients,incoming->number,&prev);

				struct Client *client;
				client = (struct Client*) client_node->ptr;

				fprintf(stderr,"NICK %s NOT REGISTERED\n",client->active_msg->to_nick);

				free_client_ptrs(client);
				if (prev) {
					rm_element(client_node,prev);
				}
				else {
					rm_first(&clients);
				}

				remove_pkt(incoming->number,clients);

				free(response);
				response = NULL;
				break;
			case MSG_LOOKUP_OK: ;

				struct sockaddr_in *new_addr;
				new_addr = malloc(sizeof(struct sockaddr_in));

				if (!new_addr) {
					fprintf(stderr,"Malloc failed, aborting.\n");
					exit(EXIT_FAILURE);
				}

				new_addr->sin_family = AF_INET;
				inet_pton(AF_INET,incoming->ip,&new_addr->sin_addr.s_addr);
				new_addr->sin_port = htons(incoming->port);


				client_node = find_client_by_pkt(clients,incoming->number,&dummy);
				client = (struct Client*) client_node->ptr;

				add_client(clients,incoming->to_nick,new_addr,0,client->active_msg->triggering_pkt);

				remove_pkt(incoming->number,clients);

				free(response);
				response = NULL;
				break;
			case MSG_WR_NAME: ;
				remove_pkt(incoming->number,clients);
				free(response);
				response = NULL;
				break;
			case MSG_WR_FORM: ;
				remove_pkt(incoming->number,clients);
				free(response);
				response = NULL;
				break;


			default:
				// ignorer andre meldinger
				free(response);
				response = NULL;
				break;
		}
	}
	// feilformatert melding
	else if (ret == DECON_WR_FORM) {
		response->type = MSG_WR_FORM;
	}

	free(incoming);

	return response;
}


struct Node *find_blocked_nick(struct Node *head,char *nick, struct Node **prevaddr) {
	// vi setter *prevaddr til NULL, dersom head er den riktige blir den
	// aldri satt til noe annet
	*prevaddr = NULL;
	struct Node *cur;
	// cur er naa clients (= head)
	cur = head;
	while (cur && cur->ptr) {
		// enten er det riktig aa vi returnerer cur
		if (!strcmp(cur->ptr,nick)) {
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

void manage_block_list(char *buf,struct Node *blocked_nicks) {

	char *delim;
	delim = " ";
	char *token;

	token = strtok(buf,delim);
	if (!token) return; // bare delimiters

	if (!strcmp(token,"BLOCK")) {
		char *nick;
		nick = strtok(NULL,delim);
		if (!nick) return; // intet andre ord

		// dersom nicken allerede er blokka gjoer vi ingenting
		struct Node *nick_node;
		struct Node *dummy;
		nick_node = find_blocked_nick(blocked_nicks,nick,&dummy);
		if (nick_node) return;

		char *blocked_nick;
		blocked_nick = malloc(sizeof(char)*(strlen(nick)+1));

		if (!blocked_nick) {
			fprintf(stderr,"Malloc failed, aborting.\n");
			exit(EXIT_FAILURE);
		}

		strcpy(blocked_nick,nick);
		*append(blocked_nicks) = blocked_nick;
		print_blocked_nicks(blocked_nicks);
	}

	else if (!strcmp(token,"UNBLOCK")) {
		char *nick;
		nick = strtok(NULL,delim);
		if (!nick) return; // intet andre ord

		struct Node *prev;
		struct Node *nick_node;
		prev = NULL;

		nick_node = find_blocked_nick(blocked_nicks,nick,&prev);

		if (nick_node) {
			if (prev) {
				rm_element(nick_node,prev);
			}
			else {
				rm_first(&blocked_nicks);
			}
		}
		print_blocked_nicks(blocked_nicks);
	}
}

/* Denne funksjonen gjoer et par ting:
 * Den ser paa neste melding i meldingskoen til en klient
 * Den vil sjekke om denne klienten har en ip
 * og dersom, returnere meldingen med riktig ip. Om ikke 
 * vil den returnere en lookupmelding og la meldingen som laa som neste i 
 * meldingskoen bli vaerende. Den vil sette looked_up til 1 
 * paa denne meldingen. Naar denne meldingen forsoekes sendt 
 * neste gang vil vi enten ha en ip-addresse, eller meldingen vil ha 
 * looked_up == 1 og vil bli fjernet av koen.
 */
struct Message *prepare_next_message(struct Node **messages_addr,struct sockaddr_in *serv_addr,struct Client *calling_client) {
	struct Node *messages;
	messages = *messages_addr;

	struct Message *message_head;

	struct Message *ret_msg;

	message_head = (struct Message*) messages->ptr;

	if (!message_head) return NULL;

	int msg_type;
	msg_type = message_head->type;

	// --- TIL SERVEREN ---
	// her skal vi bare kopiere serv_addr og returnere meldingen "as is"
	if (msg_type == MSG_REGISTER || msg_type == MSG_LOOKUP) {
		memcpy(&message_head->dest_addr,serv_addr,sizeof(struct sockaddr_in));

		ret_msg = message_head;

		(*messages_addr)->ptr = NULL;
		rm_first(messages_addr);
		return ret_msg;
	}

	// ---- TIL EN CLIENT -------
	// her maa vi finne addressen eller returnere en lookupmelding
	// Det er forventet at alle disse har satt to_nick
	else if (msg_type == MSG_WR_FORM || 
			 msg_type == MSG_AFFIRM_ACK || 
			 msg_type == MSG_TEXT ||
			 msg_type == MSG_TEXT_2) 
	{
		// har vi en ip?
		if (calling_client->addr) {
			struct sockaddr_in *msg_addr;
			msg_addr = &message_head->dest_addr;

			memcpy(msg_addr,calling_client->addr,sizeof(struct sockaddr_in));

			// naa har vi addressen, saa returner denne og fjern den
			// fra koeen
			ret_msg = message_head;

			(*messages_addr)->ptr = NULL;
			rm_first(messages_addr);
			return ret_msg;
		}

		else {
			// vi har aldri proevd aa finne ipen
			if (message_head->looked_up == 0) {
				struct Message *lookup;
				lookup = malloc(sizeof(struct Message));

				if (!lookup) {
					fprintf(stderr,"Malloc failed, aborting.\n");
					exit(EXIT_FAILURE);
				}

				lookup->type = 2;
				strcpy(lookup->to_nick,message_head->to_nick);
				lookup->number = PKT_NR;
				lookup->attempts = 3;
				lookup->triggering_pkt = message_head->number;
				memcpy(&lookup->dest_addr,serv_addr,sizeof(struct sockaddr_in));
				PKT_NR++;

				message_head->looked_up = 1;
				return lookup;
			}
			else {
				// meldingen slettes og vi returnerer null
				rm_first(messages_addr);
				return NULL;
			}
		}
	}
	// om vi kom hit var meldingen ikke av noen av typene, bare slett den
	// og returner null
	else {
		rm_first(messages_addr);
		return NULL;
	}
}
// legger til en melding i den riktige klientens koee. Lager en klient
// dersom denne ikke eksisterer
void add_to_queue(struct Node *clients, struct Message *message) {
	// alle disse skal til en nick, og boer ha en to_nick
	if (message->type == MSG_TEXT || message->type == MSG_AFFIRM_ACK || message->type == MSG_WR_FORM || message->type == MSG_WR_NAME) {
		// printf("message head %s\n",message_head->to_nick);

		struct Node *client_node;
		struct Node *dummy;
		client_node = find_client(clients,message->to_nick,&dummy);

		// om den ikke fantes adder vi den
		if (!client_node) add_client(clients,message->to_nick,NULL,0,0);
		client_node = find_client(clients,message->to_nick,&dummy);

		struct Client *client;
		client = (struct Client*) client_node->ptr;
		if (message->type == MSG_TEXT) {
			*append(client->messages) = message;
		}
		else {
			*prepend(&client->messages) = message;
		}
	}

	else if (message->type == MSG_REGISTER) {
		struct Node *reg_node;
		struct Node *dummy;
		reg_node = find_client(clients,"reg_client",&dummy);

		if (!reg_node) {
			fprintf(stderr,"Error: Register client is not in list of possible recipients. Aborting.\n");
			exit(EXIT_FAILURE);
		}

		struct Client *reg_client;
		reg_client = (struct Client*) reg_node->ptr;
		*append(reg_client->messages) = message;
	}
}

// utfoerer stop and wait logic per client ved hjelp av send_active_message
// og prepare_next_message
int client_send(struct Client *client,struct sockaddr_in *serv_addr, int sock_fd, unsigned long timeout_ticks) {
	int ret;

	client->pkt_wait++;

	if (client->pkt_wait > timeout_ticks) {
		// om denne er tom maa vi sjekke hva vi skal gjoere med neste
		// melding
		if (!client->active_msg) {

			/* Denne funksjonen gjoer et par ting:
			 * Den ser paa neste melding i meldingskoen. Om det er
			 * noe som skal til serveren setter den activemsg til det,
			 * ettersom vi vet hvor det skal. Ellers vil den sjekke om
			 * vi vet om denne nicken sin addresse, og dersom, sette
			 * activemsg til denne med ipen. Om ikke vil den sette
			 * activemsg til en lookupmelding og la meldingen som
			 * laa som neste i meldingskoen bli vaerende. Den
			 * vil sette looked_up til 1 paa denne meldingen.
			 * Naar denne meldingen forsoekes sendt neste gang
			 * vil den enten ha en ip-addresse, eller den vil ha
			 * looked_up == 1 og vil bli fjernet av koen.
			 */
			client->active_msg = prepare_next_message(&(client->messages),serv_addr,client);

		}
		else {
			client->pkt_wait = timeout_ticks+1;
		}

		ret = send_active_msg(client->active_msg,sock_fd,&(client->messages));

		int server_down;
		server_down = 0;

		switch (ret) {
			case ACTIVE_MSG_SENT: ;
				// meldingen ble sendt, vi maa naa vente paa ack
				// eller timeout
				client->pkt_wait = 0;
				break;
			case ACTIVE_MSG_EXPIRED: ;
				client->active_msg = NULL;
				// forrige melding expired, send neste saa fort
				// som mulig
				client->pkt_wait = timeout_ticks+1;
				break;
			case ACTIVE_NO_MSG: ;
				// ingen melding i koeen, sjekke neste gang saa fort som
				// mulig
				client->pkt_wait = timeout_ticks+1;
				break;
			case ACTIVE_SERV_NO_RESPONSE: ;
				client->active_msg = NULL;
				// critical failure: server nede
				server_down = 1;
				break;
		}
		if (server_down) return -1;
	}
	return 0;
}

// tar i mot et pakkenummer og fjerner fra klientlistene dersom den
// passer med pakkenummeret til noen av disse sine in flight meldinger
void remove_pkt(unsigned char incoming_nr, struct Node *clients) {
	struct Node *cur_node;
	cur_node = clients;

	while (cur_node && cur_node->ptr) {
		struct Client *cur_client;
		cur_client = (struct Client*) cur_node->ptr;

		if (cur_client->active_msg) {
			if (cur_client->active_msg->number == incoming_nr) {
				// vi fjerner det som ligger i meldingskoen
				cur_client->pkt_wait = LONG_MAX;
				free(cur_client->active_msg);
				cur_client->active_msg = NULL;
				break;
			}
		}
		cur_node = cur_node->next;
	}
}
