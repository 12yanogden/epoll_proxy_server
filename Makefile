# Makefile for Proxy Lab 
#
# You may modify this file any way you like (except for the handin
# rule). You instructor will type "make" on your specific Makefile to
# build your proxy from sources.

CC = g++
CFLAGS = -g -Wall
LDFLAGS = -lpthread

all: proxy

proxy: proxy.c
	$(CC) $(CFLAGS) proxy.cpp -o proxy
	chmod 775 proxy

# Creates a tarball in ../proxylab-handin.tar that you can then
# hand in. DO NOT MODIFY THIS!
handin:
	(make clean; cd ..; tar cvf $(USER)-proxylab2-handin.tar --exclude tiny --exclude nop-server.py --exclude slow-client.py --exclude proxy --exclude driver.py --exclude port-for-user.pl --exclude free-port.sh --exclude ".*" proxylab2-handout)

clean:
	rm -f *~ *.o proxy core *.tar *.zip *.gzip *.bzip *.gz
	(cd tiny; make clean)
	(cd tiny/cgi-bin; make clean)
