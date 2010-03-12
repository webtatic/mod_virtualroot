# Makefile for mod_virtualroot.c
APXS=$(shell which apxs) 

default: mod_virtualroot.la mod_virtualroot_worker.la
	@echo make done
	@echo type \"make install\" to install mod_virtualroot

mod_virtualroot.la: mod_virtualroot.c
	$(APXS) -c -o $@ mod_virtualroot.c

mod_virtualroot_worker.la: mod_virtualroot.c
	$(APXS) -c -o $@ -D USE_ITHREADS mod_virtualroot.c

mod_virtualroot.c:

install: mod_virtualroot.la mod_virtualroot_worker.la
	$(APXS) -i -n mod_virtualroot.so mod_virtualroot.la
	$(APXS) -i -n mod_virtualroot_worker.so mod_virtualroot_worker.la

clean:
	rm -rf *~ *.o *.so *.lo *.la *.slo *.loT .libs/ 

dist:
	mkdir mod_virtualroot-0.3
	cp README Makefile virtualroot.conf mod_virtualroot.c mod_virtualroot-0.3
	tar czf mod_virtualroot-0.3.tgz mod_virtualroot-0.3/
	rm -rf mod_virtualroot-0.3/	