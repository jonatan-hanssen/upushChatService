CFLAGS=-g -Wall -Wextra -std=gnu11
VFLAGS=--track-origins=yes --malloc-fill=0x40 --free-fill=0x23 --leak-check=full
BIN= upush_client upush_server

all: $(BIN)

upush_client: upush_client.o upush_helper.o send_packet.o send_packet.h upush_helper.h
	gcc $(CFLAGS) upush_client.o upush_helper.o send_packet.o -o $@

upush_server: upush_server.o upush_helper.o send_packet.o send_packet.h upush_helper.h
	gcc $(CFLAGS) upush_server.o upush_helper.o send_packet.o -o $@

%.o: %.c %.h
	gcc $(CFLAGS) $^ -c

# disse brukes for enkel testing med valgrind
val_client_moe: upush_client
	valgrind $(VFLAGS) ./upush_client moe 127.0.0.1 2020 3 0

val_client_joe: upush_client
	valgrind $(VFLAGS) ./upush_client joe 127.0.0.1 2020 3 0

val_client_roe: upush_client
	valgrind $(VFLAGS) ./upush_client roe 127.0.0.1 2020 3 0

val_server: upush_server
	valgrind $(VFLAGS) ./upush_server 2020 0

# samme som over men med 20 loss rate
val_client_moe_lossy: upush_client
	valgrind $(VFLAGS) ./upush_client moe 127.0.0.1 2020 2 20

val_client_joe_lossy: upush_client
	valgrind $(VFLAGS) ./upush_client joe 127.0.0.1 2020 2 20

val_client_roe_lossy: upush_client
	valgrind $(VFLAGS) ./upush_client roe 127.0.0.1 2020 2 20

val_server_lossy: upush_server
	valgrind $(VFLAGS) ./upush_server 2020 20

clean:
	rm -f *.o
	rm -f $(BIN)
	rm -f *.h.gch
	rm -f vgcore.*
