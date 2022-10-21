#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <time.h>

#include "send_packet.h"
#include "upush_helper.h"

char *handle_message(char *buf,struct Node *clients,struct sockaddr_in *src_addr,unsigned long client_max_ticks);

int main(int argc, char *argv[]) {
	int ret,rc;
	if (argc != 3) {
		printf("usage: ./upush_server <port> <tapssansynlighet>\n");
		return EXIT_SUCCESS;
	}

	unsigned short port;

	port = str_to_port(argv[1]);

	float loss_percentage;

	ret = str_to_float(argv[2],&loss_percentage);
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

	int sock_fd;

	sock_fd = create_and_bind_sock(port);

	char buf[BUFSIZE];

	// --------------- LOOP SETUP -------------------

	struct Node* clients = initialize();

	/* Serveren har ogsaa en select loop for aa motta en QUIT melding.
	 * Dette staar ikke i oppgaven, men hvordan ellers skal man sjekke
	 * valgrind paa servern?
	 */

	// hvor lang en tick er i mikrosekunder
	unsigned long ticklen;
	// 16.66 ms, aka 60 fps (det er nok ikke akkuratt 60 fps fordi hver
	// operasjon mellom ticksene bruker tid)
	ticklen = 16666;
	unsigned long us_to_live;

	us_to_live = 30*ONE_MILLION;

	unsigned long client_max_ticks;
	client_max_ticks = us_to_live/ticklen;

	printf("Server initialized, starting main loop.\n\nWrite QUIT to exit.\n");
	// --------------- SELECT LOOP -------------------
	while (1) {
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(sock_fd,&fds);
		FD_SET(STDIN_FILENO,&fds); // STDIN_FILENO == 0

		struct timeval tick;
		tick.tv_sec = 0;
		tick.tv_usec = ticklen;
		// tick.tv_usec = 1000000;
		ret = select(FD_SETSIZE,&fds,NULL,NULL,&tick);
		check_error(ret,"select");

		// vi har faatt noe fra nettet
		if (FD_ISSET(sock_fd,&fds)) {
			struct sockaddr_in* src_addr;
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

			if (VERBOSE) printf("RECIEVED: %s\n",buf);
			char *payload;
			payload = handle_message(buf,clients,src_addr,client_max_ticks);

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
			free(payload);
			free(src_addr);
		}

		// vi har skrevet noe i terminalen
		else if (FD_ISSET(STDIN_FILENO,&fds)) {
			read_from_stdin(buf,BUFSIZE);
			if (!strcmp(buf,"QUIT")) break;
		}

		// ticklen har gaatt
		else {
			// ------------------- CLEARING OF OLD CLIENTS ----------------
			struct Node *prev;
			prev = NULL;

			struct Node *cur;
			cur = clients;
			while (cur) {
				// vi breaker fordi dette skjer bare om head er den eneste
				// i listen om ptr er null
				if (cur->ptr == NULL) break;

				struct Client *client;
				client = (struct Client*) cur->ptr;

				client->ticks_to_live--;

				if (client->ticks_to_live < 0) {
					if (VERBOSE) printf("Removed client with nick: %s\n",client->nick);
					free_client_ptrs(client);
					if (!prev) {
						rm_first(&clients);
						break;
					}
					else {
						rm_element(cur,prev);
						break;
					}
				}

				prev = cur;
				cur = cur->next;
				// print_list(clients);
			}
		}
	}


	// ------------ CLEANUP ------------------
	// vi kommer hit fordi vi skrev QUIT

	// vi freer alle nodesene i clients
	while(clients->ptr) {
		free_client_ptrs((struct Client*) clients->ptr);
		rm_first(&clients);
	}
	free(clients);

	close(sock_fd);
	return EXIT_SUCCESS;
}

// tar i mot en melding og returnerer en svarmelding, legger nick til i klients
char *handle_message(char *buf,struct Node *clients,struct sockaddr_in *src_addr,unsigned long client_max_ticks) {
	int ret;
	struct Message *incoming;

	struct Message *response;

	response = malloc(sizeof(struct Message));

	if (!response) {
		fprintf(stderr,"Malloc failed, aborting.\n");
		exit(EXIT_FAILURE);
	}

	ret = deconstruct_message(buf,&incoming);

	switch (ret) {
		case DECON_SUCCESS: ;
			response->number = incoming->number;

			switch (incoming->type) {
				case MSG_REGISTER: ;
					struct sockaddr_in *cpy_addr;
					cpy_addr = malloc(sizeof(struct sockaddr_in));

					if (!cpy_addr) {
						fprintf(stderr,"Malloc failed, aborting.\n");
						exit(EXIT_FAILURE);
					}

					// ingen feilsjekk for memcpy, den kan ikke feile
					// uten at programmet kraesjer
					memcpy(cpy_addr,src_addr,sizeof(struct sockaddr_in));
					add_client(clients,incoming->from_nick,cpy_addr,client_max_ticks,0);
					// vi sender en ACK nummer OK tilbake
					response->type = MSG_AFFIRM_ACK;

					break;

				case MSG_LOOKUP: ;
					struct Node *dummy; // vi trenger ikke prev her
					struct Node *node;

					// vi finner en node med nicken vi fikk av klienten
					node = find_client(clients,incoming->to_nick,&dummy);
					if (node == NULL) {
						response->type = MSG_LOOKUP_FAIL;
					}
					else {
						response->type = MSG_LOOKUP_OK;
						struct Client *correct_client;
						correct_client = (struct Client*) node->ptr;

						struct sockaddr_in *correct_addr;
						correct_addr = (struct sockaddr_in*) correct_client->addr;

						const char *ret_ptr;
						ret_ptr = inet_ntop(AF_INET,&correct_addr->sin_addr.s_addr,response->ip,16);
						if (!ret_ptr) {
							check_error(-1,"inet_ntop");
						}

						response->port = ntohs(correct_addr->sin_port);
						strcpy(response->to_nick,incoming->to_nick);
					}

					break;

				default:
					// ingen respons til noen andre typer meldinger
					free(incoming);
					return NULL;

					break;
			}
			break; // end case DECON_SUCCESS

		case DECON_WR_FORM: ;
			// feilformatert melding
			response->number = incoming->number;
			response->type = MSG_WR_FORM;
			break; // end case DECON_WR_FORM
		case DECON_CRIT_FAIL: ;
			if (VERBOSE) printf("Recieved critically illformed message. No response will be sent.\n");
			// for feilformatert, ikke svar
			free(incoming);
			return NULL;
	}

	free(incoming);

	char *response_msg = construct_message(response);
	free(response);

	// denne maa free'es!!
	return response_msg;
}
